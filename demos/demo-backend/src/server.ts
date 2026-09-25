/**
 * demo-backend — plays the role of a robot company's EXISTING backend
 * (deliberately TypeScript/Node: the stack most customers have).
 *
 * DEMO RULE (docs/02): integrates with Fjarr exclusively through the
 * ADR-0015 contract — minting session grants, receiving webhooks, calling
 * the control-plane REST API of the fjarr-server sidecar running BESIDE it.
 * It imports no Fjarr code. "Keep your backend, add two endpoints."
 *
 * M1 STATUS: real HS256 grant signing, and webhook receipt with HMAC signature
 * verification (slice 7 / the M1 gate review). Per-tenant keys arrive at M5.
 * spec: docs/09-interfaces.md#2-backend-tier--the-integration-contract-adr-0015
 */
import { createHmac, timingSafeEqual } from "node:crypto";
import { createServer } from "node:http";

const PORT = Number(process.env.PORT ?? 9090);
const FJARR_SERVER_URL = process.env.FJARR_SERVER_URL ?? "http://localhost:8080";
// The grant-signing secret registered in fjarr-server (docs/09#a-session-grants).
// Slice 3b: HS256 with the shared dev secret; per-tenant keys arrive at M5.
const GRANT_SECRET = process.env.FJARR_GRANT_HS256_SECRET ?? "dev-only-grant-secret";
// The webhook secret the sidecar signs with (docs/09#b-webhooks). Same shared-secret
// story as the grant key: per-tenant keys arrive at M5.
const WEBHOOK_SECRET = process.env.FJARR_WEBHOOK_SECRET ?? "dev-only-webhook-secret";

/** What arrived, newest last, bounded — a customer backend would write these to its own store. */
interface ReceivedWebhook {
  event: string;
  event_id: string;
  ts: number;
  data: unknown;
}
const received: ReceivedWebhook[] = [];
const RECEIVED_MAX = 200;

/**
 * A webhook receiver that does not verify its signature is an open endpoint that
 * anyone may post session events to (docs/10). The demo is the reference
 * integration, so it verifies — constant-time, over the raw body.
 */
function signatureValid(raw: string, header: string | undefined): boolean {
  if (!header) return false;
  const expected = `sha256=${createHmac("sha256", WEBHOOK_SECRET).update(raw).digest("hex")}`;
  const a = Buffer.from(header);
  const b = Buffer.from(expected);
  return a.length === b.length && timingSafeEqual(a, b);
}

const b64url = (s: string | Buffer) => Buffer.from(s).toString("base64url");
function mintGrant(robotId: string, operator: { id: string; label: string }, capabilities: Array<{ name: string; params?: Record<string, unknown> }>) {
  const header = b64url(JSON.stringify({ alg: "HS256", typ: "JWT" }));
  const claims = b64url(
    JSON.stringify({
      iss: "demo-backend",
      aud: "fjarr",
      exp: Math.floor(Date.now() / 1000) + 300, // ≤ 5 min to START a session (docs/09)
      tenant: "demo",
      robot_id: robotId,
      operator,
      capabilities,
    }),
  );
  const sig = createHmac("sha256", GRANT_SECRET).update(`${header}.${claims}`).digest("base64url");
  return `${header}.${claims}.${sig}`;
}

/** The company's own roles → the Fjarr capabilities each may open (a demo-only convention, docs/09). */
// A shell is a different risk class from watching a camera (docs/10#terminal), so only the
// developer role carries it — the demo's stand-in for "their auth decides".
const ROLES: Record<string, string[]> = {
  operator: ["fjarr.test", "fjarr.camera"],
  developer: ["fjarr.test", "fjarr.camera", "fjarr.introspect", "fjarr.terminal"],
};

/** The company's own robot registry — their data, not Fjarr's. */
const robots = [
  { id: "demo-robot-01", name: "Demo Robot 01", site: "Lab" },
  { id: "demo-robot-02", name: "Demo Robot 02", site: "Warehouse" },
  { id: "demo-robot-03", name: "Demo Robot 03", site: "Yard" },
];

const server = createServer(async (req, res) => {
  const url = new URL(req.url ?? "/", `http://localhost:${PORT}`);
  const respond = (status: number, body: unknown) => {
    res.writeHead(status, {
      "content-type": "application/json",
      // Demo-only CORS so the demo-dashboard can call us directly.
      "access-control-allow-origin": "*",
      "access-control-allow-headers": "content-type",
    });
    res.end(JSON.stringify(body, null, 2));
  };

  if (req.method === "OPTIONS") return respond(204, {});

  // The company's own API…
  if (url.pathname === "/api/robots") return respond(200, robots);
  if (url.pathname === "/healthz") return respond(200, { ok: true });

  // …plus ENDPOINT 1 of the Fjarr contract: mint a session grant — a real
  // HS256 JWT the sidecar verifies (docs/09#a-session-grants). The company's
  // own auth decides the operator and the capabilities; the demo stands in
  // for it with a ROLE the dashboard picks: an operator gets the robot's
  // media capabilities, a developer additionally `fjarr.introspect`
  // (docs/24) — nobody gets everything by default.
  if (url.pathname === "/api/fjarr/grant" && req.method === "POST") {
    const robotId = url.searchParams.get("robot") ?? "demo-robot-01";
    if (!robots.some((r) => r.id === robotId)) return respond(404, { error: `unknown robot ${robotId}` });
    const role = url.searchParams.get("role") ?? "operator";
    const grants = ROLES[role];
    if (!grants) return respond(400, { error: `unknown role ${role} (operator | developer)` });
    const operator = { id: `${role}@example.com`, label: role === "developer" ? "Demo Developer" : "Demo Operator" };
    const capabilities = grants.map((name) => ({ name }));
    return respond(200, { grant: mintGrant(robotId, operator, capabilities), robot_id: robotId, role, capabilities });
  }

  // …and ENDPOINT 2: the webhook receiver (docs/09#b-webhooks).
  if (url.pathname === "/api/fjarr/webhook" && req.method === "POST") {
    let body = "";
    for await (const chunk of req) body += chunk;
    if (!signatureValid(body, req.headers["x-fjarr-signature"] as string | undefined)) {
      console.warn(`[demo-backend] webhook REJECTED: bad or missing x-fjarr-signature`);
      return respond(401, { error: "bad signature" });
    }
    const event = JSON.parse(body) as ReceivedWebhook;
    // At-least-once delivery (docs/09): the same event_id may arrive twice.
    if (!received.some((e) => e.event_id === event.event_id)) {
      received.push(event);
      if (received.length > RECEIVED_MAX) received.shift();
    }
    console.log(`[demo-backend] webhook ${event.event} ${event.event_id} ${JSON.stringify(event.data).slice(0, 160)}`);
    return respond(200, { received: true });
  }

  // Not part of the contract: the demo's own window onto what it has received, so the
  // dashboard and the e2e suite can see that the webhook half of ADR-0015 really runs.
  if (url.pathname === "/api/fjarr/webhooks" && req.method === "GET") {
    return respond(200, { events: received });
  }

  return respond(404, { error: "not found" });
});

server.listen(PORT, () => {
  console.log(`[demo-backend] listening on :${PORT}`);
  console.log(`[demo-backend] fjarr-server sidecar expected at ${FJARR_SERVER_URL}`);
});
