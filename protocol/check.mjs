#!/usr/bin/env node
/**
 * Protocol conformance gate: every fixture in fixtures/valid must validate
 * against its schema; every fixture in fixtures/invalid must be rejected.
 * Fixture naming selects the schema: sig-*.json → signaling, env-*.json →
 * envelope. // spec: docs/08-protocol.md#versioning, docs/15 (conformance)
 */
import { readFileSync, readdirSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import Ajv2020 from "ajv/dist/2020.js";

const root = dirname(fileURLToPath(import.meta.url));
const load = (p) => JSON.parse(readFileSync(join(root, p), "utf8"));

const ajv = new Ajv2020.default({ strict: true, allErrors: true });
ajv.addSchema(load("schemas/envelope.schema.json"));
const validateSignaling = ajv.compile(load("schemas/signaling.schema.json"));
const validateEnvelope = ajv.getSchema("https://fjarr.io/protocol/envelope.schema.json");

const pick = (name) => {
  if (name.startsWith("sig-")) return validateSignaling;
  if (name.startsWith("env-")) return validateEnvelope;
  throw new Error(`fixture ${name}: name must start with sig- or env-`);
};

let failures = 0;
const run = (dir, expectValid) => {
  for (const f of readdirSync(join(root, "fixtures", dir)).sort()) {
    const validate = pick(f);
    const ok = validate(load(join("fixtures", dir, f)));
    const pass = ok === expectValid;
    if (!pass) failures++;
    console.log(
      `${pass ? "PASS" : "FAIL"}  ${dir}/${f}` +
        (!pass && expectValid ? `  ${ajv.errorsText(validate.errors)}` : "") +
        (!pass && !expectValid ? "  (validated but MUST be rejected)" : ""),
    );
  }
};
run("valid", true);
run("invalid", false);

console.log(failures === 0 ? "\nprotocol-check: all fixtures conform" : `\nprotocol-check: ${failures} failure(s)`);
process.exit(failures === 0 ? 0 : 1);
