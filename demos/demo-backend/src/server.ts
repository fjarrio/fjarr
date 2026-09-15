/**
 * demo-backend — plays the role of a robot company's EXISTING backend
 * (deliberately TypeScript/Node: the stack most customers have).
 *
 * DEMO RULE (docs/02): integrates with Fjarr exclusively through the
 * ADR-0015 contract — minting session grants, receiving webhooks, calling
 * the control-plane REST API of the fjarr-server sidecar running BESIDE it.
 * It imports no Fjarr code. "Keep your backend, add two endpoints."
 *
 * M0 STATUS: the two endpoints exist with stub payloads; real JWT signing
 * and webhook verification land with the M1 contract implementation.
 * spec: docs/09-interfaces.md#2-backend-tier--the-integration-contract-adr-0015
 */
import { createServer } from "node:http";

const PORT = Number(process.env.PORT ?? 9090);
const FJARR_SERVER_URL = process.env.FJARR_SERVER_URL ?? "http://localhost:8080";

/** The company's own robot registry — their data, not Fjarr's. */
const robots = [
  { id: "demo-robot-01", name: "Demo Robot 01", site: "Lab" },
  { id: "demo-robot-02", name: "Demo Robot 02", site: "Warehouse" },
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

  // …plus ENDPOINT 1 of the Fjarr contract: mint a session grant.
  // M1: a real signed JWT (docs/09#a-session-grants); M0: stub shape.
  if (url.pathname === "/api/fjarr/grant" && req.method === "POST") {
    const robotId = url.searchParams.get("robot") ?? "demo-robot-01";
    return respond(200, {
      grant: "UNSIGNED-M0-STUB",
      claims: {
        iss: "demo-backend",
        aud: "fjarr",
        exp: Math.floor(Date.now() / 1000) + 300,
        tenant: "demo",
        robot_id: robotId,
        operator: { id: "demo@example.com", label: "Demo Operator" },
        capabilities: [{ name: "fjarr.camera" }],
      },
    });
  }

  // …and ENDPOINT 2: the webhook receiver (docs/09#b-webhooks).
  if (url.pathname === "/api/fjarr/webhook" && req.method === "POST") {
    let body = "";
    for await (const chunk of req) body += chunk;
    console.log(`[demo-backend] webhook received: ${body.slice(0, 200)}`);
    return respond(200, { received: true });
  }

  return respond(404, { error: "not found" });
});

server.listen(PORT, () => {
  console.log(`[demo-backend] listening on :${PORT}`);
  console.log(`[demo-backend] fjarr-server sidecar expected at ${FJARR_SERVER_URL}`);
});
