// `bun run dev`: builds and starts the C++ research API (exsim_api) and
// `next dev` together, points the UI proxy at the service, and stops both when
// either exits or on Ctrl+C.
//
// `bun run dev:mock` (or `bun run dev -- --mock`) starts the TypeScript mock
// service in web/mock-server/ instead of the C++ service.

import { join } from "node:path";

const useMockService = process.argv.includes("--mock") || process.env.EXSIM_DEV_BACKEND === "mock";
const repositoryRoot = join(import.meta.dir, "..", "..");
const researchServicePort = process.env.EXSIM_API_PORT ?? process.env.EXSIM_MOCK_PORT ?? "7600";
const researchServiceDataDirectory = process.env.EXSIM_API_DATA_DIR ?? join(repositoryRoot, "build", "exsim-api-data");
const sharedEnvironment = {
  ...process.env,
  EXSIM_MOCK_PORT: researchServicePort,
  EXSIM_API_BASE_URL: process.env.EXSIM_API_BASE_URL ?? `http://127.0.0.1:${researchServicePort}`,
};

function startResearchService(): ReturnType<typeof Bun.spawn> {
  if (useMockService) {
    console.log(`dev: starting the mock research service on port ${researchServicePort}`);
    return Bun.spawn(["bun", "mock-server/start-mock-server.ts"], { env: sharedEnvironment, stdout: "inherit", stderr: "inherit" });
  }
  console.log("dev: building the C++ research service (make MODE=release build/release/exsim_api)");
  const build = Bun.spawnSync(["make", "-C", repositoryRoot, "MODE=release", "build/release/exsim_api"], { stdout: "inherit", stderr: "inherit" });
  if (build.exitCode !== 0) {
    console.error("dev: building exsim_api failed; fix the build or run `bun run dev:mock` for the TypeScript mock");
    process.exit(build.exitCode ?? 1);
  }
  console.log(`dev: starting exsim_api on port ${researchServicePort} with data in ${researchServiceDataDirectory}`);
  return Bun.spawn([join(repositoryRoot, "build", "release", "exsim_api"), "--port", researchServicePort, "--data-dir", researchServiceDataDirectory], {
    env: sharedEnvironment,
    stdout: "inherit",
    stderr: "inherit",
  });
}

const researchServiceProcess = startResearchService();
const nextDevelopmentProcess = Bun.spawn(["bunx", "next", "dev"], { env: sharedEnvironment, stdout: "inherit", stderr: "inherit" });

function stopBothServers(): void {
  researchServiceProcess.kill("SIGTERM");
  nextDevelopmentProcess.kill("SIGTERM");
}

process.on("SIGINT", stopBothServers);
process.on("SIGTERM", stopBothServers);

const firstExitCode = await Promise.race([researchServiceProcess.exited, nextDevelopmentProcess.exited]);
stopBothServers();
await Promise.all([researchServiceProcess.exited, nextDevelopmentProcess.exited]);
process.exit(firstExitCode);

export {};
