import { defineConfig } from "@playwright/test";

// Isolated UI failure tests. Each browser context gets its own in-process
// service; nothing reaches the installed API or real research data.
export default defineConfig({
  testDir: "./browser-tests",
  testMatch: "**/*.browser.ts",
  workers: 1,
  timeout: 20_000,
  expect: { timeout: 5000 },
  reporter: "list",
  use: { baseURL: "http://127.0.0.1:3762", viewport: { width: 1440, height: 1000 }, trace: "retain-on-failure" },
  webServer: { command: "node .next/standalone/server.js", env: { HOSTNAME: "127.0.0.1", PORT: "3762" }, url: "http://127.0.0.1:3762/experiments", timeout: 30_000, reuseExistingServer: false },
});
