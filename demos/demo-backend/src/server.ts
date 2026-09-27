/**
 * demo-backend — plays the role of a robot company's EXISTING backend
 * (deliberately TypeScript/Node: the stack most customers have).
 *
 * DEMO RULE (docs/02): integrates with Fjarr exclusively through the
 * ADR-0015 contract — minting session grants, receiving webhooks, calling
 * the control-plane REST API of the fjarr-server sidecar running BESIDE it.
 * It imports no Fjarr code. "Keep your backend, add two endpoints" — plus, since slice 4.5f,
 * the optional operator API for `fjarr-connect` (docs/09#operator-api): a few more, all thin.
 *
 * M1 STATUS: real HS256 grant signing, and webhook receipt with HMAC signature
 * verification (slice 7 / the M1 gate review). Per-tenant keys arrive at M5.
 * spec: docs/09-interfaces.md#2-backend-tier--the-integration-contract-adr-0015
 */
import { createHmac, randomBytes, timingSafeEqual } from "node:crypto";
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

// ---------------------------------------------------------------- the operator API (docs/09#operator-api)
//
// The optional third piece of the contract, for `fjarr-connect` (docs/27#discovery): a terminal
// has no logged-in session, so the customer's backend hands it one. Everything here is the
// company's own idea of who a user is — the demo's stand-in is the role the dashboard picked —
// and none of it touches fjarr-server: the CLI never holds the control-plane token.

/** An operator credential: opaque to Fjarr, minted for a signed-in user, checked on the two reads below. */
interface CliToken {
  role: string;
  operator: { id: string; label: string };
  issued: number;
}
const cliTokens = new Map<string, CliToken>();
const CLI_TOKEN_TTL_MS = 24 * 3600 * 1000;

/** A pending login code for a terminal with no browser (docs/09: create → approve → poll, once). */
interface CliCode {
  code: string;
  pollToken: string;
  created: number;
  credential?: string; // set by the approving dashboard, handed out exactly once
}
const cliCodes = new Map<string, CliCode>(); // by code
const CLI_CODE_TTL_MS = 10 * 60 * 1000;
// Eight characters from an alphabet with no 0/O/1/I: 32^8 = 2^40 guesses, and the PUT is rate limited.
const CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
const randomCode = () => Array.from(randomBytes(8), (b) => CODE_ALPHABET[b % CODE_ALPHABET.length]).join("");
let approveAttempts: number[] = [];
const APPROVE_LIMIT_PER_MINUTE = 10;

function bearer(req: import("node:http").IncomingMessage): CliToken | null {
  const header = req.headers.authorization ?? "";
  const token = header.startsWith("Bearer ") ? header.slice(7).trim() : "";
  const record = cliTokens.get(token);
  if (!record) return null;
  if (Date.now() - record.issued > CLI_TOKEN_TTL_MS) {
    cliTokens.delete(token);
    return null;
  }
  return record;
}

async function jsonBody(req: import("node:http").IncomingMessage): Promise<Record<string, unknown>> {
  let body = "";
  for await (const chunk of req) body += chunk;
  if (!body) return {};
  try {
    return JSON.parse(body) as Record<string, unknown>;
  } catch {
    return {};
  }
}

/** Presence, from the robot.online / robot.offline webhooks this backend already receives (docs/09#b-webhooks). */
function presence(robotId: string): { status: "online" | "offline" | "unknown"; last_seen: number | null } {
  for (let i = received.length - 1; i >= 0; i--) {
    const e = received[i]!;
    const data = e.data as { robot_id?: string } | undefined;
    if (data?.robot_id !== robotId) continue;
    if (e.event === "robot.online") return { status: "online", last_seen: e.ts };
    if (e.event === "robot.offline") return { status: "offline", last_seen: e.ts };
  }
  return { status: "unknown", last_seen: null };
}

const server = createServer(async (req, res) => {
  const url = new URL(req.url ?? "/", `http://localhost:${PORT}`);
  const respond = (status: number, body: unknown) => {
    res.writeHead(status, {
      "content-type": "application/json",
      // Demo-only CORS so the demo-dashboard can call us directly.
      "access-control-allow-origin": "*",
      "access-control-allow-headers": "content-type, authorization",
      "access-control-allow-methods": "GET, POST, PUT, OPTIONS",
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

  // The company's own "who am I" for a CLI: mints an operator credential for the signed-in user.
  // The demo has no users, so the role stands in — a real backend reads its session here.
  if (url.pathname === "/api/me/fjarr-cli-token" && req.method === "POST") {
    const role = url.searchParams.get("role") ?? "operator";
    if (!ROLES[role]) return respond(400, { error: `unknown role ${role} (operator | developer)` });
    const credential = randomBytes(32).toString("base64url");
    cliTokens.set(credential, { role, operator: { id: `${role}@example.com`, label: role === "developer" ? "Demo Developer" : "Demo Operator" }, issued: Date.now() });
    return respond(200, { credential, role, expires_in: CLI_TOKEN_TTL_MS / 1000 });
  }

  // ENDPOINT 3 (optional, docs/09#operator-api): the robots THIS human may reach. Scoped by the
  // caller, which is the property only the customer's backend can provide; fjarr-server knows who
  // is connected but not who is asking.
  if (url.pathname === "/api/fjarr/robots" && req.method === "GET") {
    if (!bearer(req)) return respond(401, { error: "operator credential required (fjarr-connect login)" });
    return respond(
      200,
      robots.map((r) => ({ robot_id: r.id, label: `${r.name} · ${r.site}`, ...presence(r.id) })),
    );
  }

  // …and a grant for one of them: the same JWT, the same code, with what this role may open. The
  // tunnel is as consequential as a shell (docs/10#network-tunnel), so it rides with the terminal
  // on the developer role and nowhere else.
  if (url.pathname === "/api/fjarr/grants" && req.method === "POST") {
    const who = bearer(req);
    if (!who) return respond(401, { error: "operator credential required (fjarr-connect login)" });
    const body = await jsonBody(req);
    const robotId = typeof body.robot_id === "string" ? body.robot_id : "";
    if (!robots.some((r) => r.id === robotId)) return respond(404, { error: `unknown robot ${robotId || "(none)"}` });
    const names = [...ROLES[who.role]!, ...(who.role === "developer" ? ["fjarr.net"] : [])];
    const capabilities = names.map((name) => ({ name }));
    return respond(200, { grant: mintGrant(robotId, who.operator, capabilities), robot_id: robotId, capabilities });
  }

  // The headless login (docs/09#operator-api, docs/27#logging-in): a terminal with no browser
  // creates a code, a human approves it in the dashboard, the terminal polls with a separate token.
  if (url.pathname === "/api/fjarr/cli-codes" && req.method === "POST") {
    for (const [code, c] of cliCodes) if (Date.now() - c.created > CLI_CODE_TTL_MS) cliCodes.delete(code);
    const entry: CliCode = { code: randomCode(), pollToken: randomBytes(24).toString("base64url"), created: Date.now() };
    cliCodes.set(entry.code, entry);
    return respond(200, { code: entry.code, poll_token: entry.pollToken, expires_in: CLI_CODE_TTL_MS / 1000 });
  }
  const approve = url.pathname.match(/^\/api\/fjarr\/cli-codes\/([A-Z2-9]{8})$/);
  if (approve && req.method === "PUT") {
    const now = Date.now();
    approveAttempts = approveAttempts.filter((t) => now - t < 60_000);
    if (approveAttempts.length >= APPROVE_LIMIT_PER_MINUTE) return respond(429, { error: "too many approvals; wait a minute" });
    approveAttempts.push(now);
    const entry = cliCodes.get(approve[1]!);
    if (!entry || now - entry.created > CLI_CODE_TTL_MS) return respond(404, { error: "unknown or expired code" });
    if (entry.credential) return respond(409, { error: "already approved" });
    const body = await jsonBody(req);
    const credential = typeof body.credential === "string" ? body.credential : "";
    // The credential must be one this backend minted: the page got it from /api/me/fjarr-cli-token.
    if (!cliTokens.has(credential)) return respond(401, { error: "the credential is not one this backend issued" });
    entry.credential = credential;
    return respond(200, { approved: true });
  }
  const poll = url.pathname.match(/^\/api\/fjarr\/cli-codes\/([A-Za-z0-9_-]{20,})$/);
  if (poll && req.method === "GET") {
    const entry = [...cliCodes.values()].find((c) => c.pollToken === poll[1]);
    if (!entry) return respond(200, { status: "expired" });
    if (Date.now() - entry.created > CLI_CODE_TTL_MS) {
      cliCodes.delete(entry.code);
      return respond(200, { status: "expired" });
    }
    if (!entry.credential) return respond(200, { status: "pending" });
    cliCodes.delete(entry.code); // exactly once
    return respond(200, { status: "approved", credential: entry.credential });
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
