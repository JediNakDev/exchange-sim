// End-to-end contract test against the real C++ research service.
//
// Starts build/release/exsim_api (or EXSIM_API_BINARY) on an ephemeral port
// with a fresh data directory, drives it through the same openapi-fetch client
// the UI uses, and validates every response body against the schema
// docs/api/openapi.yaml declares for that operation and status.
//
// Run with `bun run test:e2e` (builds the binary first).

import { afterAll, beforeAll, describe, expect, test } from "bun:test";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { validateOperationResponse } from "../mock-server/openapi-schema-validator";
import { createExsimApiClient, type ApiSchemas, type ExsimApiClient } from "../src/lib/api/exsim-api-client";

const EXSIM_API_BINARY_PATH = process.env.EXSIM_API_BINARY ?? join(import.meta.dir, "..", "..", "build", "release", "exsim_api");
const RUN_TIMEOUT_MS = 60_000;
const TERMINAL_RUN_STATES = new Set(["completed", "cancelled", "failed"]);

interface RunningApiService {
  process: ReturnType<typeof Bun.spawn>;
  baseUrl: string;
  stderrChunks: string[];
}

const dataDirectory = mkdtempSync(join(tmpdir(), "exsim-api-e2e-"));
let service: RunningApiService;
let apiClient: ExsimApiClient;
const observedOperations = new Set<string>();

async function startApiService(): Promise<RunningApiService> {
  const childProcess = Bun.spawn([EXSIM_API_BINARY_PATH, "--port", "0", "--data-dir", dataDirectory], { stdout: "pipe", stderr: "pipe" });
  const stderrChunks: string[] = [];
  void (async () => {
    const stderrReader = (childProcess.stderr as ReadableStream<Uint8Array>).getReader();
    for (;;) {
      const { value, done } = await stderrReader.read();
      if (done) return;
      stderrChunks.push(new TextDecoder().decode(value));
    }
  })();
  const reader = (childProcess.stdout as ReadableStream<Uint8Array>).getReader();
  let stdoutText = "";
  const deadline = Date.now() + 10_000;
  while (!/listening on (http:\/\/\S+)/.test(stdoutText)) {
    if (Date.now() > deadline) throw new Error(`exsim_api did not report its port; stderr: ${stderrChunks.join("")}`);
    const { value, done } = await reader.read();
    if (done) throw new Error(`exsim_api exited before listening; stderr: ${stderrChunks.join("")}`);
    stdoutText += new TextDecoder().decode(value);
  }
  reader.releaseLock();
  return { process: childProcess, baseUrl: stdoutText.match(/listening on (http:\/\/\S+)/)![1], stderrChunks };
}

async function stopApiService(running: RunningApiService): Promise<number> {
  running.process.kill("SIGTERM");
  return await running.process.exited;
}

/** Asserts status, schema-version header and contract conformance; returns the body. */
function expectContract<ResponseBody>(
  result: { data?: ResponseBody; error?: unknown; response: Response },
  httpMethod: "get" | "post",
  pathTemplate: string,
  expectedStatus: number,
): ResponseBody {
  const body = result.data ?? result.error;
  expect({ status: result.response.status, body }).toMatchObject({ status: expectedStatus });
  expect(result.response.headers.get("Exsim-Schema-Version")).toBe("exsim.api/1");
  const { issues } = validateOperationResponse(pathTemplate, httpMethod, result.response.status, body);
  expect({ operation: `${httpMethod} ${pathTemplate}`, issues }).toEqual({ operation: `${httpMethod} ${pathTemplate}`, issues: [] });
  observedOperations.add(`${httpMethod} ${pathTemplate}`);
  return body as ResponseBody;
}

function expectContractError(
  result: { error?: unknown; response: Response },
  httpMethod: "get" | "post",
  pathTemplate: string,
  expectedStatus: number,
): ApiSchemas["ApiError"] {
  return expectContract<ApiSchemas["ErrorResponseBody"]>({ error: result.error, response: result.response }, httpMethod, pathTemplate, expectedStatus).error;
}

let idempotencyCounter = 0;
function nextIdempotencyKey(purpose: string): string {
  idempotencyCounter += 1;
  return `e2e-${purpose}-${Date.now()}-${idempotencyCounter}`;
}

async function readRun(runId: string): Promise<ApiSchemas["Run"]> {
  return expectContract(await apiClient.GET("/v1/runs/{runId}", { params: { path: { runId } } }), "get", "/v1/runs/{runId}", 200);
}

async function waitForRunState(runId: string, acceptableStates: Set<string>): Promise<ApiSchemas["Run"]> {
  const deadline = Date.now() + RUN_TIMEOUT_MS;
  for (;;) {
    const run = await readRun(runId);
    if (acceptableStates.has(run.state)) return run;
    if (Date.now() > deadline) throw new Error(`run ${runId} stayed ${run.state}`);
    await Bun.sleep(25);
  }
}

async function createRun(experimentId: string, masterSeed: string, startPaused = false): Promise<ApiSchemas["Run"]> {
  const run = expectContract(
    await apiClient.POST("/v1/runs", { body: { experimentId, masterSeed, startPaused }, params: { header: { "Idempotency-Key": nextIdempotencyKey("run") } } }),
    "post",
    "/v1/runs",
    201,
  );
  expect(run.state).toBe("queued");
  return run;
}

let baselines: ApiSchemas["BaselineConfiguration"][] = [];
const experimentIds: string[] = [];
const PAIRED_MASTER_SEEDS = ["11", "12"];

function experimentRequest(baselineIndex: number, name: string): ApiSchemas["ExperimentCreateRequest"] {
  const baseline = baselines[baselineIndex];
  const config = structuredClone(baseline.config);
  config.seedSet = { masterSeeds: [...PAIRED_MASTER_SEEDS, "13"] };
  config.bounds.endSimulationTimeNs = "20000000000";
  return { name, description: "end-to-end test experiment", derivedFromBaselineId: baseline.baselineId, config };
}

beforeAll(async () => {
  service = await startApiService();
  apiClient = createExsimApiClient(service.baseUrl);
});

afterAll(async () => {
  if (service) await stopApiService(service);
  rmSync(dataDirectory, { recursive: true, force: true });
});

describe("exsim_api binary", () => {
  test("service info, baselines and connections", async () => {
    const serviceInfo = expectContract(await apiClient.GET("/v1/service"), "get", "/v1/service", 200);
    expect(serviceInfo.executableMarketWorlds).toEqual(["interactive_synthetic"]);
    expect(serviceInfo.realTimeProtocolModeAvailable).toBe(false);
    baselines = expectContract(await apiClient.GET("/v1/baselines"), "get", "/v1/baselines", 200).items;
    expect(baselines.length).toBeGreaterThanOrEqual(2);
    const connections = expectContract(await apiClient.GET("/v1/connections"), "get", "/v1/connections", 200);
    expect(connections.serviceState).toBe("not_running");
  });

  test("previews and experiment creation, including unsupported worlds", async () => {
    const preview = expectContract(await apiClient.POST("/v1/experiment-previews", { body: experimentRequest(0, "preview") }), "post", "/v1/experiment-previews", 200);
    expect(preview.valid).toBe(true);
    expect(preview.configDigest.algorithm).toBe("sha256");

    const historicalRequest = experimentRequest(0, "historical");
    (historicalRequest.config as { marketWorld: string }).marketWorld = "historical_reconstruction";
    const historicalPreview = await apiClient.POST("/v1/experiment-previews", { body: historicalRequest });
    if (historicalPreview.response.status === 200) {
      expect(expectContract(historicalPreview, "post", "/v1/experiment-previews", 200).valid).toBe(false);
    } else {
      expectContractError(historicalPreview, "post", "/v1/experiment-previews", 422);
    }
    const historicalCreate = await apiClient.POST("/v1/experiments", { body: historicalRequest, params: { header: { "Idempotency-Key": nextIdempotencyKey("exp") } } });
    expect(["unsupported_market_world", "validation_failed"]).toContain(expectContractError(historicalCreate, "post", "/v1/experiments", 422).code);

    for (const [baselineIndex, name] of [
      [0, "baseline A"],
      [1, "candidate B"],
    ] as const) {
      const experiment = expectContract(
        await apiClient.POST("/v1/experiments", { body: experimentRequest(baselineIndex, name), params: { header: { "Idempotency-Key": nextIdempotencyKey("exp") } } }),
        "post",
        "/v1/experiments",
        201,
      );
      experimentIds.push(experiment.experimentId);
    }
    expectContract(await apiClient.GET("/v1/experiments/{experimentId}", { params: { path: { experimentId: experimentIds[0] } } }), "get", "/v1/experiments/{experimentId}", 200);
    const experimentPage = expectContract(await apiClient.GET("/v1/experiments", { params: { query: { limit: 1 } } }), "get", "/v1/experiments", 200);
    expect(experimentPage.items[0].experimentId).toBe(experimentIds[1]);
    const secondPage = expectContract(
      await apiClient.GET("/v1/experiments", { params: { query: { limit: 1, cursor: experimentPage.page.nextCursor! } } }),
      "get",
      "/v1/experiments",
      200,
    );
    expect(secondPage.items[0].experimentId).toBe(experimentIds[0]);
    const missing = await apiClient.GET("/v1/experiments/{experimentId}", { params: { path: { experimentId: "exp_doesnotexist" } } });
    expect(expectContractError(missing, "get", "/v1/experiments/{experimentId}", 404).code).toBe("not_found");
  });

  let completedRunId = "";
  test(
    "a run executes to completion and every artifact view conforms",
    async () => {
      const run = await createRun(experimentIds[0], PAIRED_MASTER_SEEDS[0]);
      const completed = await waitForRunState(run.runId, TERMINAL_RUN_STATES);
      expect(completed.state).toBe("completed");
      expect(completed.terminalDiagnostics?.outcomeCode).toBe("completed_at_simulation_bound");
      expect(completed.progress.simulationTimeNs).toBe("20000000000");
      completedRunId = run.runId;
      const runPath = { runId: run.runId };

      const manifest = expectContract(await apiClient.GET("/v1/runs/{runId}/manifest", { params: { path: runPath } }), "get", "/v1/runs/{runId}/manifest", 200);
      expect(manifest.configDigest.algorithm).toBe("sha256");
      const summary = expectContract(await apiClient.GET("/v1/runs/{runId}/summary", { params: { path: runPath } }), "get", "/v1/runs/{runId}/summary", 200);
      expect(summary.availability).toBe("final");
      const series = expectContract(
        await apiClient.GET("/v1/runs/{runId}/series", { params: { path: runPath, query: { maxPoints: 50 } } }),
        "get",
        "/v1/runs/{runId}/series",
        200,
      );
      expect(series.series.map((timeSeries) => timeSeries.seriesKind)).toEqual(["equity", "inventory", "exchange_mid", "quoted_spread", "drawdown"]);

      const firstEventPage = expectContract(
        await apiClient.GET("/v1/runs/{runId}/events", { params: { path: runPath, query: { limit: 200 } } }),
        "get",
        "/v1/runs/{runId}/events",
        200,
      );
      expect(firstEventPage.items).toHaveLength(200);
      const secondEventPage = expectContract(
        await apiClient.GET("/v1/runs/{runId}/events", { params: { path: runPath, query: { limit: 200, cursor: firstEventPage.page.nextCursor! } } }),
        "get",
        "/v1/runs/{runId}/events",
        200,
      );
      expect(secondEventPage.items[0].eventPosition).toBe("200");
      const fillEvents = expectContract(
        await apiClient.GET("/v1/runs/{runId}/events", { params: { path: runPath, query: { eventKinds: ["order_filled"], visibility: "exchange_truth", limit: 5 } } }),
        "get",
        "/v1/runs/{runId}/events",
        200,
      );
      expect(fillEvents.items.every((event) => event.eventKind === "order_filled")).toBe(true);
      const badCursor = await apiClient.GET("/v1/runs/{runId}/events", { params: { path: runPath, query: { cursor: "not-a-cursor" } } });
      expect(expectContractError(badCursor, "get", "/v1/runs/{runId}/events", 400).code).toBe("invalid_request");

      const orders = expectContract(await apiClient.GET("/v1/runs/{runId}/orders", { params: { path: runPath, query: { limit: 20 } } }), "get", "/v1/runs/{runId}/orders", 200);
      expect(orders.items.length).toBeGreaterThan(0);
      for (const order of orders.items.slice(0, 5)) {
        const lifecycle = expectContract(
          await apiClient.GET("/v1/runs/{runId}/orders/{orderId}/lifecycle", { params: { path: { runId: run.runId, orderId: order.orderId } } }),
          "get",
          "/v1/runs/{runId}/orders/{orderId}/lifecycle",
          200,
        );
        expect(lifecycle.stages[0].stageKind).toBe("strategy_submitted");
      }
      const orderEvents = expectContract(
        await apiClient.GET("/v1/runs/{runId}/events", { params: { path: runPath, query: { orderId: orders.items[0].orderId } } }),
        "get",
        "/v1/runs/{runId}/events",
        200,
      );
      expect(orderEvents.items.length).toBeGreaterThan(1);

      const fills = expectContract(await apiClient.GET("/v1/runs/{runId}/fills", { params: { path: runPath, query: { limit: 50 } } }), "get", "/v1/runs/{runId}/fills", 200);
      expect(fills.items.length).toBeGreaterThan(0);
      const filteredFills = expectContract(
        await apiClient.GET("/v1/runs/{runId}/fills", { params: { path: runPath, query: { orderId: fills.items[0].orderId } } }),
        "get",
        "/v1/runs/{runId}/fills",
        200,
      );
      expect(filteredFills.items.every((fill) => fill.orderId === fills.items[0].orderId)).toBe(true);

      for (const view of ["exchange_truth", "strategy_visible"] as const) {
        const book = expectContract(
          await apiClient.GET("/v1/runs/{runId}/book", { params: { path: runPath, query: { eventPosition: "1000", view, depth: 5 } } }),
          "get",
          "/v1/runs/{runId}/book",
          200,
        );
        expect(book.view).toBe(view);
      }

      const stepAfterCompletion = await apiClient.POST("/v1/runs/{runId}/step", {
        params: { path: runPath, header: { "Idempotency-Key": nextIdempotencyKey("step") } },
        body: { canonicalEventCount: 1 },
      });
      const stepError = expectContractError(stepAfterCompletion, "post", "/v1/runs/{runId}/step", 409);
      expect(stepError.code).toBe("invalid_state_transition");
      expect(stepError.currentRunState).toBe("completed");
    },
    RUN_TIMEOUT_MS,
  );

  test(
    "pause, step and resume another run at event boundaries",
    async () => {
      const run = await createRun(experimentIds[0], "13", true);
      const paused = await waitForRunState(run.runId, new Set(["paused"]));
      expect(paused.allowedActions).toEqual(["resume", "step", "cancel"]);
      const runPath = { runId: run.runId };
      const stepped = expectContract(
        await apiClient.POST("/v1/runs/{runId}/step", { params: { path: runPath, header: { "Idempotency-Key": nextIdempotencyKey("step") } }, body: { canonicalEventCount: 250 } }),
        "post",
        "/v1/runs/{runId}/step",
        200,
      );
      expect(stepped.state).toBe("paused");
      const processedAfterStep = BigInt(stepped.progress.processedCanonicalEventCount);
      expect(processedAfterStep >= 250n).toBe(true);

      const partialSummary = expectContract(await apiClient.GET("/v1/runs/{runId}/summary", { params: { path: runPath } }), "get", "/v1/runs/{runId}/summary", 200);
      expect(partialSummary.availability).toBe("partial");
      const partialEvents = expectContract(
        await apiClient.GET("/v1/runs/{runId}/events", { params: { path: runPath, query: { limit: 1000 } } }),
        "get",
        "/v1/runs/{runId}/events",
        200,
      );
      expect(BigInt(partialEvents.items.length)).toBe(processedAfterStep);
      expectContract(await apiClient.GET("/v1/runs/{runId}/series", { params: { path: runPath } }), "get", "/v1/runs/{runId}/series", 200);
      expectContract(await apiClient.GET("/v1/runs/{runId}/orders", { params: { path: runPath } }), "get", "/v1/runs/{runId}/orders", 200);
      expectContract(await apiClient.GET("/v1/runs/{runId}/fills", { params: { path: runPath } }), "get", "/v1/runs/{runId}/fills", 200);
      expectContract(
        await apiClient.GET("/v1/runs/{runId}/book", { params: { path: runPath, query: { eventPosition: "100", view: "exchange_truth" } } }),
        "get",
        "/v1/runs/{runId}/book",
        200,
      );
      const stillPaused = await readRun(run.runId);
      expect(stillPaused.progress.processedCanonicalEventCount).toBe(stepped.progress.processedCanonicalEventCount);

      const pauseWhilePaused = await apiClient.POST("/v1/runs/{runId}/pause", { params: { path: runPath, header: { "Idempotency-Key": nextIdempotencyKey("pause") } }, body: {} });
      expect(expectContractError(pauseWhilePaused, "post", "/v1/runs/{runId}/pause", 409).code).toBe("invalid_state_transition");

      const resumed = expectContract(
        await apiClient.POST("/v1/runs/{runId}/resume", { params: { path: runPath, header: { "Idempotency-Key": nextIdempotencyKey("resume") } }, body: { reason: "e2e" } }),
        "post",
        "/v1/runs/{runId}/resume",
        200,
      );
      expect(["running", "completed"]).toContain(resumed.state);
      if (resumed.state === "running") {
        const pausedAgain = await apiClient.POST("/v1/runs/{runId}/pause", { params: { path: runPath, header: { "Idempotency-Key": nextIdempotencyKey("pause") } }, body: {} });
        if (pausedAgain.response.status === 200) {
          expect(expectContract(pausedAgain, "post", "/v1/runs/{runId}/pause", 200).state).toBe("paused");
          expectContract(
            await apiClient.POST("/v1/runs/{runId}/resume", { params: { path: runPath, header: { "Idempotency-Key": nextIdempotencyKey("resume") } }, body: {} }),
            "post",
            "/v1/runs/{runId}/resume",
            200,
          );
        } else {
          // The run finished between the two requests.
          expect(expectContractError(pausedAgain, "post", "/v1/runs/{runId}/pause", 409).currentRunState).toBe("completed");
        }
      }
      const finished = await waitForRunState(run.runId, TERMINAL_RUN_STATES);
      expect(finished.state).toBe("completed");
      const transitions = finished.stateHistory.map((transition) => `${transition.fromState ?? "none"}>${transition.toState}`);
      expect(transitions.slice(0, 4)).toEqual(["none>queued", "queued>running", "running>paused", "paused>paused"]);
      expect(transitions.at(-1)).toBe("running>completed");
    },
    RUN_TIMEOUT_MS,
  );

  test(
    "cancel stops a run with a recorded outcome",
    async () => {
      const run = await createRun(experimentIds[1], "13", true);
      await waitForRunState(run.runId, new Set(["paused"]));
      const cancelled = expectContract(
        await apiClient.POST("/v1/runs/{runId}/cancel", { params: { path: { runId: run.runId }, header: { "Idempotency-Key": nextIdempotencyKey("cancel") } }, body: { reason: "e2e cancel" } }),
        "post",
        "/v1/runs/{runId}/cancel",
        200,
      );
      expect(cancelled.state).toBe("cancelled");
      expect(cancelled.terminalDiagnostics?.outcomeCode).toBe("cancelled_by_request");
      expectContract(await apiClient.GET("/v1/runs/{runId}/summary", { params: { path: { runId: run.runId } } }), "get", "/v1/runs/{runId}/summary", 200);
    },
    RUN_TIMEOUT_MS,
  );

  test("idempotent replays return the stored response", async () => {
    const idempotencyKey = nextIdempotencyKey("replay");
    const request = { body: { experimentId: experimentIds[1], masterSeed: PAIRED_MASTER_SEEDS[0], startPaused: false }, params: { header: { "Idempotency-Key": idempotencyKey } } };
    const first = await apiClient.POST("/v1/runs", request);
    const replay = await apiClient.POST("/v1/runs", request);
    expect(expectContract(replay, "post", "/v1/runs", 201).runId).toBe(expectContract(first, "post", "/v1/runs", 201).runId);
    expect(first.response.headers.get("Idempotent-Replayed")).toBe("false");
    expect(replay.response.headers.get("Idempotent-Replayed")).toBe("true");
    const reused = await apiClient.POST("/v1/runs", { body: { experimentId: experimentIds[1], masterSeed: "13", startPaused: false }, params: { header: { "Idempotency-Key": idempotencyKey } } });
    expect(expectContractError(reused, "post", "/v1/runs", 409).code).toBe("idempotency_key_reused_with_different_request");
    const notInSeedSet = await apiClient.POST("/v1/runs", { body: { experimentId: experimentIds[1], masterSeed: "999", startPaused: false }, params: { header: { "Idempotency-Key": nextIdempotencyKey("run") } } });
    expect(expectContractError(notInSeedSet, "post", "/v1/runs", 422).code).toBe("validation_failed");
  });

  test(
    "compare two experiments paired by master seed",
    async () => {
      const pendingRuns = [await createRun(experimentIds[0], PAIRED_MASTER_SEEDS[1]), await createRun(experimentIds[1], PAIRED_MASTER_SEEDS[1])];
      for (const run of pendingRuns) expect((await waitForRunState(run.runId, TERMINAL_RUN_STATES)).state).toBe("completed");
      const allRunsOfB = expectContract(await apiClient.GET("/v1/runs", { params: { query: { experimentId: experimentIds[1] } } }), "get", "/v1/runs", 200);
      for (const run of allRunsOfB.items) await waitForRunState(run.runId, TERMINAL_RUN_STATES);
      const completedRuns = expectContract(await apiClient.GET("/v1/runs", { params: { query: { state: ["completed"], masterSeed: PAIRED_MASTER_SEEDS[1] } } }), "get", "/v1/runs", 200);
      expect(completedRuns.items).toHaveLength(2);

      const comparison = expectContract(
        await apiClient.GET("/v1/comparisons", { params: { query: { baselineExperimentId: experimentIds[0], candidateExperimentId: experimentIds[1], maxPoints: 40 } } }),
        "get",
        "/v1/comparisons",
        200,
      );
      expect(comparison.comparable).toBe(true);
      expect(comparison.seedPairing.pairedSeeds).toEqual(PAIRED_MASTER_SEEDS);
      expect(comparison.metricComparisons.length).toBeGreaterThan(0);
      expect(comparison.assumptionDifferences.length).toBeGreaterThan(0);
      const samePair = await apiClient.GET("/v1/comparisons", { params: { query: { baselineExperimentId: experimentIds[0], candidateExperimentId: experimentIds[0] } } });
      expect(expectContractError(samePair, "get", "/v1/comparisons", 409).code).toBe("comparison_not_paired");
    },
    RUN_TIMEOUT_MS,
  );

  test(
    "a restarted service serves the stored runs",
    async () => {
      expect(await stopApiService(service)).toBe(0);
      service = await startApiService();
      apiClient = createExsimApiClient(service.baseUrl);
      const run = await readRun(completedRunId);
      expect(run.state).toBe("completed");
      const summary = expectContract(await apiClient.GET("/v1/runs/{runId}/summary", { params: { path: { runId: completedRunId } } }), "get", "/v1/runs/{runId}/summary", 200);
      expect(summary.availability).toBe("final");
      const fills = expectContract(await apiClient.GET("/v1/runs/{runId}/fills", { params: { path: { runId: completedRunId } } }), "get", "/v1/runs/{runId}/fills", 200);
      expect(fills.items.length).toBeGreaterThan(0);
    },
    RUN_TIMEOUT_MS,
  );

  test("every contract operation was exercised", () => {
    expect([...observedOperations].sort()).toEqual(
      [
        "get /v1/service",
        "get /v1/baselines",
        "get /v1/connections",
        "post /v1/experiment-previews",
        "post /v1/experiments",
        "get /v1/experiments",
        "get /v1/experiments/{experimentId}",
        "post /v1/runs",
        "get /v1/runs",
        "get /v1/runs/{runId}",
        "post /v1/runs/{runId}/cancel",
        "post /v1/runs/{runId}/pause",
        "post /v1/runs/{runId}/resume",
        "post /v1/runs/{runId}/step",
        "get /v1/runs/{runId}/manifest",
        "get /v1/runs/{runId}/summary",
        "get /v1/runs/{runId}/series",
        "get /v1/runs/{runId}/events",
        "get /v1/runs/{runId}/orders",
        "get /v1/runs/{runId}/orders/{orderId}/lifecycle",
        "get /v1/runs/{runId}/fills",
        "get /v1/runs/{runId}/book",
        "get /v1/comparisons",
      ].sort(),
    );
  });
});
