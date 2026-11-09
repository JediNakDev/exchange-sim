import { defineConfig } from "@playwright/test";

// Run the original assertions and narrow review probes on this checkout's build.
export default defineConfig({
  testDir: "..",
  testMatch: ["**/frontend-execution.browser.ts", "**/fill-selection.browser.ts", "**/fill-identity.browser.ts"],
  workers: 1,
  timeout: 20_000,
  expect: { timeout: 5000 },
  reporter: "list",
  outputDir: "/tmp/exsim-fer01-c95c391d-browser-results",
  use: { baseURL: "http://127.0.0.1:3762", viewport: { width: 1440, height: 1000 }, trace: "on" },
  webServer: {
    command: "node .next/standalone/server.js",
    cwd: process.cwd(),
    env: { HOSTNAME: "127.0.0.1", PORT: "3762", EXSIM_API_BASE_URL: "http://127.0.0.1:9" },
    url: "http://127.0.0.1:3762/experiments",
    timeout: 30_000,
    reuseExistingServer: false,
  },
});
