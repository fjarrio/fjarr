import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig({
  plugins: [react()],
  // Dev server only: the browser lab reaches this page as `demo-dashboard` on the
  // compose network (docs/25); Vite would otherwise answer 403 to that Host header.
  server: { port: 5173, host: true, allowedHosts: true },
});
