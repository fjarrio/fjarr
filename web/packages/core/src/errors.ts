import type { ErrorCode } from "./protocol.js";

/** Typed failure: `code` is a docs/08 error code or a client-side one. */
export class FjarrError extends Error {
  readonly code: ErrorCode | ClientErrorCode;
  readonly causedBy: string | undefined;
  constructor(code: ErrorCode | ClientErrorCode, message: string, causedBy?: string) {
    super(message);
    this.name = "FjarrError";
    this.code = code;
    this.causedBy = causedBy;
  }
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
