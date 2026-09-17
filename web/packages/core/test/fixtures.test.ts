/**
 * Golden fixtures against the TS types/guards — closes docs/18 open
 * question #15: the same protocol/fixtures the schema and Rust are checked
 * against must parse here, and the invalid ones must be rejected.
 * spec: docs/15-testing-strategy.md (conformance) · docs/21#testing-docs15
 */
import { readdirSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";
import { isEnvelope, isSignalingMessage } from "../src/index.js";

const root = join(dirname(fileURLToPath(import.meta.url)), "../../../../protocol/fixtures");
const load = (dir: string, name: string): unknown => JSON.parse(readFileSync(join(root, dir, name), "utf8"));
// intro-* fixtures (pipeline snapshots, docs/24) are schema-only until the
// TS types land with the fjarr.introspect capability (slice 5).
const guard = (name: string) => (name.startsWith("sig-") ? isSignalingMessage : name.startsWith("env-") ? isEnvelope : null);

describe("protocol golden fixtures", () => {
  const valid = readdirSync(join(root, "valid")).filter((f) => f.endsWith(".json") && !f.startsWith("intro-"));
  const invalid = readdirSync(join(root, "invalid")).filter((f) => f.endsWith(".json") && !f.startsWith("intro-"));

  it("has the full fixture set", () => {
    expect(valid.length).toBeGreaterThanOrEqual(23);
    expect(invalid.length).toBeGreaterThanOrEqual(6);
  });

  for (const f of valid) {
    it(`accepts valid/${f}`, () => {
      expect(guard(f)!(load("valid", f))).toBe(true);
    });
  }

  for (const f of invalid) {
    it(`rejects invalid/${f}`, () => {
      expect(guard(f)!(load("invalid", f))).toBe(false);
    });
  }
});
