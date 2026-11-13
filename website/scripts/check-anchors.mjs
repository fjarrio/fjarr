// Every in-site link with a #fragment, in the built site, lands on an id that exists on its page.
// spec: docs/19-website-and-publishing.md#local-workflow
//
// `make docs-links` checks the markdown, where `## The envelope {#envelope}` is an anchor; this
// checks what readers get. The two disagreed for months: the site rendered "{#envelope}" as text
// and slugged the heading `the-envelope-envelope`, and 361 links missed while lychee passed.
import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, relative, sep } from "node:path";

const dist = new URL("../dist/", import.meta.url).pathname;
const pages = new Map(); // "/08-protocol/" -> html
const walk = (dir) => {
  for (const name of readdirSync(dir)) {
    const path = join(dir, name);
    if (statSync(path).isDirectory()) walk(path);
    else if (name === "index.html") pages.set("/" + relative(dist, dir).split(sep).filter(Boolean).map((p) => p + "/").join(""), readFileSync(path, "utf8"));
  }
};
walk(dist);
if (pages.size === 0) throw new Error(`no pages under ${dist} — build the site first`);

const ids = new Map([...pages].map(([page, html]) => [page, new Set([...html.matchAll(/\sid="([^"]+)"/g)].map((m) => m[1]))]));
const decode = (s) => s.replace(/&amp;/g, "&").replace(/&quot;/g, '"').replace(/&#39;/g, "'");

let checked = 0;
const broken = new Set();
for (const [page, html] of pages) {
  for (const [, raw] of html.matchAll(/<a\s[^>]*href="([^"]+)"/g)) {
    const href = decode(raw);
    if (/^[a-z]+:/i.test(href) || !href.includes("#")) continue;
    const url = new URL(href, "https://site" + page);
    const fragment = decodeURIComponent(url.hash.slice(1));
    if (!fragment) continue;
    const target = url.pathname.endsWith("/") ? url.pathname : url.pathname + "/";
    if (!ids.has(target)) continue; // not a page of this site (or a broken page link: lychee's job)
    checked++;
    if (!ids.get(target).has(fragment)) broken.add(`${page} -> ${target}#${fragment}`);
  }
}
for (const b of [...broken].sort()) console.error(`broken anchor: ${b}`);
console.log(`anchors: ${checked} in-site links with a #fragment, ${broken.size} broken`);
process.exit(broken.size ? 1 : 0);
