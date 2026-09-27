/**
 * The route `fjarr-connect login` hands off to (docs/27#logging-in). DEMO RULE: the component is
 * @fjarr/react's; what this file owns is the company's side of it — who may mint a credential
 * (the demo's role stands in for their auth) and where their operator API lives.
 * spec: docs/21-web-client-architecture.md#cli-login · docs/09-interfaces.md#operator-api
 */
import { FjarrCliLogin } from "@fjarr/react";

export function CliLoginPage({ backend, role }: { backend: string; role: string }) {
  return (
    <main style={{ maxWidth: 560, margin: "3rem auto", fontFamily: "system-ui, sans-serif" }}>
      <FjarrCliLogin
        mintOperatorCredential={async () => {
          const res = await fetch(`${backend}/api/me/fjarr-cli-token?role=${encodeURIComponent(role)}`, { method: "POST" });
          if (!res.ok) throw new Error(`credential request failed: ${res.status}`);
          return ((await res.json()) as { credential: string }).credential;
        }}
        approveCode={async (code, credential) => {
          const res = await fetch(`${backend}/api/fjarr/cli-codes/${code}`, {
            method: "PUT",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ credential }),
          });
          if (!res.ok) throw new Error(((await res.json().catch(() => ({}))) as { error?: string }).error ?? `approval failed: ${res.status}`);
        }}
      />
      <p style={{ opacity: 0.7 }}>
        Signed in as <strong>{role}</strong> (the demo's stand-in for your account).
      </p>
    </main>
  );
}
