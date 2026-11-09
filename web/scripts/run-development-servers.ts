// `bun run dev`: builds and starts the C++ research API (exsim_api) and
// `next dev` together, points the UI proxy at the service, and stops both when
// either exits or on Ctrl+C.
//
// `bun run dev:mock` (or `bun run dev -- --mock`) starts the TypeScript mock
// service in web/test/mock-server/ instead of the C++ service.

import { join } from "node:path";

const useMockService = process.argv.includes("--mock") || process.env.EXSIM_DEV_BACKEND === "mock";
const repositoryRoot = join(import.meta.dir, "..", "..");
const researchServicePort = process.env.EXSIM_API_PORT ?? process.env.EXSIM_MOCK_PORT ?? "7600";
const researchServiceDataDirectory = process.env.EXSIM_API_DATA_DIR ?? join(repositoryRoot, "build", "exsim-api-data");
const sharedEnvironment = {
  ...process.env,
  EXSIM_MOCK_PORT: researchServicePort,
  EXSIM_API_BASE_URL: `http://127.0.0.1:${researchServicePort}`,
};

function startResearchService(): Bun.Subprocess<"ignore", "pipe", "inherit"> {
  if (useMockService) {
    console.log(`dev: starting the mock research service on port ${researchServicePort}`);
    return Bun.spawn(["bun", "test/mock-server/start-mock-server.ts"], { env: sharedEnvironment, stdin: "ignore", stdout: "pipe", stderr: "inherit" });
  }
  console.log("dev: building the C++ research service (make MODE=release build/release/exsim_api)");
  const build = Bun.spawnSync(["make", "-C", repositoryRoot, "MODE=release",
    `WITH_POSTGRES=${process.env.EXSIM_DATABASE_URL ? "1" : "0"}`, "build/release/exsim_api"], { stdout: "inherit", stderr: "inherit" });
  if (build.exitCode !== 0) {
    console.error("dev: building exsim_api failed; fix the build or run `bun run dev:mock` for the TypeScript mock");
    process.exit(build.exitCode ?? 1);
  }
  console.log(`dev: starting exsim_api on port ${researchServicePort} with data in ${researchServiceDataDirectory}`);
  return Bun.spawn([join(repositoryRoot, "build", "release", "exsim_api"), "--port", researchServicePort, "--data-dir", researchServiceDataDirectory], {
    env: sharedEnvironment,
    stdin: "ignore",
    stdout: "pipe",
    stderr: "inherit",
  });
}

const researchServiceProcess = startResearchService();
let nextDevelopmentProcess: ReturnType<typeof Bun.spawn> | undefined;

function stopBothServers(): void {
  researchServiceProcess.kill("SIGTERM");
  nextDevelopmentProcess?.kill("SIGTERM");
}

process.on("SIGINT", stopBothServers);
process.on("SIGTERM", stopBothServers);

try {
  // Wait for our own child to bind, rather than accepting an older API on the port.
  await new Promise<void>((resolve, reject) => {
    const timeout = setTimeout(() => reject(new Error("dev: API startup timed out")), 10_000);
    researchServiceProcess.exited.then((code) => {
      clearTimeout(timeout);
      reject(new Error(`dev: API exited before startup (${code}); frontend was not started`));
    });
    void (async () => {
      const decoder = new TextDecoder();
      let output = "";
      const reader = researchServiceProcess.stdout.getReader();
      while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        process.stdout.write(value);
        output = (output + decoder.decode(value, { stream: true })).slice(-2048);
        if (output.includes("listening on http://")) {
          clearTimeout(timeout);
          resolve();
        }
      }
    })().catch((error) => { clearTimeout(timeout); reject(error); });
  });
  nextDevelopmentProcess = Bun.spawn(["bunx", "next", "dev"], { env: sharedEnvironment, stdout: "inherit", stderr: "inherit" });
  const firstExitCode = await Promise.race([researchServiceProcess.exited, nextDevelopmentProcess.exited]);
  stopBothServers();
  await Promise.all([researchServiceProcess.exited, nextDevelopmentProcess.exited]);
  process.exit(firstExitCode);
} catch (error) {
  stopBothServers();
  await researchServiceProcess.exited;
  console.error(error instanceof Error ? error.message : error);
  process.exit(1);
}

export {};
