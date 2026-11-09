import { defineConfig } from "@playwright/test";

// Use an existing standalone artifact, or build once in the parent's serial
// validation slot. Port 3862 is private to this report-only renderer test.
export default defineConfig({
  testDir: "..",
  testMatch: ["**/frontend-execution.browser.ts", "**/research-workflow.browser.ts"],
  workers: 1,
  timeout: 15_000,
  expect: { timeout: 5000 },
  reporter: "list",
  outputDir: "/tmp/exsim-frontend-execution-recheck-browser-results",
  use: { baseURL: "http://127.0.0.1:3862", viewport: { width: 1440, height: 1000 }, trace: "retain-on-failure" },
  webServer: {
    command: 'node "$EXSIM_REVIEW_STANDALONE"',
    cwd: process.cwd(),
    env: { HOSTNAME: "127.0.0.1", PORT: "3862", EXSIM_API_BASE_URL: "http://127.0.0.1:9", EXSIM_REVIEW_STANDALONE: process.env.EXSIM_REVIEW_STANDALONE ?? ".next/standalone/server.js" },
    url: "http://127.0.0.1:3862/experiments",
    timeout: 15_000,
    reuseExistingServer: false,
  },
});
