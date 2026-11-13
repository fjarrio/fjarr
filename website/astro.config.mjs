// fjarr.io — one Astro app for the landing page (src/pages) and the
// developer docs (Starlight rendering the repo's docs/ tree via a symlinked
// content directory — single source of truth, docs never fork).
// spec: docs/19-website-and-publishing.md  |  ADR-0014
import { defineConfig } from "astro/config";
import starlight from "@astrojs/starlight";
import { remarkMdLinks } from "./plugins/remark-md-links.mjs";
import { remarkMermaid } from "./plugins/remark-mermaid.mjs";
import { remarkHeadingIds } from "./plugins/remark-heading-ids.mjs";
import { gstGrammar } from "./plugins/gst-grammar.mjs";

export default defineConfig({
  site: "https://fjarr.io",
  markdown: {
    remarkPlugins: [remarkMdLinks, remarkMermaid, remarkHeadingIds],
  },
  integrations: [
    starlight({
      title: "Fjarr",
      description:
        "Peer-to-peer remote access for the machines you ship — camera video, remote desktop, terminal, a direct network link, and fleet tooling.",
      social: [{ icon: "github", label: "GitHub", href: "https://github.com/fjarrio/fjarr" }],
      // Look and feel: docs/19-website-and-publishing.md#visual-design
      logo: { src: "./src/assets/fjarr-mark.svg" },
      favicon: "/favicon.svg",
      customCss: ["./src/styles/theme.css", "./src/styles/docs.css"],
      components: {
        Header: "./src/components/overrides/Header.astro",
        ThemeProvider: "./src/components/overrides/ThemeProvider.astro",
        ThemeSelect: "./src/components/overrides/ThemeSelect.astro",
      },
      expressiveCode: {
        themes: ["github-dark-default", "github-light-default"],
        // ```gst: GStreamer pipeline descriptions (docs/19#authoring-rules).
        shiki: { langs: [gstGrammar] },
        styleOverrides: {
          borderRadius: "0.5rem",
          borderColor: "var(--sl-color-hairline-light)",
          codePaddingBlock: "0.9rem",
          codePaddingInline: "1.25rem",
          codeBackground: "var(--fj-code-bg)",
          frames: { shadowColor: "transparent" },
        },
      },
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
            const nodes = [...document.querySelectorAll("pre.mermaid[data-mermaid]")];
            if (nodes.length) {
              // The attribute is the author's source, byte for byte; the element's text has
              // been through the markdown pipeline's whitespace normalisation.
              const reset = () => {
                for (const node of nodes) {
                  const exact = node.getAttribute("data-mermaid");
                  if (exact) node.textContent = exact;
                  node.removeAttribute("data-processed");
                }
              };
              // Diagram colours follow the site tokens (docs/19#visual-design).
              const palette = (light) => light
                ? { darkMode: false, background: "#ffffff", primaryColor: "#e8f1ff", primaryBorderColor: "#1f6feb",
                    primaryTextColor: "#1b2433", secondaryColor: "#f1f5fb", tertiaryColor: "#ffffff",
                    lineColor: "#1f6feb", textColor: "#1b2433", noteBkgColor: "#fff8e6", noteBorderColor: "#d4a72c",
                    edgeLabelBackground: "#ffffff", clusterBkg: "#f6f9fd", clusterBorder: "#c9d6e8" }
                : { darkMode: true, background: "#0b0f17", primaryColor: "#14223b", primaryBorderColor: "#2f81f7",
                    primaryTextColor: "#e6edf3", secondaryColor: "#161f2e", tertiaryColor: "#0f1622",
                    lineColor: "#58a6ff", textColor: "#c9d4e3", noteBkgColor: "#1f2a1a", noteBorderColor: "#d29922",
                    edgeLabelBackground: "#0b0f17", clusterBkg: "#0e1420", clusterBorder: "#253047" };
              const { default: mermaid } = await import("https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.esm.min.mjs");
              const render = async () => {
                const light = document.documentElement.dataset.theme === "light";
                mermaid.initialize({ startOnLoad: false, theme: "base", themeVariables: palette(light) });
                reset();
                try {
                  await mermaid.run({ nodes });
                } catch (e) {
                  // One bad diagram should not take the rest of the page's diagrams
                  // with it; mermaid draws its own error box in the element it failed on.
                  console.error("mermaid:", e);
                }
              };
              await render();
              document.addEventListener("fjarr:theme", render);
            }
          `,
        },
      ],
    }),
  ],
});
