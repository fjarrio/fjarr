import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// Built to plain static files the agent serves from `introspect.viewer_dir` (docs/24#the-viewer):
// relative asset paths, hashed names under assets/ (the endpoint marks those immutable).
export default defineConfig({
  plugins: [react()],
  base: "./",
  build: { outDir: "dist", emptyOutDir: true, sourcemap: false },
  // Dev server: run beside a locally started agent and point it with ?endpoint=http://127.0.0.1:7381.
  server: { port: 5175, host: true, allowedHosts: true },
});
