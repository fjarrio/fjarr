// Every build renders the docs fresh. Astro's content store caches rendered markdown — including
// the link to Expressive Code's stylesheet, whose name is a hash of the code-block config — and
// does not notice when that config changes. A cached store (a local one, or a CI build cache)
// then ships pages linking a stylesheet the build no longer emits. The full build takes seconds.
// spec: docs/19-website-and-publishing.md#local-workflow
import { rmSync } from "node:fs";

for (const path of ["node_modules/.astro", ".astro/data-store.json"]) {
  rmSync(new URL(`../${path}`, import.meta.url), { recursive: true, force: true });
}
