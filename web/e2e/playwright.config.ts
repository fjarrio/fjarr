/**
 * Browser lab runner (docs/25). The browser is never launched here: every
 * project connects over CDP to the `browser` compose service (or whatever
 * E2E_BROWSER points at); the lab page is served by Vite from this package.
 */
import { defineConfig } from "@playwright/test";
import { env } from "./src/env.ts";

export default defineConfig({
  testDir: "tests",
  outputDir: "out/test-results",
  fullyParallel: false,
  workers: 1,
  retries: process.env.CI ? 1 : 0,
  timeout: 60_000,
  expect: { timeout: 10_000 },
  reporter: process.env.CI ? [["list"], ["html", { open: "never", outputFolder: "out/report" }]] : [["list"]],
  use: {
    baseURL: env.pageOrigin,
    trace: "retain-on-failure",
    actionTimeout: 15_000,
  },
  projects: [
    { name: "loopback", testDir: "tests/loopback" },
    { name: "stack", testDir: "tests/stack" },
    { name: "spike", testDir: "tests/spike" },
  ],
  webServer: {
    command: "pnpm run app",
    url: "http://localhost:5174/",
    reuseExistingServer: true,
    timeout: 60_000,
    stdout: "ignore",
  },
});
