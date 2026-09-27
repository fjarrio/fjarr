// fjarr.io — one Astro app for the landing page (src/pages) and the
// developer docs (Starlight rendering the repo's docs/ tree via a symlinked
// content directory — single source of truth, docs never fork).
// spec: docs/19-website-and-publishing.md  |  ADR-0014
import { defineConfig } from "astro/config";
import starlight from "@astrojs/starlight";
import { remarkMdLinks } from "./plugins/remark-md-links.mjs";
import { remarkMermaid } from "./plugins/remark-mermaid.mjs";

export default defineConfig({
  site: "https://fjarr.io",
  markdown: {
    remarkPlugins: [remarkMdLinks, remarkMermaid],
  },
  integrations: [
    starlight({
      title: "Fjarr",
      description:
        "Generic peer-to-peer connectivity for robot fleets — camera video, remote desktop, files, terminal, and fleet tooling over WebRTC.",
      social: [
        // populated at M0.5 when the repo has a public remote
      ],
      sidebar: [
        { label: "Start here", slug: "readme" },
        {
          label: "Product",
          items: ["00-vision", "03-product-strategy", "17-roadmap"],
        },
        {
          label: "Architecture & specs",
          items: [
            "02-architecture",
            "05-extension-model",
            "06-capabilities",
            "07-desktop-backends",
            "08-protocol",
            "09-interfaces",
            "10-security",
            "16-performance-budgets",
            "21-web-client-architecture",
            "22-remote-desktop-client",
            "23-agent-core-architecture",
            "24-pipeline-introspection",
            "25-browser-lab",
            "26-robot-install-and-drivers",
          ],
        },
        {
          label: "Working on Fjarr",
          items: [
            "12-development-environment",
            "13-development-workflow",
            "15-testing-strategy",
            "14-dependencies",
            "11-prior-art",
            "04-supported-platforms",
            "01-glossary",
            "18-open-questions",
            "19-website-and-publishing",
            "20-agentic-development",
          ],
        },
        {
          label: "Decisions (ADRs)",
          items: [{ autogenerate: { directory: "adr" } }],
        },
      ],
      // Client-side mermaid rendering. `remark-mermaid` has already turned every
      // fence into a `div.mermaid` holding the author's own source, so there is
      // nothing to un-highlight here — only a theme to match and a run to make
      // (docs/19#diagrams).
      head: [
        {
          tag: "script",
          attrs: { type: "module" },
          content: `
            const nodes = document.querySelectorAll("pre.mermaid[data-mermaid]");
            if (nodes.length) {
              // The attribute is the author's source, byte for byte; the element's text has
              // been through the markdown pipeline's whitespace normalisation.
              for (const node of nodes) {
                const exact = node.getAttribute("data-mermaid");
                if (exact) node.textContent = exact;
              }
              const { default: mermaid } = await import("https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.esm.min.mjs");
              const light = document.documentElement.dataset.theme === "light";
              mermaid.initialize({ startOnLoad: false, theme: light ? "default" : "dark" });
              try {
                await mermaid.run({ nodes });
              } catch (e) {
                // One bad diagram should not take the rest of the page's diagrams
                // with it; mermaid draws its own error box in the element it failed on.
                console.error("mermaid:", e);
              }
            }
          `,
        },
      ],
    }),
  ],
});
