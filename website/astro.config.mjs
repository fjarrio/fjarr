// fjarr.io — one Astro app for the landing page (src/pages) and the
// developer docs (Starlight rendering the repo's docs/ tree via a symlinked
// content directory — single source of truth, docs never fork).
// spec: docs/19-website-and-publishing.md  |  ADR-0014
import { defineConfig } from "astro/config";
import starlight from "@astrojs/starlight";
import { remarkMdLinks } from "./plugins/remark-md-links.mjs";

export default defineConfig({
  site: "https://fjarr.io",
  markdown: {
    remarkPlugins: [remarkMdLinks],
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
      // Client-side mermaid rendering for ```mermaid fences (docs/19).
      head: [
        {
          tag: "script",
          attrs: { type: "module" },
          content: `
            const blocks = () => document.querySelectorAll("pre[data-language='mermaid'], code.language-mermaid");
            if (blocks().length) {
              const { default: mermaid } = await import("https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.esm.min.mjs");
              mermaid.initialize({ startOnLoad: false, theme: "dark" });
              for (const el of blocks()) {
                const src = el.textContent ?? "";
                const holder = document.createElement("div");
                holder.className = "mermaid";
                holder.textContent = src;
                (el.closest("pre") ?? el).replaceWith(holder);
              }
              mermaid.run();
            }
          `,
        },
      ],
    }),
  ],
});
