/**
 * Rewrite relative `*.md` links from the docs/ tree into Starlight routes,
 * so the same files link correctly on GitHub, in editors, AND on fjarr.io
 * (docs/19-website-and-publishing.md#authoring-rules).
 *
 *   08-protocol.md#envelope  ->  ../08-protocol/#envelope
 *   adr/0006-x.md            ->  ../adr/0006-x/
 */
import { visit } from "unist-util-visit";

export function remarkMdLinks() {
  return (tree) => {
    visit(tree, "link", (node) => {
      const url = node.url ?? "";
      if (/^[a-z]+:\/\//i.test(url) || url.startsWith("/") || url.startsWith("#")) {
        return; // absolute, site-rooted, or in-page — leave alone
      }
      const match = url.match(/^([^#]+)\.md(#.*)?$/i);
      if (!match) return;
      const [, path, anchor = ""] = match;
      // Starlight slugs are lowercased; pages get trailing slashes. Pages
      // live at /docs/<slug>/, so same-dir links need one "../".
      node.url = `../${path.toLowerCase()}/${anchor}`;
    });
  };
}
