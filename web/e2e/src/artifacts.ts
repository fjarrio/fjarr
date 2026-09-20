/**
 * Every run leaves artifacts readable without a screen (docs/25): JSON for
 * CI, text for people and AI agents.
 */
import { createHash } from "node:crypto";
import { mkdirSync, rmSync, writeFileSync, appendFileSync } from "node:fs";
import { join } from "node:path";
import { env } from "./env.ts";

export class OutDir {
  readonly dir: string;
  private readonly summary: Record<string, unknown> = {};
  private readonly lines: string[] = [];

  /** A fresh directory per run: JSON-lines files never accumulate across runs or retries. */
  constructor(name: string, options: { keep?: boolean } = {}) {
    this.dir = join(env.outRoot, slug(name));
    if (!options.keep) rmSync(this.dir, { recursive: true, force: true });
    mkdirSync(this.dir, { recursive: true });
  }

  path(file: string): string {
    return join(this.dir, file);
  }

  writeJson(file: string, data: unknown): string {
    const p = this.path(file);
    writeFileSync(p, JSON.stringify(data, null, 2));
    return p;
  }

  writeText(file: string, text: string): string {
    const p = this.path(file);
    writeFileSync(p, text);
    return p;
  }

  appendJsonl(file: string, record: unknown): void {
    appendFileSync(this.path(file), JSON.stringify(record) + "\n");
  }

  /** Add a fact to summary.json and a line to summary.txt. */
  note(key: string, value: unknown, line?: string): void {
    this.summary[key] = value;
    this.lines.push(line ?? `${key}: ${typeof value === "object" ? JSON.stringify(value) : String(value)}`);
  }

  finish(status: string): { json: string; txt: string } {
    this.summary.status = status;
    this.summary.writtenAt = new Date().toISOString();
    const json = this.writeJson("summary.json", this.summary);
    const txt = this.writeText("summary.txt", [`status: ${status}`, ...this.lines, ""].join("\n"));
    return { json, txt };
  }
}

/** Readable prefix plus a short hash of the full name, so long titles never share a directory. */
export function slug(s: string): string {
  const readable = s
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "");
  if (readable.length <= 72) return readable;
  return `${readable.slice(0, 64).replace(/-+$/, "")}-${createHash("sha1").update(s).digest("hex").slice(0, 7)}`;
}

export function percentile(values: number[], p: number): number | null {
  if (values.length === 0) return null;
  const sorted = [...values].sort((a, b) => a - b);
  const idx = Math.min(sorted.length - 1, Math.max(0, Math.ceil((p / 100) * sorted.length) - 1));
  return sorted[idx]!;
}
