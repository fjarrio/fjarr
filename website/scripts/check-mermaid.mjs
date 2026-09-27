/**
 * Parse every ```mermaid fence in the docs tree with mermaid itself, and fail with
 * the file, the line and mermaid's own message.
 *
 * A gate rather than a convenience: a diagram that does not parse is invisible on
 * fjarr.io — the reader gets "Syntax error in text" where the architecture should
 * be — and nothing else in the pipeline notices. Both defects this was written for
 * were silent. One was a `;` inside a `Note over` line, which mermaid reads as a
 * statement separator. The other was the site scraping highlighted markup for its
 * source and losing every line break, which `remark-mermaid.mjs` now prevents; the
 * extraction half is checked by the lab (web/e2e/tests/website/mermaid.spec.ts),
 * because only a browser can tell whether a diagram actually drew.
 *
 * Mermaid is a browser library, so it gets the small DOM it insists on from jsdom.
 * No network and no browser: this runs in the docs job beside the markdown linters.
 *
 * spec: docs/19-website-and-publishing.md#diagrams
 */
import { readFile, readdir } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { JSDOM } from "jsdom";

const here = path.dirname(fileURLToPath(import.meta.url));
const docsDir = process.argv[2] ?? path.resolve(here, "../../docs");

/** Every fenced mermaid block under `dir`, with the line its fence opened on. */
async function collect(dir) {
  const blocks = [];
  for (const entry of await readdir(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      blocks.push(...(await collect(full)));
      continue;
    }
    if (!entry.name.endsWith(".md")) continue;
    const lines = (await readFile(full, "utf8")).split("\n");
    let open = -1;
    for (let i = 0; i < lines.length; i++) {
      if (open < 0 && lines[i].trim() === "```mermaid") open = i;
      else if (open >= 0 && lines[i].trim() === "```") {
        blocks.push({
          file: path.relative(process.cwd(), full),
          line: open + 1,
          source: lines.slice(open + 1, i).join("\n"),
        });
        open = -1;
      }
    }
    if (open >= 0) {
      blocks.push({ file: path.relative(process.cwd(), full), line: open + 1, source: null });
    }
  }
  return blocks;
}

const dom = new JSDOM("<!doctype html><html><body></body></html>", { pretendToBeVisual: true });
globalThis.window = dom.window;
globalThis.document = dom.window.document;
for (const name of ["Element", "HTMLElement", "SVGElement", "DOMParser", "Node", "getComputedStyle", "MutationObserver"]) {
  if (!(name in globalThis)) globalThis[name] = dom.window[name];
}
const { default: mermaid } = await import("mermaid");
mermaid.initialize({ startOnLoad: false });

const blocks = await collect(docsDir);
let failed = 0;
for (const block of blocks) {
  const where = `${block.file}:${block.line}`;
  if (block.source === null) {
    console.error(`${where}  unterminated \`\`\`mermaid fence`);
    failed++;
    continue;
  }
  try {
    await mermaid.parse(block.source);
    console.log(`ok    ${where}`);
  } catch (error) {
    failed++;
    const message = String(error?.message ?? error)
      .split("\n")
      .map((line) => `        ${line}`)
      .join("\n");
    console.error(`FAIL  ${where}\n${message}`);
  }
}
// Phase two: the built site, when there is one. Parsing proves the author wrote valid
// mermaid; this proves the page hands mermaid what the author wrote. The bug it exists for
// lost every line break — Expressive Code emits one div per line and no newline text nodes,
// so scraping the highlighted markup produced a one-line diagram and "Syntax error in text"
// on a source that parses perfectly. A dropped element is caught here too: the first attempt
// at the fix emitted a raw `html` node, which the pipeline discarded without a word.
const distDir = process.argv[3] ?? path.resolve(here, "../dist");
let checkedDist = false;
try {
  const pages = [];
  const walk = async (dir) => {
    for (const entry of await readdir(dir, { withFileTypes: true })) {
      const full = path.join(dir, entry.name);
      if (entry.isDirectory()) await walk(full);
      else if (entry.name.endsWith(".html")) pages.push(full);
    }
  };
  await walk(distDir);
  checkedDist = true;

  const rendered = new Map(); // source text -> where it was found
  let highlighted = 0;
  for (const page of pages) {
    const html = await readFile(page, "utf8");
    if (!html.includes('class="mermaid"') && !html.includes('data-language="mermaid"')) continue;
    const where = path.relative(process.cwd(), page);
    // A highlighted mermaid block in the output means the fence reached the code renderer.
    const doc = new JSDOM(html).window.document;
    highlighted += doc.querySelectorAll('[data-language="mermaid"], code.language-mermaid').length;
    for (const el of doc.querySelectorAll("pre.mermaid, div.mermaid")) {
      // The attribute, because that is what the client feeds mermaid.
      rendered.set((el.getAttribute("data-mermaid") ?? el.textContent ?? "").trim(), where);
    }
  }
  if (highlighted > 0) {
    console.error(
      `FAIL  ${highlighted} mermaid fence(s) reached the code highlighter in the built site — ` +
        "remark-mermaid should have replaced them, and scraping highlighted markup loses every line break",
    );
    failed++;
  }
  for (const block of blocks) {
    if (block.source === null) continue;
    const want = block.source.trim();
    const where = rendered.get(want);
    if (where) {
      console.log(`built ${block.file}:${block.line} -> ${where}`);
      continue;
    }
    failed++;
    // Say which way it is wrong: missing entirely, or present with its lines collapsed.
    const collapsed = [...rendered.keys()].find(
      (text) => text.replace(/\s+/g, " ") === want.replace(/\s+/g, " ") && text !== want,
    );
    console.error(
      collapsed !== undefined
        ? `FAIL  ${block.file}:${block.line} is in the built site with its line breaks lost ` +
            `(${collapsed.split("\n").length} line(s) instead of ${want.split("\n").length}) — mermaid cannot parse that`
        : `FAIL  ${block.file}:${block.line} is not in the built site as a div.mermaid at all`,
    );
  }
} catch (error) {
  if (error?.code === "ENOENT") {
    console.log(`mermaid: no built site at ${distDir} — parse checked only (run the website build first)`);
  } else {
    throw error;
  }
}

console.log(
  `mermaid: ${blocks.length} diagram(s)${checkedDist ? ", parsed and found in the built site" : ", parsed"}, ${failed} failing`,
);
process.exit(failed ? 1 : 0);
