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
  /** The demo backend as the BROWSER reaches it (the dashboard fetches grants from it). */
  dashboardBackend: process.env.E2E_DASHBOARD_BACKEND ?? "http://demo-backend:9090",
  /** Dev hooks shared with compose (.env / docs/09). */
  grantSecret: process.env.FJARR_GRANT_HS256_SECRET ?? "dev-only-grant-secret",
  deviceToken: process.env.FJARR_DEV_DEVICE_TOKEN ?? "dev-only-device-token",
  /** The real robot's id (the C++ agent in `demo-robot`, or a locally started fjarr-agent). */
  robotId: process.env.E2E_ROBOT_ID ?? "demo-robot-01",
  /**
   * The agent's docs/24 endpoint as the harness reaches it: the demo profile exposes it on the compose
   * network (slice 5b), CI on the runner's loopback. Empty = `curl` inside the robot container.
   */
  introspectHttp: process.env.E2E_INTROSPECT_HTTP ?? "http://demo-robot:7381",
  /** `introspect.token` — the demo's dev token (docs/24), the compose default. */
  introspectToken: process.env.E2E_INTROSPECT_TOKEN ?? process.env.FJARR_INTROSPECT_TOKEN ?? "dev-only-introspect-token",
  /** The viewer served by that endpoint, as the BROWSER reaches it. */
  viewerUrl: process.env.E2E_VIEWER_URL ?? "http://demo-robot:7381/",
  /** Compose service that carries the media path (netem, fault switches). */
  robotService: process.env.E2E_ROBOT_SERVICE ?? "demo-robot",
  /** Where every run's artifacts go (`out/<test>/`). */
  outRoot: process.env.E2E_OUT ?? fileURLToPath(new URL("../out/", import.meta.url)),
};
