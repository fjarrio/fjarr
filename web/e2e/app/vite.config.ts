import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// The lab page (docs/25): served on :5174 to the lab browser; the demo
// dashboard keeps :5173.
export default defineConfig({
  root: new URL(".", import.meta.url).pathname,
  plugins: [react()],
  // Any Host header: the lab browser reaches this server as lab-host (compose)
  // or host.docker.internal (CI); this is test scaffolding, never deployed.
  server: { port: 5174, host: true, strictPort: true, allowedHosts: true },
  // Workspace packages resolve through their `exports` → dist: build core/react first.
  optimizeDeps: { exclude: ["@fjarr/core", "@fjarr/react"] },
});
