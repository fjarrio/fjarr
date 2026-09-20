/**
 * Mint a session grant the way a customer backend does (docs/09#a-session-grants),
 * signed with the dev HS256 secret fjarr-server is started with.
 */
import { createHmac } from "node:crypto";

export interface GrantOptions {
  robotId: string;
  secret?: string;
  tenant?: string;
  operator?: { id: string; label: string };
  capabilities?: Array<{ name: string; params?: Record<string, unknown> }>;
  /** Seconds until expiry (≤ 5 min per docs/09). */
  ttlSeconds?: number;
  /** Override the expiry (e.g. in the past, to exercise grant-expired). */
  exp?: number;
}

const b64url = (b: Buffer | string) => Buffer.from(b).toString("base64url");

export function mintGrant(o: GrantOptions): string {
  const header = b64url(JSON.stringify({ alg: "HS256", typ: "JWT" }));
  const claims = {
    iss: "fjarr-lab",
    aud: "fjarr",
    exp: o.exp ?? Math.floor(Date.now() / 1000) + (o.ttlSeconds ?? 300),
    tenant: o.tenant ?? "lab",
    robot_id: o.robotId,
    operator: o.operator ?? { id: "lab@fjarr.test", label: "Lab operator" },
    capabilities: o.capabilities ?? [{ name: "fjarr.test" }],
  };
  const body = b64url(JSON.stringify(claims));
  const sig = createHmac("sha256", o.secret ?? process.env.FJARR_GRANT_HS256_SECRET ?? "dev-only-grant-secret")
    .update(`${header}.${body}`)
    .digest("base64url");
  return `${header}.${body}.${sig}`;
}
