#!/usr/bin/env node
// fjarr-lab — the agent-first CLI over the lab browser (docs/25). Runs the
// TypeScript entry point with Node's built-in TypeScript transform (parameter properties need it); no build step.
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const cli = fileURLToPath(new URL("../src/cli.ts", import.meta.url));
const child = spawn(process.execPath, ["--experimental-transform-types", "--no-warnings=ExperimentalWarning", cli, ...process.argv.slice(2)], { stdio: "inherit" });
child.on("exit", (code, signal) => process.exit(code ?? (signal ? 1 : 0)));
