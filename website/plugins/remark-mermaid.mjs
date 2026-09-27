/**
 * Hand ```mermaid fences to mermaid as raw source, instead of letting the code
 * highlighter render them first (docs/19-website-and-publishing.md#diagrams).
 *
 * Starlight highlights code with Expressive Code, which emits one
 * `<div class="ec-line">` per source line and **no newline text nodes**. Scraping
 * `textContent` off that markup therefore returns the whole diagram on a single
 * line, and every diagram fails with "Syntax error in text" — the source is fine,
 * the extraction was not. Replacing the fence here, before any highlighter sees
 * it, means the browser gets exactly the bytes the author wrote.
 *
 *   ```mermaid          ->  <pre class="mermaid">flowchart LR
 *   flowchart LR                A --&gt; B</pre>
 *     A --> B
 *   ```
 *
 * The client script in astro.config.mjs runs mermaid over `pre.mermaid`.
 */
import { visit } from "unist-util-visit";

export function remarkMermaid() {
  return (tree) => {
    visit(tree, "code", (node, index, parent) => {
      if (node.lang !== "mermaid" || !parent || index === undefined) return;
      // A node carrying `hName`/`hProperties` with a single text child, rather than a
      // raw `html` node: raw HTML depends on the pipeline keeping `rehype-raw` in it,
      // and Astro's dropped the element silently. A text child is escaped on the way
      // out and decoded by `textContent` on the way in, so mermaid sees the author's
      // bytes — including any `<br/>` in a label, which mermaid parses itself.
      parent.children[index] = {
        type: "paragraph",
        data: {
          hName: "pre",
          // The source goes in an attribute as well as in the element's text, and the
          // attribute is what the client reads. Something between here and the HTML strips
          // the leading whitespace of every line in a text node — measured, and it is
          // neither `compressHTML` nor the choice of tag. Flowcharts and sequence diagrams
          // do not care, but a mermaid YAML config header (`---`, `config:`, two-space
          // `theme:`) would, and an attribute is serialised byte for byte.
          hProperties: { class: "mermaid", "data-mermaid": node.value },
        },
        // Kept as text too: it is what a reader without JavaScript sees, and what mermaid
        // falls back to if the attribute ever goes missing.
        children: [{ type: "text", value: node.value }],
      };
    });
  };
}
