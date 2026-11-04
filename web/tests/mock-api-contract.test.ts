import { afterAll, describe, expect, test } from "bun:test";
import { BASELINE_CONFIGURATIONS } from "../mock-server/baseline-catalog";
import { loadOpenApiDocument } from "../mock-server/openapi-schema-validator";
import type { ApiSchemas } from "../src/lib/api/exsim-api-client";
import { createMockApiHarness, expectContractError, expectContractResponse } from "./mock-api-test-harness";

const harness = createMockApiHarness();
const { apiClient } = harness;
const FIXTURE_RUN_ID = "run_fsw2seed303";

function experimentRequestFromBaseline(name: string, quoteWidthTicks = "3"): ApiSchemas["ExperimentCreateRequest"] {
  const config = structuredClone(BASELINE_CONFIGURATIONS[0].config);
  if (config.strategy.strategyKind === "fixed_spread_market_maker") config.strategy.quoteWidthTicks = quoteWidthTicks;
  config.seedSet = { masterSeeds: ["101", "202"] };
  config.bounds.endSimulationTimeNs = "10000000000";
  return { name, derivedFromBaselineId: BASELINE_CONFIGURATIONS[0].baselineId, parentExperimentId: "exp_fixedspreadw2", config };
}

describe("every read operation returns contract-conformant bodies", () => {
  test("service, baselines and experiments", async () => {
    const serviceInfo = expectContractResponse(harness, await apiClient.GET("/v1/service"), "get", "/v1/service", 200);
    expect(serviceInfo.executableStrategyKinds).toContain("fixed_spread_market_maker");
    expectContractResponse(harness, await apiClient.GET("/v1/baselines"), "get", "/v1/baselines", 200);
    const experimentPage = expectContractResponse(harness, await apiClient.GET("/v1/experiments", { params: { query: { limit: 1 } } }), "get", "/v1/experiments", 200);
    expect(experimentPage.items).toHaveLength(1);
    expect(experimentPage.page.nextCursor).not.toBeNull();
    const secondPage = expectContractResponse(harness, await apiClient.GET("/v1/experiments", { params: { query: { limit: 1, cursor: experimentPage.page.nextCursor! } } }), "get", "/v1/experiments", 200);
    expect(secondPage.items[0].experimentId).not.toBe(experimentPage.items[0].experimentId);
    expectContractResponse(harness, await apiClient.GET("/v1/experiments/{experimentId}", { params: { path: { experimentId: "exp_fixedspreadw4" } } }), "get", "/v1/experiments/{experimentId}", 200);
  });

  test("run list filters and paginates", async () => {
    const completedRuns = expectContractResponse(harness, await apiClient.GET("/v1/runs", { params: { query: { experimentId: "exp_fixedspreadw2", state: ["completed"], limit: 3 } } }), "get", "/v1/runs", 200);
    expect(completedRuns.items).toHaveLength(3);
    expect(completedRuns.items.every((run) => run.experimentId === "exp_fixedspreadw2" && run.state === "completed")).toBe(true);
    const seedRuns = expectContractResponse(harness, await apiClient.GET("/v1/runs", { params: { query: { masterSeed: "404" } } }), "get", "/v1/runs", 200);
    expect(seedRuns.items.map((run) => run.runId).sort()).toEqual(["run_fsw2seed404", "run_fsw4seed404"]);
    const mismatchedCursor = await apiClient.GET("/v1/runs", { params: { query: { masterSeed: "101", cursor: completedRuns.page.nextCursor! } } });
    expect(expectContractError(harness, mismatchedCursor, "get", "/v1/runs", 400).code).toBe("invalid_request");
  });

  test("run artifacts", async () => {
    const runPath = { runId: FIXTURE_RUN_ID };
    const run = expectContractResponse(harness, await apiClient.GET("/v1/runs/{runId}", { params: { path: runPath } }), "get", "/v1/runs/{runId}", 200);
    expect(run.state).toBe("completed");
    expect(run.allowedActions).toEqual([]);
    expectContractResponse(harness, await apiClient.GET("/v1/runs/{runId}/manifest", { params: { path: runPath } }), "get", "/v1/runs/{runId}/manifest", 200);
    expectContractResponse(harness, await apiClient.GET("/v1/runs/{runId}/summary", { params: { path: runPath } }), "get", "/v1/runs/{runId}/summary", 200);
    const series = expectContractResponse(
      harness,
      await apiClient.GET("/v1/runs/{runId}/series", { params: { path: runPath, query: { seriesKinds: ["equity", "drawdown"], maxPoints: 20, fromSimulationTimeNs: "10000000000", toSimulationTimeNs: "20000000000" } } }),
      "get",
      "/v1/runs/{runId}/series",
      200,
    );
    expect(series.series.map((timeSeries) => timeSeries.seriesKind)).toEqual(["equity", "drawdown"]);
    for (const timeSeries of series.series) {
      expect(timeSeries.points.length).toBeLessThanOrEqual(20);
      for (const point of timeSeries.points) {
        expect(BigInt(point.bucketEndTimeNs) > 10_000_000_000n && BigInt(point.bucketEndTimeNs) <= 20_000_000_000n).toBe(true);
      }
    }
    const eventPage = expectContractResponse(harness, await apiClient.GET("/v1/runs/{runId}/events", { params: { path: runPath, query: { limit: 50, visibility: "strategy_visible" } } }), "get", "/v1/runs/{runId}/events", 200);
    expect(eventPage.items.every((canonicalEvent) => canonicalEvent.visibility === "strategy_visible")).toBe(true);
    const nextEventPage = expectContractResponse(
      harness,
      await apiClient.GET("/v1/runs/{runId}/events", { params: { path: runPath, query: { limit: 50, visibility: "strategy_visible", cursor: eventPage.page.nextCursor! } } }),
      "get",
      "/v1/runs/{runId}/events",
      200,
    );
    expect(BigInt(nextEventPage.items[0].eventPosition) > BigInt(eventPage.items.at(-1)!.eventPosition)).toBe(true);
    const orderPage = expectContractResponse(harness, await apiClient.GET("/v1/runs/{runId}/orders", { params: { path: runPath, query: { finalStatus: "filled" } } }), "get", "/v1/runs/{runId}/orders", 200);
    expect(orderPage.items.length).toBeGreaterThan(0);
    const lifecycle = expectContractResponse(
      harness,
      await apiClient.GET("/v1/runs/{runId}/orders/{orderId}/lifecycle", { params: { path: { runId: FIXTURE_RUN_ID, orderId: orderPage.items[0].orderId } } }),
      "get",
      "/v1/runs/{runId}/orders/{orderId}/lifecycle",
      200,
    );
    expect(lifecycle.stages.map((stage) => stage.stageKind)).toContain("filled");
    expect(lifecycle.fifoAheadEvidence.length).toBeGreaterThan(0);
    const fillPage = expectContractResponse(harness, await apiClient.GET("/v1/runs/{runId}/fills", { params: { path: runPath, query: { orderId: orderPage.items[0].orderId } } }), "get", "/v1/runs/{runId}/fills", 200);
    expect(fillPage.items.every((fill) => fill.orderId === orderPage.items[0].orderId)).toBe(true);
    for (const view of ["exchange_truth", "strategy_visible"] as const) {
      const book = expectContractResponse(harness, await apiClient.GET("/v1/runs/{runId}/book", { params: { path: runPath, query: { eventPosition: "1500", view, depth: 3 } } }), "get", "/v1/runs/{runId}/book", 200);
      expect(book.bids.length).toBeLessThanOrEqual(3);
      expect(book.source).toBe(view === "exchange_truth" ? "exact_simulated_truth" : "estimated_from_market_data");
    }
  });

  test("comparisons and connections", async () => {
    const comparison = expectContractResponse(
      harness,
      await apiClient.GET("/v1/comparisons", { params: { query: { baselineExperimentId: "exp_fixedspreadw2", candidateExperimentId: "exp_fixedspreadw4", maxPoints: 50 } } }),
      "get",
      "/v1/comparisons",
      200,
    );
    expect(comparison.comparable).toBe(true);
    expect(comparison.assumptionDifferences.map((difference) => difference.fieldPath)).toContain("/strategy/quoteWidthTicks");
    const selfComparison = await apiClient.GET("/v1/comparisons", { params: { query: { baselineExperimentId: "exp_fixedspreadw2", candidateExperimentId: "exp_fixedspreadw2" } } });
    expect(expectContractError(harness, selfComparison, "get", "/v1/comparisons", 409).code).toBe("comparison_not_paired");
    expectContractResponse(harness, await apiClient.GET("/v1/connections"), "get", "/v1/connections", 200);
  });

  test("errors use the uniform error body", async () => {
    const missingRun = await apiClient.GET("/v1/runs/{runId}", { params: { path: { runId: "run_doesnotexist" } } });
    expect(expectContractError(harness, missingRun, "get", "/v1/runs/{runId}", 404).code).toBe("not_found");
    const unprocessedBook = await apiClient.GET("/v1/runs/{runId}/book", { params: { path: { runId: FIXTURE_RUN_ID }, query: { eventPosition: "99999999", view: "exchange_truth" } } });
    expect(expectContractError(harness, unprocessedBook, "get", "/v1/runs/{runId}/book", 409).code).toBe("artifact_not_ready");
    const badSeriesKind = await apiClient.GET("/v1/runs/{runId}/series", { params: { path: { runId: FIXTURE_RUN_ID }, query: { seriesKinds: ["volatility" as ApiSchemas["SeriesKind"]] } } });
    expect(expectContractError(harness, badSeriesKind, "get", "/v1/runs/{runId}/series", 422).fieldIssues[0].fieldPath).toBe("?seriesKinds");
    const wrongSchemaVersion = await apiClient.GET("/v1/service", { headers: { "Exsim-Schema-Version": "exsim.api/2" } });
    expect(expectContractError(harness, wrongSchemaVersion, "get", "/v1/service", 400).code).toBe("unsupported_schema_version");
  });
});

describe("experiment creation", () => {
  test("preview flattens assumptions and marks differences from the baseline", async () => {
    const preview = expectContractResponse(harness, await apiClient.POST("/v1/experiment-previews", { body: experimentRequestFromBaseline("preview") }), "post", "/v1/experiment-previews", 200);
    expect(preview.valid).toBe(true);
    const changedPaths = preview.assumptions.filter((assumption) => assumption.differsFromBaseline).map((assumption) => assumption.fieldPath);
    expect(changedPaths).toContain("/strategy/quoteWidthTicks");
    expect(changedPaths).toContain("/bounds/endSimulationTimeNs");
    expect(preview.assumptions.some((assumption) => assumption.fieldPath === "/latency/cancelLatencyNs")).toBe(true);
  });

  test("semantic problems are reported as field issues", async () => {
    const request = experimentRequestFromBaseline("bad");
    request.config.latency.jitterNs = "5";
    request.config.scenario.backgroundAgents[0].parameters[0].value = "0.5";
    const preview = expectContractResponse(harness, await apiClient.POST("/v1/experiment-previews", { body: request }), "post", "/v1/experiment-previews", 200);
    expect(preview.valid).toBe(false);
    expect(preview.fieldIssues.map((fieldIssue) => fieldIssue.fieldPath)).toEqual(["/config/latency/jitterNs", "/config/scenario/backgroundAgents/0/parameters/0/value"]);
    const creation = await apiClient.POST("/v1/experiments", { body: request, params: { header: { "Idempotency-Key": "create-bad-1" } } });
    expect(expectContractError(harness, creation, "post", "/v1/experiments", 422).code).toBe("validation_failed");
    const malformed = await apiClient.POST("/v1/experiment-previews", { body: { ...request, config: { ...request.config, initialCash: "1e6" } } });
    expect(expectContractError(harness, malformed, "post", "/v1/experiment-previews", 422).fieldIssues.length).toBeGreaterThan(0);
  });

  test("idempotency keys replay the first response and reject a different body", async () => {
    const request = experimentRequestFromBaseline("idempotent");
    const missingKey = await apiClient.POST("/v1/experiments", { body: request, params: { header: {} as { "Idempotency-Key": string } } });
    expect(expectContractError(harness, missingKey, "post", "/v1/experiments", 400).code).toBe("idempotency_key_required");
    const first = await apiClient.POST("/v1/experiments", { body: request, params: { header: { "Idempotency-Key": "create-idem-1" } } });
    const created = expectContractResponse(harness, first, "post", "/v1/experiments", 201);
    expect(first.response.headers.get("Idempotent-Replayed")).toBe("false");
    const replay = await apiClient.POST("/v1/experiments", { body: request, params: { header: { "Idempotency-Key": "create-idem-1" } } });
    expect(expectContractResponse(harness, replay, "post", "/v1/experiments", 201).experimentId).toBe(created.experimentId);
    expect(replay.response.headers.get("Idempotent-Replayed")).toBe("true");
    const conflicting = await apiClient.POST("/v1/experiments", { body: { ...request, name: "other" }, params: { header: { "Idempotency-Key": "create-idem-1" } } });
    expect(expectContractError(harness, conflicting, "post", "/v1/experiments", 409).code).toBe("idempotency_key_reused_with_different_request");
  });
});

describe("run state machine", () => {
  test("queued, running, pause, step, resume, completed, and invalid transitions", async () => {
    const experiment = expectContractResponse(
      harness,
      await apiClient.POST("/v1/experiments", { body: experimentRequestFromBaseline("state machine"), params: { header: { "Idempotency-Key": "create-state-1" } } }),
      "post",
      "/v1/experiments",
      201,
    );
    const createRun = await apiClient.POST("/v1/runs", { body: { experimentId: experiment.experimentId, masterSeed: "101", startPaused: false }, params: { header: { "Idempotency-Key": "run-state-1" } } });
    const queuedRun = expectContractResponse(harness, createRun, "post", "/v1/runs", 201);
    expect(queuedRun.state).toBe("queued");
    expect(queuedRun.allowedActions).toEqual(["cancel"]);
    const runPath = { runId: queuedRun.runId };

    const earlyStep = await apiClient.POST("/v1/runs/{runId}/step", { body: { canonicalEventCount: 5 }, params: { path: runPath, header: { "Idempotency-Key": "step-early-1" } } });
    const earlyStepError = expectContractError(harness, earlyStep, "post", "/v1/runs/{runId}/step", 409);
    expect(earlyStepError.code).toBe("invalid_state_transition");
    expect(earlyStepError.currentRunState).toBe("queued");

    harness.advanceWallClockMs(2000);
    const runningRun = expectContractResponse(harness, await apiClient.GET("/v1/runs/{runId}", { params: { path: runPath } }), "get", "/v1/runs/{runId}", 200);
    expect(runningRun.state).toBe("running");
    expect(BigInt(runningRun.progress.processedCanonicalEventCount) > 0n).toBe(true);
    const partialSummary = expectContractResponse(harness, await apiClient.GET("/v1/runs/{runId}/summary", { params: { path: runPath } }), "get", "/v1/runs/{runId}/summary", 200);
    expect(partialSummary.availability).toBe("partial");

    const pausedRun = expectContractResponse(harness, await apiClient.POST("/v1/runs/{runId}/pause", { body: { reason: "look closer" }, params: { path: runPath, header: { "Idempotency-Key": "pause-1-abc" } } }), "post", "/v1/runs/{runId}/pause", 200);
    expect(pausedRun.allowedActions).toEqual(["resume", "step", "cancel"]);
    harness.advanceWallClockMs(5000);
    const stillPaused = expectContractResponse(harness, await apiClient.GET("/v1/runs/{runId}", { params: { path: runPath } }), "get", "/v1/runs/{runId}", 200);
    expect(stillPaused.progress.processedCanonicalEventCount).toBe(pausedRun.progress.processedCanonicalEventCount);
    const steppedRun = expectContractResponse(harness, await apiClient.POST("/v1/runs/{runId}/step", { body: { canonicalEventCount: 7 }, params: { path: runPath, header: { "Idempotency-Key": "step-1-abcd" } } }), "post", "/v1/runs/{runId}/step", 200);
    expect(BigInt(steppedRun.progress.processedCanonicalEventCount) - BigInt(pausedRun.progress.processedCanonicalEventCount)).toBe(7n);
    expectContractResponse(harness, await apiClient.POST("/v1/runs/{runId}/resume", { params: { path: runPath, header: { "Idempotency-Key": "resume-1-ab" } } }), "post", "/v1/runs/{runId}/resume", 200);
    harness.advanceWallClockMs(60_000);
    const completedRun = expectContractResponse(harness, await apiClient.GET("/v1/runs/{runId}", { params: { path: runPath } }), "get", "/v1/runs/{runId}", 200);
    expect(completedRun.state).toBe("completed");
    expect(completedRun.stateHistory.map((transition) => transition.toState)).toEqual(["queued", "running", "paused", "running", "completed"]);
    expect(completedRun.digests.canonicalEventDigest).not.toBeNull();
    const lateCancel = await apiClient.POST("/v1/runs/{runId}/cancel", { params: { path: runPath, header: { "Idempotency-Key": "cancel-late-1" } } });
    expect(expectContractError(harness, lateCancel, "post", "/v1/runs/{runId}/cancel", 409).code).toBe("invalid_state_transition");
  });

  test("cancel keeps artifacts up to the cancellation point, and seeds must belong to the experiment", async () => {
    const runPath = {
      runId: expectContractResponse(
        harness,
        await apiClient.POST("/v1/runs", { body: { experimentId: "exp_fixedspreadw4", masterSeed: "505", startPaused: true }, params: { header: { "Idempotency-Key": "run-cancel-1" } } }),
        "post",
        "/v1/runs",
        201,
      ).runId,
    };
    harness.advanceWallClockMs(1000);
    const pausedRun = expectContractResponse(harness, await apiClient.GET("/v1/runs/{runId}", { params: { path: runPath } }), "get", "/v1/runs/{runId}", 200);
    expect(pausedRun.state).toBe("paused");
    expectContractResponse(harness, await apiClient.POST("/v1/runs/{runId}/step", { body: { canonicalEventCount: 400 }, params: { path: runPath, header: { "Idempotency-Key": "step-cancel-1" } } }), "post", "/v1/runs/{runId}/step", 200);
    const cancelledRun = expectContractResponse(harness, await apiClient.POST("/v1/runs/{runId}/cancel", { body: { reason: "enough" }, params: { path: runPath, header: { "Idempotency-Key": "cancel-1-abcd" } } }), "post", "/v1/runs/{runId}/cancel", 200);
    expect(cancelledRun.state).toBe("cancelled");
    expect(cancelledRun.terminalDiagnostics?.outcomeCode).toBe("cancelled_by_request");
    const eventPage = expectContractResponse(harness, await apiClient.GET("/v1/runs/{runId}/events", { params: { path: runPath, query: { limit: 1000 } } }), "get", "/v1/runs/{runId}/events", 200);
    expect(eventPage.totalEventCount).toBe("400");
    const summary = expectContractResponse(harness, await apiClient.GET("/v1/runs/{runId}/summary", { params: { path: runPath } }), "get", "/v1/runs/{runId}/summary", 200);
    expect(summary.availability).toBe("final");
    expect(summary.asOfEventPosition).toBe("400");

    const foreignSeed = await apiClient.POST("/v1/runs", { body: { experimentId: "exp_fixedspreadw4", masterSeed: "999", startPaused: false }, params: { header: { "Idempotency-Key": "run-foreign-1" } } });
    expect(expectContractError(harness, foreignSeed, "post", "/v1/runs", 422).fieldIssues[0].fieldPath).toBe("/masterSeed");
  });
});

afterAll(() => {
  // Every operation in the contract must have been exercised above.
  const openApiPaths = loadOpenApiDocument().paths as Record<string, Record<string, unknown>>;
  const contractOperations = Object.entries(openApiPaths).flatMap(([pathTemplate, pathItem]) => Object.keys(pathItem).filter((method) => method === "get" || method === "post").map((method) => `${method} ${pathTemplate}`));
  expect(contractOperations).toHaveLength(23);
  const unexercised = contractOperations.filter((operation) => !harness.observedOperations.has(operation));
  expect(unexercised).toEqual([]);
});
