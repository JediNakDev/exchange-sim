// Mock implementation of docs/api/openapi.yaml for UI development and tests.
// Request handling is a pure function of (request, service state, wall clock)
// so tests can drive it without opening a socket.

import { createHash } from "node:crypto";
import type { components } from "../src/lib/api/generated-schema";
import { BASELINE_CONFIGURATIONS, createFixedSpreadBaselineConfig, FIRST_SLICE_MASTER_SEEDS } from "./baseline-catalog";
import { buildExperimentComparison, flattenExperimentConfig, type CompletedRunForComparison } from "./experiment-comparison";
import { formatDerivedStatistic } from "./fixed-point-decimal";
import { validateAgainstComponentSchema, type SchemaValidationIssue } from "./openapi-schema-validator";
import {
  buildBookView,
  buildFill,
  buildOrderLifecycle,
  buildOwnOrderSummary,
  buildRunSeries,
  buildRunSummary,
  createArtifactCutoff,
} from "./run-artifact-views";
import { SEED_DERIVATION_FUNCTION_DESCRIPTION } from "./seeded-random-streams";
import { PIPELINE_STAGE_ORDER_FOR_EQUAL_TIME, simulateSyntheticMarketRun, type ExperimentConfig, type SimulatedRunArtifacts } from "./synthetic-market-simulation";

type Schemas = components["schemas"];
type RunState = Schemas["RunState"];
type ErrorCode = Schemas["ErrorCode"];

export const API_SCHEMA_VERSION = "exsim.api/1";
const MOCK_BUILD_ID = "exsim-mock-research-service/1";
const QUEUED_DURATION_WALL_MS = 800;
const RUNNING_DURATION_WALL_MS = 8000;
const MOCK_MAX_SIMULATED_DURATION_NS = 600_000_000_000n;
const TERMINAL_RUN_STATES: RunState[] = ["completed", "cancelled", "failed"];

const HTTP_STATUS_BY_ERROR_CODE: Record<ErrorCode, number> = {
  invalid_request: 400,
  validation_failed: 422,
  not_found: 404,
  idempotency_key_required: 400,
  idempotency_key_reused_with_different_request: 409,
  invalid_state_transition: 409,
  action_not_supported_for_mode: 409,
  artifact_not_ready: 409,
  evidence_unavailable: 409,
  not_execution_strategy: 409,
  selected_run_ineligible: 409,
  run_not_terminal: 409,
  artifact_unsafe: 409,
  artifact_too_large: 409,
  artifact_changed: 409,

  unsupported_market_world: 422,
  unsupported_schema_version: 400,
  comparison_not_paired: 409,
  internal_error: 500,
  service_unavailable: 503,
};

export class MockApiError extends Error {
  constructor(
    readonly errorCode: ErrorCode,
    message: string,
    readonly fieldIssues: SchemaValidationIssue[] = [],
    readonly currentRunState?: RunState,
  ) {
    super(message);
  }
}

interface MockExperimentRecord {
  experimentId: string;
  name: string;
  description: string;
  createdAt: string;
  derivedFromBaselineId: string;
  parentExperimentId: string | null;
  config: ExperimentConfig;
  configDigestHex: string;
}

interface MockRunRecord {
  runId: string;
  experimentId: string;
  masterSeed: string;
  createdAtWallMs: number;
  startPaused: boolean;
  state: RunState;
  processedEventCount: number;
  lastProgressWallMs: number;
  stateHistory: Schemas["RunStateTransition"][];
  terminalDiagnostics: Schemas["TerminalDiagnostics"] | null;
}

interface StoredIdempotentResponse {
  requestFingerprint: string;
  status: number;
  body: unknown;
}

function stableStringify(value: unknown): string {
  if (Array.isArray(value)) return `[${value.map(stableStringify).join(",")}]`;
  if (value !== null && typeof value === "object") {
    return `{${Object.keys(value as Record<string, unknown>).sort().map((key) => `${JSON.stringify(key)}:${stableStringify((value as Record<string, unknown>)[key])}`).join(",")}}`;
  }
  return JSON.stringify(value);
}

function sha256Hex(text: string): string {
  return createHash("sha256").update(text).digest("hex");
}

function fnv1a64Hex(text: string): string {
  let hash = 0xcbf29ce484222325n;
  for (const byte of new TextEncoder().encode(text)) {
    hash ^= BigInt(byte);
    hash = (hash * 0x100000001b3n) & 0xffffffffffffffffn;
  }
  return hash.toString(16).padStart(16, "0");
}

function encodeCursor(offset: number, filterFingerprint: string): string {
  return Buffer.from(JSON.stringify({ offset, filterFingerprint })).toString("base64url");
}

function decodeCursor(cursor: string | null, filterFingerprint: string): number {
  if (cursor === null) return 0;
  try {
    const decoded = JSON.parse(Buffer.from(cursor, "base64url").toString("utf8")) as { offset: number; filterFingerprint: string };
    if (decoded.filterFingerprint !== filterFingerprint || !Number.isInteger(decoded.offset) || decoded.offset < 0) throw new Error("mismatch");
    return decoded.offset;
  } catch {
    throw new MockApiError("invalid_request", "cursor is invalid or was issued for different filters");
  }
}

function paginate<T>(items: T[], searchParams: URLSearchParams, filterFingerprint: string): { items: T[]; page: Schemas["PageInfo"] } {
  const limit = parseIntegerParameter(searchParams, "limit", 100, 1, 1000);
  const offset = decodeCursor(searchParams.get("cursor"), filterFingerprint);
  const pageItems = items.slice(offset, offset + limit);
  const nextOffset = offset + pageItems.length;
  return { items: pageItems, page: { nextCursor: nextOffset < items.length ? encodeCursor(nextOffset, filterFingerprint) : null, limit } };
}

function parseIntegerParameter(searchParams: URLSearchParams, name: string, defaultValue: number, minimum: number, maximum: number): number {
  const text = searchParams.get(name);
  if (text === null) return defaultValue;
  if (!/^[0-9]+$/.test(text) || Number(text) < minimum || Number(text) > maximum) {
    throw new MockApiError("validation_failed", `${name} must be an integer in [${minimum}, ${maximum}]`, [{ fieldPath: `?${name}`, issue: "out of range" }]);
  }
  return Number(text);
}

function parseDecimalPositionParameter(searchParams: URLSearchParams, name: string): number | null {
  const text = searchParams.get(name);
  if (text === null) return null;
  if (!/^(0|[1-9][0-9]*)$/.test(text)) throw new MockApiError("validation_failed", `${name} must be an unsigned decimal string`, [{ fieldPath: `?${name}`, issue: "not an unsigned integer string" }]);
  return Number(text);
}

function parseEnumListParameter<T extends string>(searchParams: URLSearchParams, name: string, allowed: readonly T[]): T[] | null {
  const text = searchParams.get(name);
  if (text === null || text === "") return null;
  const values = text.split(",");
  for (const value of values) {
    if (!allowed.includes(value as T)) throw new MockApiError("validation_failed", `${name} contains unknown value ${value}`, [{ fieldPath: `?${name}`, issue: `unknown value ${value}` }]);
  }
  return values as T[];
}

const RUN_STATES: RunState[] = ["queued", "running", "paused", "completed", "cancelled", "failed"];
const SERIES_KINDS: Schemas["SeriesKind"][] = ["equity", "inventory", "exchange_mid", "quoted_spread", "drawdown"];
const EVENT_VISIBILITIES: Schemas["EventVisibility"][] = ["exchange_truth", "strategy_visible"];
const ACTORS: Schemas["Actor"][] = ["research_strategy", "background_market_maker", "background_noise_taker", "exchange", "runner"];
const ORDER_FINAL_STATUSES: Schemas["OrderFinalStatus"][] = ["open_at_run_end", "filled", "canceled", "partially_filled_then_canceled", "rejected"];
const CANONICAL_EVENT_KINDS: Schemas["CanonicalEventKind"][] = [
  "market_data_delivered", "strategy_order_submitted", "strategy_cancel_submitted", "strategy_replace_submitted",
  "order_arrived_at_exchange", "cancel_arrived_at_exchange", "replace_arrived_at_exchange", "order_accepted",
  "order_rejected", "order_rested", "order_filled", "order_canceled", "order_replaced", "execution_report_delivered",
  "risk_limit_blocked_order", "phase_changed", "simulation_bounds_reached",
];

export class MockResearchService {
  private readonly experimentsById = new Map<string, MockExperimentRecord>();
  private readonly runsById = new Map<string, MockRunRecord>();
  private readonly idempotentResponsesByKey = new Map<string, StoredIdempotentResponse>();
  private readonly artifactsByConfigDigestAndSeed = new Map<string, SimulatedRunArtifacts>();
  private nextRequestNumber = 1;

  constructor(private readonly readWallClockMs: () => number = () => Date.now()) {
    this.seedFirstVerticalSliceFixtures();
  }

  // ---------- fixtures ----------

  private seedFirstVerticalSliceFixtures(): void {
    const fixtureCreatedAtMs = Date.parse("2026-11-03T09:00:00.000Z");
    const fixtures = [
      { experimentId: "exp_fixedspreadw2", baselineId: "fixed-spread-2-ticks", width: "2", halfSpreadTicks: "1", name: "Fixed spread 2 ticks (baseline)" },
      { experimentId: "exp_fixedspreadw4", baselineId: "fixed-spread-4-ticks", width: "4", halfSpreadTicks: "2", name: "Fixed spread 4 ticks (candidate)" },
    ];
    fixtures.forEach((fixture, fixtureIndex) => {
      const config = createFixedSpreadBaselineConfig(fixture.halfSpreadTicks);
      this.experimentsById.set(fixture.experimentId, {
        experimentId: fixture.experimentId,
        name: fixture.name,
        description: `First vertical slice: fixed-spread market maker quoting ${fixture.width} ticks wide against a background market maker and noise takers.`,
        createdAt: new Date(fixtureCreatedAtMs + fixtureIndex * 60_000).toISOString(),
        derivedFromBaselineId: fixture.baselineId,
        parentExperimentId: fixtureIndex === 0 ? null : "exp_fixedspreadw2",
        config,
        configDigestHex: sha256Hex(stableStringify(config)),
      });
      for (const masterSeed of FIRST_SLICE_MASTER_SEEDS) {
        const runId = `run_fsw${fixture.width}seed${masterSeed}`;
        const createdAtWallMs = fixtureCreatedAtMs + fixtureIndex * 60_000 + 5_000;
        const artifacts = this.getArtifacts(fixture.experimentId, masterSeed);
        const completedAtEvent = artifacts.events.length;
        const lastEventTime = artifacts.events.at(-1)?.simulationTimeNs ?? "0";
        this.runsById.set(runId, {
          runId,
          experimentId: fixture.experimentId,
          masterSeed,
          createdAtWallMs,
          startPaused: false,
          state: "completed",
          processedEventCount: completedAtEvent,
          lastProgressWallMs: createdAtWallMs + 9_000,
          stateHistory: [
            { fromState: null, toState: "queued", transitionedAt: new Date(createdAtWallMs).toISOString(), atEventPosition: "0", atSimulationTimeNs: "0", reason: "run created" },
            { fromState: "queued", toState: "running", transitionedAt: new Date(createdAtWallMs + 1_000).toISOString(), atEventPosition: "0", atSimulationTimeNs: "0", reason: "worker started" },
            { fromState: "running", toState: "completed", transitionedAt: new Date(createdAtWallMs + 9_000).toISOString(), atEventPosition: String(completedAtEvent), atSimulationTimeNs: lastEventTime, reason: "simulation end time reached" },
          ],
          terminalDiagnostics: this.createTerminalDiagnostics("completed", artifacts, completedAtEvent),
        });
      }
    });
  }

  // ---------- helpers ----------

  private getArtifacts(experimentId: string, masterSeed: string): SimulatedRunArtifacts {
    const experiment = this.requireExperiment(experimentId);
    const cacheKey = `${experiment.configDigestHex}:${masterSeed}`;
    let artifacts = this.artifactsByConfigDigestAndSeed.get(cacheKey);
    if (!artifacts) {
      artifacts = simulateSyntheticMarketRun(experiment.config, masterSeed);
      this.artifactsByConfigDigestAndSeed.set(cacheKey, artifacts);
    }
    return artifacts;
  }

  private requireExperiment(experimentId: string): MockExperimentRecord {
    const experiment = this.experimentsById.get(experimentId);
    if (!experiment) throw new MockApiError("not_found", `experiment ${experimentId} does not exist`);
    return experiment;
  }

  private requireRun(runId: string): MockRunRecord {
    const run = this.runsById.get(runId);
    if (!run) throw new MockApiError("not_found", `run ${runId} does not exist`);
    this.advanceRun(run);
    return run;
  }

  private createTerminalDiagnostics(state: RunState, artifacts: SimulatedRunArtifacts, processedEventCount: number): Schemas["TerminalDiagnostics"] {
    const lastFill = artifacts.ownFills.filter((fill) => fill.eventPosition < processedEventCount).at(-1);
    const openOrders = artifacts.ownOrdersInSubmitOrder.filter((order) => order.restedEventPosition !== null && order.restedEventPosition < processedEventCount && (order.removedFromBookEventPosition === null || order.removedFromBookEventPosition >= processedEventCount)).length;
    return {
      outcomeCode: state === "completed" ? "completed_at_simulation_bound" : state === "cancelled" ? "cancelled_by_request" : "failed_event_bound_exceeded",
      message: state === "completed" ? "Reached the configured simulation end time; residual own orders were cancelled, the position was kept." : state === "cancelled" ? "Cancelled on request; artifacts are complete up to the cancellation point." : "Exceeded the configured canonical event bound.",
      residualPositionShares: String(lastFill?.ledgerAfter.positionShares ?? 0n),
      residualOpenOrderCount: String(openOrders),
    };
  }

  private transitionRun(run: MockRunRecord, toState: RunState, wallMs: number, reason: string): void {
    const artifacts = this.getArtifacts(run.experimentId, run.masterSeed);
    const lastEvent = artifacts.events[run.processedEventCount - 1];
    run.stateHistory.push({
      fromState: run.state,
      toState,
      transitionedAt: new Date(wallMs).toISOString(),
      atEventPosition: String(run.processedEventCount),
      atSimulationTimeNs: lastEvent?.simulationTimeNs ?? "0",
      reason,
    });
    run.state = toState;
    if (TERMINAL_RUN_STATES.includes(toState)) run.terminalDiagnostics = this.createTerminalDiagnostics(toState, artifacts, run.processedEventCount);
  }

  /** Moves a run along its simulated wall-clock schedule up to now. */
  private advanceRun(run: MockRunRecord): void {
    const nowMs = this.readWallClockMs();
    if (run.state === "queued" && nowMs - run.createdAtWallMs >= QUEUED_DURATION_WALL_MS) {
      const startedAtMs = run.createdAtWallMs + QUEUED_DURATION_WALL_MS;
      this.transitionRun(run, "running", startedAtMs, "worker started");
      run.lastProgressWallMs = startedAtMs;
      if (run.startPaused) this.transitionRun(run, "paused", startedAtMs, "started paused for stepping");
    }
    if (run.state !== "running") return;
    const artifacts = this.getArtifacts(run.experimentId, run.masterSeed);
    const totalEvents = artifacts.events.length;
    const eventsPerWallMs = totalEvents / RUNNING_DURATION_WALL_MS;
    const advancedEvents = Math.floor((nowMs - run.lastProgressWallMs) * eventsPerWallMs);
    if (advancedEvents <= 0) return;
    run.processedEventCount = Math.min(totalEvents, run.processedEventCount + advancedEvents);
    run.lastProgressWallMs = nowMs;
    if (run.processedEventCount >= totalEvents) {
      this.transitionRun(run, artifacts.exceededEventBound ? "failed" : "completed", nowMs, artifacts.exceededEventBound ? "canonical event bound exceeded" : "simulation end time reached");
    }
  }

  private allowedActions(run: MockRunRecord): Schemas["RunControlAction"][] {
    switch (run.state) {
      case "queued":
        return ["cancel"];
      case "running":
        return ["pause", "cancel"];
      case "paused":
        return ["resume", "step", "cancel"];
      default:
        return [];
    }
  }

  private presentRun(run: MockRunRecord): Schemas["Run"] {
    const experiment = this.requireExperiment(run.experimentId);
    const artifacts = this.getArtifacts(run.experimentId, run.masterSeed);
    const lastEvent = artifacts.events[run.processedEventCount - 1];
    const isTerminal = TERMINAL_RUN_STATES.includes(run.state);
    const includedEvents = artifacts.events.slice(0, run.processedEventCount);
    const includedFills = artifacts.ownFills.filter((fill) => fill.eventPosition < run.processedEventCount);
    return {
      runId: run.runId,
      experimentId: run.experimentId,
      masterSeed: run.masterSeed,
      mode: "deterministic_research",
      marketWorld: experiment.config.marketWorld,
      state: run.state,
      allowedActions: this.allowedActions(run),
      progress: {
        processedCanonicalEventCount: String(run.processedEventCount),
        simulationTimeNs: lastEvent?.simulationTimeNs ?? "0",
        endSimulationTimeNs: experiment.config.bounds.endSimulationTimeNs,
      },
      stateHistory: run.stateHistory,
      createdAt: new Date(run.createdAtWallMs).toISOString(),
      terminalDiagnostics: isTerminal ? run.terminalDiagnostics : null,
      digests: {
        manifestDigest: { algorithm: "sha256", hexDigest: sha256Hex(stableStringify(this.buildManifest(run))) },
        canonicalEventDigest: isTerminal ? { algorithm: "fnv1a_64", hexDigest: fnv1a64Hex(stableStringify(includedEvents)) } : null,
        ledgerDigest: isTerminal
          ? { algorithm: "fnv1a_64", hexDigest: fnv1a64Hex(includedFills.map((fill) => `${fill.matchId}:${fill.ledgerAfter.positionShares}:${fill.ledgerAfter.cashUnits}:${fill.ledgerAfter.cumulativeFeesUnits}`).join("|")) }
          : null,
      },
      parentRunId: null,
    };
  }

  private presentExperiment(experiment: MockExperimentRecord): Schemas["Experiment"] {
    const runStateCounts: Schemas["RunStateCounts"] = { queued: 0, running: 0, paused: 0, completed: 0, cancelled: 0, failed: 0 };
    for (const run of this.runsById.values()) {
      if (run.experimentId !== experiment.experimentId) continue;
      this.advanceRun(run);
      runStateCounts[run.state]++;
    }
    return {
      experimentId: experiment.experimentId,
      name: experiment.name,
      description: experiment.description,
      createdAt: experiment.createdAt,
      derivedFromBaselineId: experiment.derivedFromBaselineId,
      parentExperimentId: experiment.parentExperimentId,
      marketWorld: experiment.config.marketWorld,
      config: experiment.config,
      configDigest: { algorithm: "sha256", hexDigest: experiment.configDigestHex },
      runStateCounts,
    };
  }

  private buildManifest(run: MockRunRecord): Schemas["RunManifest"] {
    const experiment = this.requireExperiment(run.experimentId);
    const artifacts = this.getArtifacts(run.experimentId, run.masterSeed);
    const config = experiment.config;
    return {
      schemaVersion: "exsim.run-manifest/1",
      runId: run.runId,
      experimentId: run.experimentId,
      parentRunId: null,
      mode: "deterministic_research",
      marketWorld: config.marketWorld,
      engineBuildId: "exsim-engine-mock",
      runnerBuildId: MOCK_BUILD_ID,
      strategyBuildId: `${config.strategy.strategyKind}/mock`,
      configDigest: { algorithm: "sha256", hexDigest: experiment.configDigestHex },
      inputDataDigest: null,
      instrument: config.instrument,
      scenario: config.scenario,
      strategy: config.strategy,
      seeds: {
        masterSeed: run.masterSeed,
        derivationFunction: SEED_DERIVATION_FUNCTION_DESCRIPTION,
        derivedSeeds: artifacts.derivedSeeds.map((derived) => ({ streamName: derived.streamName, seed: String(derived.seed) })),
      },
      latency: config.latency,
      queueModel: "exact_simulated_fifo",
      fees: config.fees,
      risk: config.risk,
      initialCash: config.initialCash,
      initialPositionShares: "0",
      markConvention: config.markConvention,
      markoutHorizonsNs: config.markoutHorizonsNs,
      bounds: config.bounds,
      eventOrderPolicy: { primaryKey: "simulation_time_ns", tieBreaker: "enqueue_sequence", stageOrderForEqualTime: PIPELINE_STAGE_ORDER_FOR_EQUAL_TIME },
      accounting: {
        cashScale: "0.0001",
        costBasisMethod: "fifo",
        feeSign: "positive_paid_negative_rebate",
        equityDefinition: "cash_plus_position_times_mark",
        netPnlDefinition: "equity_minus_initial_cash",
        missingMarkPolicy: "report_unavailable",
      },
      artifactEncodings: {
        canonicalEventEncoding: "exsim.canonical-event/1",
        lifecycleLogEncoding: "exsim.order-lifecycle/1",
        ledgerEncoding: "exsim.ledger/1",
        summaryEncoding: "exsim.run-summary/1",
        seriesEncoding: "exsim.series/1",
      },
      matchingRule: "price_time_fifo",
    };
  }

  private validateExperimentRequest(body: unknown): { request: Schemas["ExperimentCreateRequest"]; semanticIssues: SchemaValidationIssue[] } {
    const schemaIssues = validateAgainstComponentSchema("ExperimentCreateRequest", body);
    if (schemaIssues.length > 0) throw new MockApiError("validation_failed", "request body does not match ExperimentCreateRequest", schemaIssues);
    const request = body as Schemas["ExperimentCreateRequest"];
    const semanticIssues: SchemaValidationIssue[] = [];
    if (!BASELINE_CONFIGURATIONS.some((baseline) => baseline.baselineId === request.derivedFromBaselineId)) {
      semanticIssues.push({ fieldPath: "/derivedFromBaselineId", issue: "unknown baseline" });
    }
    if (request.parentExperimentId !== undefined && !this.experimentsById.has(request.parentExperimentId)) {
      semanticIssues.push({ fieldPath: "/parentExperimentId", issue: "unknown experiment" });
    }
    if (request.config.strategy.strategyKind !== "fixed_spread_market_maker") {
      semanticIssues.push({ fieldPath: "/config/strategy/strategyKind", issue: "not executable by this service build (see /v1/service executableStrategyKinds)" });
    }
    if (request.config.latency.latencyModel === "constant" && request.config.latency.jitterNs !== "0") {
      semanticIssues.push({ fieldPath: "/config/latency/jitterNs", issue: "must be 0 for the constant latency model" });
    }
    if (BigInt(request.config.bounds.endSimulationTimeNs) === 0n || BigInt(request.config.bounds.endSimulationTimeNs) > MOCK_MAX_SIMULATED_DURATION_NS) {
      semanticIssues.push({ fieldPath: "/config/bounds/endSimulationTimeNs", issue: "must be between 1 ns and 600 s of simulated time in the mock service" });
    }
    if (request.config.strategy.strategyKind === "fixed_spread_market_maker" && BigInt(request.config.strategy.halfSpreadTicks) === 0n) {
      semanticIssues.push({ fieldPath: "/config/strategy/halfSpreadTicks", issue: "must be at least 1" });
    }
    if (request.config.instrument.tickSize === "0.0000") semanticIssues.push({ fieldPath: "/config/instrument/tickSize", issue: "must be positive" });
    request.config.scenario.backgroundAgents.forEach((agent, agentIndex) => {
      agent.parameters.forEach((parameter, parameterIndex) => {
        const fieldPath = `/config/scenario/backgroundAgents/${agentIndex}/parameters/${parameterIndex}/value`;
        const requiresUnsignedInteger = parameter.unit === "nanoseconds" || parameter.unit === "shares" || parameter.unit === "count" || parameter.unit === "ticks";
        if (requiresUnsignedInteger && !/^(0|[1-9][0-9]*)$/.test(parameter.value)) semanticIssues.push({ fieldPath, issue: `${parameter.name} must be an unsigned integer in ${parameter.unit}` });
        if (parameter.unit === "ratio" && !/^(0(\.[0-9]+)?|1(\.0+)?)$/.test(parameter.value)) semanticIssues.push({ fieldPath, issue: `${parameter.name} must be a ratio in [0, 1]` });
        if (requiresUnsignedInteger && parameter.unit === "nanoseconds" && parameter.value === "0") semanticIssues.push({ fieldPath, issue: `${parameter.name} must be positive` });
      });
    });
    return { request, semanticIssues };
  }

  private previewExperiment(body: unknown): Schemas["ExperimentPreview"] {
    const { request, semanticIssues } = this.validateExperimentRequest(body);
    const baseline = BASELINE_CONFIGURATIONS.find((candidate) => candidate.baselineId === request.derivedFromBaselineId);
    const baselineValues = new Map(baseline ? flattenExperimentConfig(baseline.config).map((line) => [line.fieldPath, line.value]) : []);
    const durationNs = BigInt(request.config.bounds.endSimulationTimeNs);
    const estimatedEventsPerRun = (durationNs * 60n) / 1_000_000_000n;
    const runCount = request.config.seedSet.masterSeeds.length;
    return {
      valid: semanticIssues.length === 0 && request.config.marketWorld === "interactive_synthetic",
      fieldIssues: request.config.marketWorld === "interactive_synthetic"
        ? semanticIssues
        : [...semanticIssues, { fieldPath: "/config/marketWorld", issue: "only interactive_synthetic is executable in v1" }],
      configDigest: { algorithm: "sha256", hexDigest: sha256Hex(stableStringify(request.config)) },
      assumptions: flattenExperimentConfig(request.config).map((line) => ({
        ...line,
        differsFromBaseline: baselineValues.get(line.fieldPath) !== line.value,
      })),
      estimatedWork: {
        runCount,
        simulatedDurationPerRunNs: String(durationNs),
        estimatedCanonicalEventsPerRun: String(estimatedEventsPerRun),
        estimatedWallClockSecondsTotal: formatDerivedStatistic((runCount * RUNNING_DURATION_WALL_MS) / 1000),
      },
    };
  }

  private createExperiment(body: unknown): Schemas["Experiment"] {
    const { request, semanticIssues } = this.validateExperimentRequest(body);
    if (request.config.marketWorld !== "interactive_synthetic") {
      throw new MockApiError("unsupported_market_world", `market world ${request.config.marketWorld} is not executable in v1`, [{ fieldPath: "/config/marketWorld", issue: "only interactive_synthetic is executable" }]);
    }
    if (semanticIssues.length > 0) throw new MockApiError("validation_failed", "experiment configuration is not executable", semanticIssues);
    const experimentId = `exp_${crypto.randomUUID().replaceAll("-", "").slice(0, 16)}`;
    const record: MockExperimentRecord = {
      experimentId,
      name: request.name,
      description: request.description ?? "",
      createdAt: new Date(this.readWallClockMs()).toISOString(),
      derivedFromBaselineId: request.derivedFromBaselineId,
      parentExperimentId: request.parentExperimentId ?? null,
      config: request.config,
      configDigestHex: sha256Hex(stableStringify(request.config)),
    };
    this.experimentsById.set(experimentId, record);
    return this.presentExperiment(record);
  }

  private createRun(body: unknown): Schemas["Run"] {
    const schemaIssues = validateAgainstComponentSchema("RunCreateRequest", body);
    if (schemaIssues.length > 0) throw new MockApiError("validation_failed", "request body does not match RunCreateRequest", schemaIssues);
    const request = body as Schemas["RunCreateRequest"];
    const experiment = this.requireExperiment(request.experimentId);
    if (!experiment.config.seedSet.masterSeeds.includes(request.masterSeed)) {
      throw new MockApiError("validation_failed", "masterSeed is not in the experiment's seed set", [{ fieldPath: "/masterSeed", issue: "not a member of config.seedSet.masterSeeds" }]);
    }
    const nowMs = this.readWallClockMs();
    const runId = `run_${crypto.randomUUID().replaceAll("-", "").slice(0, 16)}`;
    const run: MockRunRecord = {
      runId,
      experimentId: experiment.experimentId,
      masterSeed: request.masterSeed,
      createdAtWallMs: nowMs,
      startPaused: request.startPaused ?? false,
      state: "queued",
      processedEventCount: 0,
      lastProgressWallMs: nowMs,
      stateHistory: [{ fromState: null, toState: "queued", transitionedAt: new Date(nowMs).toISOString(), atEventPosition: "0", atSimulationTimeNs: "0", reason: "run created" }],
      terminalDiagnostics: null,
    };
    this.runsById.set(runId, run);
    return this.presentRun(run);
  }

  private controlRun(runId: string, action: Schemas["RunControlAction"], body: unknown): Schemas["Run"] {
    const run = this.requireRun(runId);
    if (body !== undefined) {
      const schemaName = action === "step" ? "RunStepRequest" : "RunControlRequest";
      const issues = validateAgainstComponentSchema(schemaName, body);
      if (issues.length > 0) throw new MockApiError("validation_failed", `request body does not match ${schemaName}`, issues);
    } else if (action === "step") {
      throw new MockApiError("validation_failed", "step requires a body", [{ fieldPath: "/canonicalEventCount", issue: "required" }]);
    }
    if (!this.allowedActions(run).includes(action)) {
      throw new MockApiError("invalid_state_transition", `cannot ${action} a run in state ${run.state}`, [], run.state);
    }
    const nowMs = this.readWallClockMs();
    const reason = (body as Schemas["RunControlRequest"] | undefined)?.reason;
    if (action === "cancel") this.transitionRun(run, "cancelled", nowMs, reason ?? "cancelled on request");
    if (action === "pause") this.transitionRun(run, "paused", nowMs, reason ?? "paused on request");
    if (action === "resume") {
      this.transitionRun(run, "running", nowMs, reason ?? "resumed on request");
      run.lastProgressWallMs = nowMs;
    }
    if (action === "step") {
      const artifacts = this.getArtifacts(run.experimentId, run.masterSeed);
      run.processedEventCount = Math.min(artifacts.events.length, run.processedEventCount + (body as Schemas["RunStepRequest"]).canonicalEventCount);
      if (run.processedEventCount >= artifacts.events.length) this.transitionRun(run, "completed", nowMs, "simulation end time reached during step");
    }
    return this.presentRun(run);
  }

  private runCutoff(run: MockRunRecord) {
    const artifacts = this.getArtifacts(run.experimentId, run.masterSeed);
    return { artifacts, cutoff: createArtifactCutoff(artifacts, run.processedEventCount), availability: TERMINAL_RUN_STATES.includes(run.state) ? ("final" as const) : ("partial" as const) };
  }

  private completedRunsBySeed(experimentId: string): Map<string, CompletedRunForComparison> {
    const experiment = this.requireExperiment(experimentId);
    const completedRunsBySeed = new Map<string, CompletedRunForComparison>();
    const runs = [...this.runsById.values()].filter((run) => run.experimentId === experimentId).sort((left, right) => left.createdAtWallMs - right.createdAtWallMs);
    for (const run of runs) {
      this.advanceRun(run);
      if (run.state !== "completed") continue;
      const { artifacts, cutoff } = this.runCutoff(run);
      completedRunsBySeed.set(run.masterSeed, { runId: run.runId, masterSeed: run.masterSeed, artifacts, cutoff, summary: buildRunSummary(run.runId, experiment.config, artifacts, cutoff, "final") });
    }
    return completedRunsBySeed;
  }

  // ---------- routing ----------

  async handleRequest(request: Request): Promise<Response> {
    const requestId = `req_${this.nextRequestNumber++}`;
    try {
      const url = new URL(request.url);
      const method = request.method.toUpperCase();
      const requestedSchemaVersion = request.headers.get("Exsim-Schema-Version");
      if (requestedSchemaVersion !== null && requestedSchemaVersion !== API_SCHEMA_VERSION) {
        throw new MockApiError("unsupported_schema_version", `this service speaks ${API_SCHEMA_VERSION}, not ${requestedSchemaVersion}`, [{ fieldPath: "Exsim-Schema-Version", issue: "unsupported version" }]);
      }
      if (method === "POST") return await this.handleMutatingPost(request, url);
      if (method !== "GET") throw new MockApiError("invalid_request", `method ${method} is not supported`);
      return this.jsonResponse(200, this.routeGet(url));
    } catch (caught) {
      return this.errorResponse(caught, requestId);
    }
  }

  private async readJsonBody(request: Request, required: boolean): Promise<unknown> {
    const text = await request.text();
    if (text.trim() === "") {
      if (required) throw new MockApiError("invalid_request", "a JSON request body is required");
      return undefined;
    }
    try {
      return JSON.parse(text);
    } catch {
      throw new MockApiError("invalid_request", "request body is not valid JSON");
    }
  }

  private async handleMutatingPost(request: Request, url: URL): Promise<Response> {
    const path = url.pathname;
    if (path === "/v1/experiment-previews") {
      return this.jsonResponse(200, this.previewExperiment(await this.readJsonBody(request, true)));
    }
    const idempotencyKey = request.headers.get("Idempotency-Key");
    if (idempotencyKey === null) throw new MockApiError("idempotency_key_required", "this operation requires an Idempotency-Key header");
    if (!/^[A-Za-z0-9_-]{8,128}$/.test(idempotencyKey)) {
      throw new MockApiError("validation_failed", "Idempotency-Key must be 8-128 characters of [A-Za-z0-9_-]", [{ fieldPath: "Idempotency-Key", issue: "invalid format" }]);
    }
    const body = await this.readJsonBody(request, path === "/v1/experiments" || path === "/v1/runs" || path.endsWith("/step"));
    const requestFingerprint = sha256Hex(`${path}\n${stableStringify(body ?? null)}`);
    const stored = this.idempotentResponsesByKey.get(idempotencyKey);
    if (stored) {
      if (stored.requestFingerprint !== requestFingerprint) {
        throw new MockApiError("idempotency_key_reused_with_different_request", "this Idempotency-Key was used for a different request");
      }
      return this.jsonResponse(stored.status, stored.body, { "Idempotent-Replayed": "true" });
    }
    let status = 200;
    let responseBody: unknown;
    const runControlMatch = /^\/v1\/runs\/([^/]+)\/(cancel|pause|resume|step)$/.exec(path);
    if (path === "/v1/experiments") {
      status = 201;
      responseBody = this.createExperiment(body);
    } else if (path === "/v1/runs") {
      status = 201;
      responseBody = this.createRun(body);
    } else if (runControlMatch) {
      responseBody = this.controlRun(runControlMatch[1], runControlMatch[2] as Schemas["RunControlAction"], body);
    } else {
      throw new MockApiError("not_found", `no route for POST ${path}`);
    }
    this.idempotentResponsesByKey.set(idempotencyKey, { requestFingerprint, status, body: responseBody });
    return this.jsonResponse(status, responseBody, { "Idempotent-Replayed": "false" });
  }

  private routeGet(url: URL): unknown {
    const path = url.pathname;
    const searchParams = url.searchParams;
    if (path === "/v1/service") {
      return {
        apiVersion: "v1",
        schemaVersion: API_SCHEMA_VERSION,
        serviceBuildId: MOCK_BUILD_ID,
        engineBuildId: "exsim-engine-mock",
        executableMarketWorlds: ["interactive_synthetic"],
        executableStrategyKinds: ["fixed_spread_market_maker"],
        realTimeProtocolModeAvailable: true,
      } satisfies Schemas["ServiceInfo"];
    }
    if (path === "/v1/baselines") return { items: BASELINE_CONFIGURATIONS } satisfies Schemas["BaselineConfigurationList"];
    if (path === "/v1/experiments") {
      const marketWorld = parseEnumListParameter(searchParams, "marketWorld", ["interactive_synthetic", "historical_reconstruction", "historical_counterfactual_replay"] as const)?.[0] ?? null;
      const experiments = [...this.experimentsById.values()]
        .filter((experiment) => marketWorld === null || experiment.config.marketWorld === marketWorld)
        .sort((left, right) => right.createdAt.localeCompare(left.createdAt))
        .map((experiment) => this.presentExperiment(experiment));
      return paginate(experiments, searchParams, `experiments|${marketWorld ?? ""}`) satisfies Schemas["ExperimentPage"];
    }
    const experimentMatch = /^\/v1\/experiments\/([^/]+)$/.exec(path);
    if (experimentMatch) return this.presentExperiment(this.requireExperiment(experimentMatch[1]));
    if (path === "/v1/runs") {
      const experimentId = searchParams.get("experimentId");
      const states = parseEnumListParameter(searchParams, "state", RUN_STATES);
      const masterSeed = searchParams.get("masterSeed");
      if (experimentId !== null) this.requireExperiment(experimentId);
      const runs = [...this.runsById.values()]
        .map((run) => {
          this.advanceRun(run);
          return run;
        })
        .filter((run) => (experimentId === null || run.experimentId === experimentId) && (states === null || states.includes(run.state)) && (masterSeed === null || run.masterSeed === masterSeed))
        .sort((left, right) => right.createdAtWallMs - left.createdAtWallMs || left.runId.localeCompare(right.runId))
        .map((run) => this.presentRun(run));
      return paginate(runs, searchParams, `runs|${experimentId}|${states?.join(",")}|${masterSeed}`) satisfies Schemas["RunPage"];
    }
    if (path === "/v1/comparisons") {
      const baselineExperimentId = searchParams.get("baselineExperimentId");
      const candidateExperimentId = searchParams.get("candidateExperimentId");
      if (baselineExperimentId === null || candidateExperimentId === null) {
        throw new MockApiError("validation_failed", "baselineExperimentId and candidateExperimentId are required", [
          { fieldPath: "?baselineExperimentId", issue: "required" },
          { fieldPath: "?candidateExperimentId", issue: "required" },
        ]);
      }
      if (baselineExperimentId === candidateExperimentId) {
        throw new MockApiError("comparison_not_paired", "baseline and candidate must be different experiments");
      }
      const maxPoints = parseIntegerParameter(searchParams, "maxPoints", 200, 2, 2000);
      return buildExperimentComparison(
        { experiment: this.presentExperiment(this.requireExperiment(baselineExperimentId)), completedRunsBySeed: this.completedRunsBySeed(baselineExperimentId) },
        { experiment: this.presentExperiment(this.requireExperiment(candidateExperimentId)), completedRunsBySeed: this.completedRunsBySeed(candidateExperimentId) },
        maxPoints,
      );
    }
    if (path === "/v1/connections") return this.buildConnectionsStatus();

    const runMatch = /^\/v1\/runs\/([^/]+)(?:\/(manifest|summary|series|events|orders|fills|book|execution-trace|audit-export))?$/.exec(path);
    const lifecycleMatch = /^\/v1\/runs\/([^/]+)\/orders\/([^/]+)\/lifecycle$/.exec(path);
    if (lifecycleMatch) {
      const run = this.requireRun(lifecycleMatch[1]);
      const { artifacts, cutoff } = this.runCutoff(run);
      const order = artifacts.ownOrdersInSubmitOrder.find((candidate) => String(candidate.orderId) === lifecycleMatch[2]);
      const lifecycle = order ? buildOrderLifecycle(run.runId, order, cutoff) : null;
      if (!lifecycle) throw new MockApiError("not_found", `own order ${lifecycleMatch[2]} is not in this run's processed events`);
      return lifecycle;
    }
    if (!runMatch) throw new MockApiError("not_found", `no route for GET ${path}`);
    const run = this.requireRun(runMatch[1]);
    const experiment = this.requireExperiment(run.experimentId);
    const { artifacts, cutoff, availability } = this.runCutoff(run);
    switch (runMatch[2]) {
      case undefined:
        return this.presentRun(run);
      case "execution-trace":
      case "audit-export":
        throw new MockApiError("evidence_unavailable", "Native execution trace and audit export require the C++ research service; synthetic fixtures cannot verify native artifacts.");
      case "manifest":
        return this.buildManifest(run);
      case "summary":
        return buildRunSummary(run.runId, experiment.config, artifacts, cutoff, availability);
      case "series": {
        const seriesKinds = parseEnumListParameter(searchParams, "seriesKinds", SERIES_KINDS) ?? SERIES_KINDS;
        const latestTimeNs = BigInt(artifacts.events[run.processedEventCount - 1]?.simulationTimeNs ?? "0");
        const defaultToTimeNs = availability === "final" ? artifacts.endTimeNs : latestTimeNs;
        const fromTimeNs = BigInt(parseDecimalPositionParameter(searchParams, "fromSimulationTimeNs") ?? 0);
        const toText = searchParams.get("toSimulationTimeNs");
        if (toText !== null && !/^(0|[1-9][0-9]*)$/.test(toText)) throw new MockApiError("validation_failed", "toSimulationTimeNs must be an unsigned decimal string", [{ fieldPath: "?toSimulationTimeNs", issue: "invalid" }]);
        const toTimeNs = toText === null ? defaultToTimeNs : BigInt(toText);
        if (toTimeNs <= fromTimeNs) throw new MockApiError("validation_failed", "toSimulationTimeNs must be after fromSimulationTimeNs", [{ fieldPath: "?toSimulationTimeNs", issue: "must exceed fromSimulationTimeNs" }]);
        return buildRunSeries(run.runId, artifacts, cutoff, availability, seriesKinds, fromTimeNs, toTimeNs, parseIntegerParameter(searchParams, "maxPoints", 500, 2, 5000));
      }
      case "events": {
        const fromEventPosition = parseDecimalPositionParameter(searchParams, "fromEventPosition") ?? 0;
        const toEventPosition = parseDecimalPositionParameter(searchParams, "toEventPosition");
        const eventKinds = parseEnumListParameter(searchParams, "eventKinds", CANONICAL_EVENT_KINDS);
        const visibility = parseEnumListParameter(searchParams, "visibility", EVENT_VISIBILITIES)?.[0] ?? null;
        const actor = parseEnumListParameter(searchParams, "actor", ACTORS)?.[0] ?? null;
        const orderId = searchParams.get("orderId");
        const matching = artifacts.events.slice(0, run.processedEventCount).filter((event) => {
          const position = Number(event.eventPosition);
          return position >= fromEventPosition
            && (toEventPosition === null || position <= toEventPosition)
            && (eventKinds === null || eventKinds.includes(event.eventKind))
            && (visibility === null || event.visibility === visibility)
            && (actor === null || event.actor === actor)
            && (orderId === null || event.orderId === orderId);
        });
        const fingerprint = `events|${fromEventPosition}|${toEventPosition}|${eventKinds?.join(",")}|${visibility}|${actor}|${orderId}`;
        return { runId: run.runId, totalEventCount: String(run.processedEventCount), ...paginate(matching, searchParams, fingerprint) } satisfies Schemas["CanonicalEventPage"];
      }
      case "orders": {
        const finalStatus = parseEnumListParameter(searchParams, "finalStatus", ORDER_FINAL_STATUSES)?.[0] ?? null;
        const orders = artifacts.ownOrdersInSubmitOrder
          .map((order) => buildOwnOrderSummary(order, cutoff))
          .filter((summary): summary is Schemas["OwnOrderSummary"] => summary !== null && (finalStatus === null || summary.finalStatus === finalStatus));
        return { runId: run.runId, ...paginate(orders, searchParams, `orders|${finalStatus}`) } satisfies Schemas["OwnOrderPage"];
      }
      case "fills": {
        const orderId = searchParams.get("orderId");
        const fills = artifacts.ownFills
          .filter((fill) => fill.eventPosition < run.processedEventCount && (orderId === null || String(fill.ownOrder.orderId) === orderId))
          .map((fill) => buildFill(fill, artifacts, experiment.config, cutoff));
        return { runId: run.runId, ...paginate(fills, searchParams, `fills|${orderId}`) } satisfies Schemas["FillPage"];
      }
      case "book": {
        const eventPosition = parseDecimalPositionParameter(searchParams, "eventPosition");
        const view = parseEnumListParameter(searchParams, "view", EVENT_VISIBILITIES)?.[0] ?? null;
        if (eventPosition === null || view === null) {
          throw new MockApiError("validation_failed", "eventPosition and view are required", [{ fieldPath: "?eventPosition", issue: "required" }, { fieldPath: "?view", issue: "required" }]);
        }
        if (eventPosition >= run.processedEventCount) {
          throw new MockApiError("artifact_not_ready", `event ${eventPosition} has not been processed yet`, [], run.state);
        }
        return buildBookView(run.runId, artifacts, eventPosition, view, parseIntegerParameter(searchParams, "depth", 5, 1, 50));
      }
      default:
        throw new MockApiError("not_found", `no route for GET ${path}`);
    }
  }

  private buildConnectionsStatus(): Schemas["ConnectionsStatus"] {
    const nowMs = this.readWallClockMs();
    const minutesAgo = (minutes: number) => new Date(nowMs - minutes * 60_000).toISOString();
    return {
      mode: "real_time_protocol",
      serviceState: "running",
      observedAt: new Date(nowMs).toISOString(),
      telemetryDroppedUpdateCount: "12",
      sessions: [
        { sessionId: "1", protocol: "ouch_4_2_soupbintcp", username: "MMAKER01", state: "logged_in", remoteAddress: "127.0.0.1:51722", connectedAt: minutesAgo(42), lastInboundAt: minutesAgo(0), inboundSequenceNumber: "18342", outboundSequenceNumber: "36110", heartbeatIntervalSeconds: 1, missedHeartbeatCount: 0, liveOrderCount: "4", cancelOnDisconnect: true },
        { sessionId: "2", protocol: "fix_4_4", username: "EXECALGO", state: "recovering", remoteAddress: "127.0.0.1:51731", connectedAt: minutesAgo(3), lastInboundAt: minutesAgo(1), inboundSequenceNumber: "905", outboundSequenceNumber: "1211", heartbeatIntervalSeconds: 30, missedHeartbeatCount: 1, liveOrderCount: "0", cancelOnDisconnect: false },
        { sessionId: "3", protocol: "ouch_4_2_soupbintcp", username: "TAKER02", state: "disconnected", remoteAddress: null, connectedAt: null, lastInboundAt: minutesAgo(17), inboundSequenceNumber: "220", outboundSequenceNumber: "431", heartbeatIntervalSeconds: 1, missedHeartbeatCount: 3, liveOrderCount: "0", cancelOnDisconnect: true },
      ],
      feeds: [
        {
          feedId: "itch-a",
          protocol: "itch_5_0_moldudp64",
          multicastGroup: "239.1.1.1:26400",
          mold64Session: "EXSIM00001",
          lastPublishedSequenceNumber: "1048211",
          retransmissionRequestCount: "3",
          recentGaps: [
            { fromSequenceNumber: "1047990", toSequenceNumber: "1047994", detectedAt: minutesAgo(6), recoveryState: "recovered" },
            { fromSequenceNumber: "1048200", toSequenceNumber: "1048203", detectedAt: minutesAgo(0), recoveryState: "retransmission_requested" },
          ],
        },
      ],
      faults: [
        { faultId: "fault-drop-1", faultKind: "drop_packets", state: "active", target: "itch-a", description: "Drop 2% of MoldUDP64 packets", reproducibilitySeed: "7", activatedAt: minutesAgo(8), clearedAt: null },
        { faultId: "fault-disconnect-1", faultKind: "disconnect_session", state: "cleared", target: "session 3", description: "Disconnect TAKER02 once to exercise cancel-on-disconnect", reproducibilitySeed: null, activatedAt: minutesAgo(18), clearedAt: minutesAgo(17) },
        { faultId: "fault-halt-1", faultKind: "halt_instrument", state: "scheduled", target: "EXSM", description: "Halt EXSM for 30 s at 14:30 session time", reproducibilitySeed: null, activatedAt: null, clearedAt: null },
      ],
    };
  }

  private jsonResponse(status: number, body: unknown, extraHeaders: Record<string, string> = {}): Response {
    return new Response(JSON.stringify(body), {
      status,
      headers: { "Content-Type": "application/json; charset=utf-8", "Exsim-Schema-Version": API_SCHEMA_VERSION, ...extraHeaders },
    });
  }

  private errorResponse(caught: unknown, requestId: string): Response {
    const apiError = caught instanceof MockApiError ? caught : new MockApiError("internal_error", caught instanceof Error ? caught.message : "unexpected error");
    const body: Schemas["ErrorResponseBody"] = {
      error: {
        code: apiError.errorCode,
        message: apiError.message,
        requestId,
        retryable: apiError.errorCode === "service_unavailable" || apiError.errorCode === "internal_error",
        fieldIssues: apiError.fieldIssues,
        ...(apiError.currentRunState ? { currentRunState: apiError.currentRunState } : {}),
      },
    };
    return this.jsonResponse(HTTP_STATUS_BY_ERROR_CODE[apiError.errorCode], body);
  }
}
