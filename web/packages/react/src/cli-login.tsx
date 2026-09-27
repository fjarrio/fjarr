/**
 * The CLI login handoff (docs/21#cli-login, docs/27#logging-in): `fjarr-connect login` needs an
 * operator credential and a terminal holds no logged-in session, so the host mounts this on one
 * AUTHENTICATED route and the component hands the signed-in user's credential to the CLI.
 *
 * Two shapes, both decided by the CLI and read from the query string unless the host passes
 * `request`:
 *   - loopback (`?port=&state=`): after an explicit click, POST the credential to
 *     `http://127.0.0.1:<port>/callback` with the CLI's `state`. The callback host is never a
 *     parameter — only the port is — which is how "refuses any callback that is not loopback" is
 *     true by construction rather than by validation.
 *   - code (`?code=`, or typed): show the code so the human can check it against the terminal,
 *     then approve it through the host's backend, which the CLI is polling.
 *
 * The library never stores the credential and never sees the CLI beyond that one request.
 * Headless-first like everything else here (docs/05): every string is a prop.
 */
import { useState, type CSSProperties, type ReactNode } from "react";

export interface CliLoginRequest {
  /** Loopback: the port the CLI is listening on at 127.0.0.1. */
  port?: number;
  /** Loopback: the CLI's random `state`, echoed back so the CLI can reject a stray post. */
  state?: string;
  /** Code: the code the terminal printed. */
  code?: string;
}

export interface FjarrCliLoginProps {
  /** Mint a credential for the signed-in user — usually a call the host already has. */
  mintOperatorCredential: () => Promise<string>;
  /** Approve a login code with that credential, on the host's backend (docs/09#operator-api). */
  approveCode?: (code: string, credential: string) => Promise<void>;
  /** What the CLI asked for; read from `window.location` when omitted. */
  request?: CliLoginRequest;
  strings?: Partial<CliLoginStrings>;
  className?: string;
  style?: CSSProperties;
}

export interface CliLoginStrings {
  title: string;
  loopbackExplain: (port: number) => ReactNode;
  codeExplain: (code: string) => ReactNode;
  codePrompt: string;
  approve: string;
  working: string;
  doneLoopback: string;
  doneCode: string;
  badPort: string;
  noApprove: string;
  failed: (message: string) => string;
}

const DEFAULT_STRINGS: CliLoginStrings = {
  title: "Sign in fjarr-connect",
  loopbackExplain: (port) => (
    <>
      A terminal on <strong>this machine</strong> is waiting on port <code>{port}</code> for a credential for your account. Only approve this if you just ran{" "}
      <code>fjarr-connect login</code>.
    </>
  ),
  codeExplain: (code) => (
    <>
      Does the code below match the one your terminal printed? <code style={{ fontSize: "1.4em", letterSpacing: "0.15em" }}>{code}</code>
    </>
  ),
  codePrompt: "Enter the code your terminal printed",
  approve: "Approve",
  working: "Approving…",
  doneLoopback: "Done — you can close this tab and go back to the terminal.",
  doneCode: "Approved — the terminal will pick it up within a few seconds.",
  badPort: "The callback port is not valid; run fjarr-connect login again.",
  noApprove: "This dashboard is not set up to approve login codes.",
  failed: (message) => `Sign-in failed: ${message}`,
};

/** Read what the CLI put in the query string. Exported for the host's convenience and for tests. */
export function readCliLoginRequest(search: string = typeof window === "undefined" ? "" : window.location.search): CliLoginRequest {
  const q = new URLSearchParams(search);
  const port = q.get("port");
  const parsedPort = port !== null && /^\d{1,5}$/.test(port) ? Number(port) : undefined;
  return {
    port: parsedPort,
    state: q.get("state") ?? undefined,
    code: q.get("code")?.toUpperCase().replace(/[^A-Z0-9]/g, "") || undefined,
  };
}

/** A usable loopback port: unprivileged and in range. */
export function isValidCallbackPort(port: number | undefined): port is number {
  return typeof port === "number" && Number.isInteger(port) && port >= 1024 && port <= 65535;
}

type Phase = { kind: "idle" } | { kind: "working" } | { kind: "done" } | { kind: "error"; message: string };

export function FjarrCliLogin({ mintOperatorCredential, approveCode, request, strings, className, style }: FjarrCliLoginProps) {
  const t = { ...DEFAULT_STRINGS, ...strings };
  const req = request ?? readCliLoginRequest();
  const [phase, setPhase] = useState<Phase>({ kind: "idle" });
  const [typedCode, setTypedCode] = useState("");
  const loopback = req.port !== undefined || req.state !== undefined;
  const code = req.code ?? typedCode.toUpperCase().replace(/[^A-Z0-9]/g, "");

  const run = async (work: (credential: string) => Promise<void>) => {
    setPhase({ kind: "working" });
    try {
      const credential = await mintOperatorCredential();
      await work(credential);
      setPhase({ kind: "done" });
    } catch (e) {
      setPhase({ kind: "error", message: e instanceof Error ? e.message : String(e) });
    }
  };

  const approveLoopback = () => {
    if (!isValidCallbackPort(req.port)) return setPhase({ kind: "error", message: t.badPort });
    const port = req.port;
    void run(async (credential) => {
      // 127.0.0.1 literally: not "localhost", which a hosts file can point anywhere.
      const res = await fetch(`http://127.0.0.1:${port}/callback`, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ credential, state: req.state ?? "" }),
      });
      if (!res.ok) throw new Error(`the terminal answered ${res.status}`);
    });
  };

  const approveTheCode = () => {
    if (!approveCode) return setPhase({ kind: "error", message: t.noApprove });
    if (code.length !== 8) return;
    void run((credential) => approveCode(code, credential));
  };

  const busy = phase.kind === "working";
  return (
    <div className={className} style={style} data-fjarr-cli-login={loopback ? "loopback" : "code"}>
      <h2>{t.title}</h2>
      {loopback ? (
        <>
          <p>{isValidCallbackPort(req.port) ? t.loopbackExplain(req.port) : t.badPort}</p>
          {phase.kind === "done" ? (
            <p role="status">{t.doneLoopback}</p>
          ) : (
            <button type="button" onClick={approveLoopback} disabled={busy || !isValidCallbackPort(req.port)}>
              {busy ? t.working : t.approve}
            </button>
          )}
        </>
      ) : (
        <>
          {req.code ? (
            <p>{t.codeExplain(req.code)}</p>
          ) : (
            <label>
              {t.codePrompt}{" "}
              <input value={typedCode} onChange={(e) => setTypedCode(e.target.value)} maxLength={9} autoCapitalize="characters" spellCheck={false} />
            </label>
          )}
          {phase.kind === "done" ? (
            <p role="status">{t.doneCode}</p>
          ) : (
            <button type="button" onClick={approveTheCode} disabled={busy || code.length !== 8}>
              {busy ? t.working : t.approve}
            </button>
          )}
        </>
      )}
      {phase.kind === "error" && <p role="alert">{t.failed(phase.message)}</p>}
    </div>
  );
}
