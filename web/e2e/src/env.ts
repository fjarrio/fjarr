/**
 * Where things are, from the harness's point of view (docs/25 topology).
 * Defaults are the compose network as seen from `dev`; CI overrides the
 * page origin because it serves the lab page from the runner.
 */
import { fileURLToPath } from "node:url";

export const env = {
  /** CDP endpoint of the lab browser (fetched from the harness). */
  browser: process.env.E2E_BROWSER ?? "http://browser:9222",
  /** Origin the *browser* navigates to for the lab page (served by this package's Vite). */
  pageOrigin: process.env.E2E_PAGE_ORIGIN ?? "http://lab-host:5174",
  /** fjarr-server as the browser reaches it (WebSocket) and as the harness reaches it (HTTP). */
  serverWs: process.env.E2E_SERVER_WS ?? "ws://fjarr-server:8080/ws",
  serverHttp: process.env.E2E_SERVER_HTTP ?? "http://fjarr-server:8080",
  /** The demo dashboard as the browser reaches it, and as the harness probes it. */
  dashboardUrl: process.env.E2E_DASHBOARD_URL ?? "http://demo-dashboard:5173",
  dashboardHttp: process.env.E2E_DASHBOARD_HTTP ?? process.env.E2E_DASHBOARD_URL ?? "http://demo-dashboard:5173",
  /** Dev hooks shared with compose (.env / docs/09). */
  grantSecret: process.env.FJARR_GRANT_HS256_SECRET ?? "dev-only-grant-secret",
  deviceToken: process.env.FJARR_DEV_DEVICE_TOKEN ?? "dev-only-device-token",
  /** Compose service that carries the media path (netem, fault switches). */
  robotService: process.env.E2E_ROBOT_SERVICE ?? "demo-robot",
  /** Where every run's artifacts go (`out/<test>/`). */
  outRoot: process.env.E2E_OUT ?? fileURLToPath(new URL("../out/", import.meta.url)),
};
