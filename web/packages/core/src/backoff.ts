/**
 * Flap-resistant reconnect backoff — pure functions (fleet-daemon lesson,
 * docs/11). spec: docs/08-protocol.md#reconnection
 */

export interface BackoffPolicy {
  baseMs: number;
  factor: number;
  capMs: number;
  /** ±fraction of the delay. */
  jitter: number;
  /** Attempts reset only after this long connected. */
  stableResetMs: number;
}

export const DEFAULT_BACKOFF: BackoffPolicy = {
  baseMs: 500,
  factor: 2,
  capMs: 30_000,
  jitter: 0.2,
  stableResetMs: 30_000,
};

/** Delay for `attempt` (0-based). `random` ∈ [0,1) for deterministic tests. */
export function backoffDelay(attempt: number, policy: BackoffPolicy = DEFAULT_BACKOFF, random: () => number = Math.random): number {
  const raw = Math.min(policy.capMs, policy.baseMs * Math.pow(policy.factor, Math.max(0, attempt)));
  const spread = raw * policy.jitter;
  return Math.round(raw - spread + random() * 2 * spread);
}

export class Backoff {
  private attempt = 0;
  private connectedAt: number | null = null;
  constructor(
    private readonly policy: BackoffPolicy = DEFAULT_BACKOFF,
    private readonly random: () => number = Math.random,
  ) {}

  get attempts(): number {
    return this.attempt;
  }

  /** Delay before the next attempt; increments the attempt counter. */
  next(): number {
    return backoffDelay(this.attempt++, this.policy, this.random);
  }

  markConnected(now: number): void {
    this.connectedAt = now;
  }

  /** On disconnect: attempts reset only if the connection was stable. */
  markDisconnected(now: number): void {
    if (this.connectedAt !== null && now - this.connectedAt >= this.policy.stableResetMs) {
      this.attempt = 0;
    }
    this.connectedAt = null;
  }

  reset(): void {
    this.attempt = 0;
    this.connectedAt = null;
  }
}
