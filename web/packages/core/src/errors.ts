import { isHeldByData, type ControlDomain, type ControlHolder, type ErrorCode } from "./protocol.js";

/**
 * Typed failure: `code` is a docs/08 error code or a client-side one.
 * `data` is the wire `error.data` when the agent sent one (code-specific,
 * docs/08#errors); read it through a typed accessor such as `heldBy()`.
 */
export class FjarrError extends Error {
  readonly code: ErrorCode | ClientErrorCode;
  readonly causedBy: string | undefined;
  readonly data: Readonly<Record<string, unknown>> | undefined;
  constructor(code: ErrorCode | ClientErrorCode, message: string, causedBy?: string, data?: Record<string, unknown>) {
    super(message);
    this.name = "FjarrError";
    this.code = code;
    this.causedBy = causedBy;
    this.data = data;
  }
}

/** Who holds what a request was refused over. */
export interface HeldBy {
  /** The control domain (`control-held`); `null` for a holder of something else (`fjarr.net` `busy`). */
  domain: ControlDomain | null;
  holder: ControlHolder;
  /** Unix ms on the agent's clock. */
  since: number;
}

/**
 * The holder named by a `control-held` or `busy` refusal, so a client can say
 * who to ask; `null` for any other error or when the data is absent/malformed.
 * spec: docs/08-protocol.md#errors · docs/10-security.md#session-ownership
 */
export function heldBy(error: unknown): HeldBy | null {
  if (!(error instanceof FjarrError) || (error.code !== "control-held" && error.code !== "busy")) return null;
  const d = error.data;
  if (!isHeldByData(d)) return null;
  return { domain: d.domain ?? null, holder: { id: d.holder.id, label: d.holder.label }, since: d.since };
}

/** Client-side codes (never on the wire). */
export type ClientErrorCode =
  | "not-connected"
  | "timeout"
  | "closed"
  | "grant-fetch-failed"
  | "channel-missing"
  | "queue-overflow"
  | "session-rejected"
  | "transport-failed"
  | "reconnect-exhausted"
  | "not-implemented";

export class NotImplementedError extends FjarrError {
  constructor(what: string, milestone: string) {
    super("not-implemented", `${what} lands in ${milestone} — see docs/17-roadmap.md`);
    this.name = "NotImplementedError";
  }
}

export const isFjarrError = (e: unknown): e is FjarrError => e instanceof FjarrError;
