/**
 * Explicit heading ids: `## The envelope {#envelope}` becomes `<h2 id="envelope">The envelope</h2>`.
 * spec: docs/19-website-and-publishing.md#authoring-rules
 *
 * The docs give their stable sections an id, and link to it (`08-protocol.md#envelope`). Without
 * this the site showed "{#envelope}" in the heading and the table of contents, and gave the
 * heading a slug of its whole text (`the-envelope-envelope`), so every such link missed.
 * Astro keeps an id that is already set, so the table of contents links the same id.
 */
import { visit } from "unist-util-visit";

// Astro's smartypants has already run, turning an id's `--` into a dash; ids in the docs only
// ever use `--`, never `---`, so either dash maps back to `--`.
const TRAILING_ID = /\s*\{#([A-Za-z0-9_\u2013\u2014-]+)\}\s*$/;

export function remarkHeadingIds() {
  return (tree) => {
    visit(tree, "heading", (node) => {
      const last = node.children.at(-1);
      if (!last || last.type !== "text") return;
      const match = last.value.match(TRAILING_ID);
      if (!match) return;
      last.value = last.value.slice(0, match.index);
      if (last.value === "") node.children.pop();
      node.data ??= {};
      const id = match[1].replace(/[\u2013\u2014]/g, "--");
      node.data.hProperties = { ...node.data.hProperties, id };
    });
  };
}
