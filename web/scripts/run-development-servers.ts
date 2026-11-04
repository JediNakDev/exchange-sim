// `bun run dev`: starts the mock research API and `next dev` together, points the
// UI proxy at the mock, and stops both when either exits or on Ctrl+C.

const mockServerPort = process.env.EXSIM_MOCK_PORT ?? "7600";
const sharedEnvironment = { ...process.env, EXSIM_MOCK_PORT: mockServerPort, EXSIM_API_BASE_URL: process.env.EXSIM_API_BASE_URL ?? `http://127.0.0.1:${mockServerPort}` };

const mockServerProcess = Bun.spawn(["bun", "mock-server/start-mock-server.ts"], { env: sharedEnvironment, stdout: "inherit", stderr: "inherit" });
const nextDevelopmentProcess = Bun.spawn(["bunx", "next", "dev"], { env: sharedEnvironment, stdout: "inherit", stderr: "inherit" });

function stopBothServers(): void {
  mockServerProcess.kill();
  nextDevelopmentProcess.kill();
}

process.on("SIGINT", stopBothServers);
process.on("SIGTERM", stopBothServers);

const firstExitCode = await Promise.race([mockServerProcess.exited, nextDevelopmentProcess.exited]);
stopBothServers();
await Promise.all([mockServerProcess.exited, nextDevelopmentProcess.exited]);
process.exit(firstExitCode);

export {};
