// Starts the mock research API on loopback. Run with `bun run mock-server`.

import { MockResearchService } from "./mock-research-service";

const mockServerPort = Number(process.env.EXSIM_MOCK_PORT ?? "7600");
const mockResearchService = new MockResearchService();

const server = Bun.serve({
  hostname: "127.0.0.1",
  port: mockServerPort,
  fetch: (request) => mockResearchService.handleRequest(request),
});

console.log(`exsim mock research API listening on http://${server.hostname}:${server.port}/v1`);
