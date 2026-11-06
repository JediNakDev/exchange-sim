// Mock-only: assumption listing and paired seed comparison. Per-seed
// differences are exact decimal subtraction; dispersion statistics are
// derived floats rounded half-to-even to 6 digits, as the contract states.

import type { components } from "../src/lib/api/generated-schema";
import { compareExactDecimals, exactDecimalToDerivedFloat, formatDerivedStatistic, subtractExactDecimals } from "./fixed-point-decimal";
import { buildFullResolutionSeries, chooseBucketWidthNs, downsampleLastValueInBucket, SERIES_UNIT_BY_KIND, type ArtifactCutoff } from "./run-artifact-views";
import type { ExperimentConfig, SimulatedRunArtifacts } from "./synthetic-market-simulation";

type Schemas = components["schemas"];

const CATEGORY_BY_TOP_LEVEL_KEY: Record<keyof ExperimentConfig, Schemas["AssumptionCategory"]> = {
  marketWorld: "market_world",
  instrument: "instrument",
  scenario: "scenario",
  strategy: "strategy",
  seedSet: "seeds",
  latency: "latency",
  fees: "fees",
  risk: "risk",
  initialCash: "accounting",
  markConvention: "accounting",
  markoutHorizonsNs: "accounting",
  bounds: "bounds",
};

function inferUnitFromFieldName(fieldName: string): string | null {
  if (fieldName.endsWith("Ns") || fieldName.endsWith("NsList")) return "ns";
  if (fieldName.endsWith("Ticks")) return "ticks";
  if (fieldName.endsWith("Seconds")) return "s";
  if (fieldName.endsWith("FeePerShare") || fieldName.endsWith("Price") || fieldName === "tickSize") return "currency/share";
  if (fieldName === "initialCash") return "currency";
  if (fieldName.endsWith("Shares") || fieldName.endsWith("Quantity") || fieldName === "lotSize") return "shares";
  return null;
}

const UNIT_LABEL_BY_PARAMETER_UNIT: Record<Schemas["ModelParameter"]["unit"], string> = {
  ticks: "ticks",
  shares: "shares",
  nanoseconds: "ns",
  ratio: "ratio",
  count: "count",
  currency: "currency",
};

export interface FlattenedAssumption {
  category: Schemas["AssumptionCategory"];
  fieldPath: string;
  label: string;
  value: string;
  unit: string | null;
}

/** Flattens a configuration into display lines keyed by JSON pointer; decimal strings pass through unchanged. */
export function flattenExperimentConfig(config: ExperimentConfig): FlattenedAssumption[] {
  const lines: FlattenedAssumption[] = [];
  const visit = (value: unknown, pathSegments: string[], category: Schemas["AssumptionCategory"]): void => {
    const fieldPath = `/${pathSegments.join("/")}`;
    const fieldName = pathSegments[pathSegments.length - 1];
    if (Array.isArray(value) && value.every((item) => typeof item !== "object")) {
      lines.push({ category, fieldPath, label: pathSegments.join("."), value: value.join(", "), unit: inferUnitFromFieldName(fieldName) });
      return;
    }
    if (Array.isArray(value)) {
      value.forEach((item, index) => visit(item, [...pathSegments, String(index)], category));
      return;
    }
    if (value !== null && typeof value === "object") {
      const record = value as Record<string, unknown>;
      if ("agentKind" in record && "parameters" in record) {
        const agent = record as unknown as Schemas["BackgroundAgentConfig"];
        lines.push({ category, fieldPath: `${fieldPath}/agentCount`, label: `${agent.agentKind}.agentCount`, value: String(agent.agentCount), unit: "count" });
        agent.parameters.forEach((parameter, parameterIndex) => {
          lines.push({
            category,
            fieldPath: `${fieldPath}/parameters/${parameterIndex}/value`,
            label: `${agent.agentKind}.${parameter.name}`,
            value: parameter.value,
            unit: UNIT_LABEL_BY_PARAMETER_UNIT[parameter.unit],
          });
        });
        return;
      }
      for (const [key, child] of Object.entries(record)) visit(child, [...pathSegments, key], category);
      return;
    }
    lines.push({ category, fieldPath, label: pathSegments.join("."), value: String(value), unit: inferUnitFromFieldName(fieldName) });
  };
  for (const [topLevelKey, value] of Object.entries(config) as [keyof ExperimentConfig, unknown][]) {
    if (topLevelKey === "scenario") {
      const scenario = value as Schemas["MarketScenarioConfig"];
      lines.push({ category: "scenario", fieldPath: "/scenario/scenarioId", label: "scenario.scenarioId", value: scenario.scenarioId, unit: null });
      scenario.backgroundAgents.forEach((agent, agentIndex) => visit(agent, ["scenario", "backgroundAgents", String(agentIndex)], "scenario"));
      continue;
    }
    visit(value, [topLevelKey], CATEGORY_BY_TOP_LEVEL_KEY[topLevelKey]);
  }
  return lines;
}

export function listAssumptionDifferences(baselineConfig: ExperimentConfig, candidateConfig: ExperimentConfig): Schemas["AssumptionDifference"][] {
  const baselineByPath = new Map(flattenExperimentConfig(baselineConfig).map((line) => [line.fieldPath, line]));
  const candidateByPath = new Map(flattenExperimentConfig(candidateConfig).map((line) => [line.fieldPath, line]));
  const allPaths = [...new Set([...baselineByPath.keys(), ...candidateByPath.keys()])];
  const differences: Schemas["AssumptionDifference"][] = [];
  for (const fieldPath of allPaths) {
    const baselineLine = baselineByPath.get(fieldPath);
    const candidateLine = candidateByPath.get(fieldPath);
    if (baselineLine?.value === candidateLine?.value) continue;
    const referenceLine = (candidateLine ?? baselineLine)!;
    differences.push({
      category: referenceLine.category,
      fieldPath,
      label: referenceLine.label,
      baselineValue: baselineLine?.value ?? "(absent)",
      candidateValue: candidateLine?.value ?? "(absent)",
      unit: referenceLine.unit,
    });
  }
  return differences;
}

export const BETTER_DIRECTION_BY_METRIC: Record<Schemas["MetricKey"], Schemas["MetricComparison"]["betterDirection"]> = {
  completion_ratio: "higher",
  residual_qty: "lower",
  decision_slippage_total: "lower",
  decision_slippage_per_share: "lower",
  implementation_shortfall_total: "lower",
  implementation_shortfall_per_share: "lower",
  participation_rate: "neither",
  net_pnl: "higher",
  equity: "higher",
  gross_realized_pnl: "higher",
  unrealized_pnl: "higher",
  net_fees: "lower",
  ending_position: "neither",
  max_long_position: "neither",
  max_short_position: "neither",
  max_drawdown: "lower",
  fill_count: "neither",
  filled_shares: "neither",
  maker_fill_count: "neither",
  taker_fill_count: "neither",
  fill_rate: "neither",
  submitted_order_count: "neither",
  canceled_order_count: "neither",
  cancel_race_lost_count: "lower",
  spread_capture_total: "higher",
  markout_total: "higher",
  quote_exposure_time: "neither",
};

export interface CompletedRunForComparison {
  runId: string;
  masterSeed: string;
  summary: Schemas["RunSummary"];
  artifacts: SimulatedRunArtifacts;
  cutoff: ArtifactCutoff;
}

export interface ExperimentForComparison {
  experiment: Schemas["Experiment"];
  completedRunsBySeed: Map<string, CompletedRunForComparison>;
}

function metricIdentity(metricValue: Schemas["MetricValue"]): string {
  return `${metricValue.metricKey}@${metricValue.horizonNs ?? ""}`;
}

export function computeDifferenceDispersion(differences: string[], betterDirection: Schemas["MetricComparison"]["betterDirection"]): Schemas["DifferenceDispersion"] {
  const sampleCount = differences.length;
  const derivedDifferences = differences.map(exactDecimalToDerivedFloat);
  const mean = sampleCount === 0 ? null : derivedDifferences.reduce((total, value) => total + value, 0) / sampleCount;
  const sampleVariance = sampleCount < 2 || mean === null
    ? null
    : derivedDifferences.reduce((total, value) => total + (value - mean) ** 2, 0) / (sampleCount - 1);
  const sampleStandardDeviation = sampleVariance === null ? null : Math.sqrt(sampleVariance);
  let minimumDifference: string | null = null;
  let maximumDifference: string | null = null;
  let positiveCount = 0;
  let negativeCount = 0;
  for (const difference of differences) {
    if (minimumDifference === null || compareExactDecimals(difference, minimumDifference) < 0) minimumDifference = difference;
    if (maximumDifference === null || compareExactDecimals(difference, maximumDifference) > 0) maximumDifference = difference;
    const sign = compareExactDecimals(difference, "0");
    if (sign > 0) positiveCount++;
    if (sign < 0) negativeCount++;
  }
  const tiedCount = sampleCount - positiveCount - negativeCount;
  return {
    sampleCount,
    meanDifference: mean === null ? null : formatDerivedStatistic(mean),
    sampleStandardDeviation: sampleStandardDeviation === null ? null : formatDerivedStatistic(sampleStandardDeviation),
    standardErrorOfMean: sampleStandardDeviation === null ? null : formatDerivedStatistic(sampleStandardDeviation / Math.sqrt(sampleCount)),
    minimumDifference,
    maximumDifference,
    candidateBetterCount: betterDirection === "higher" ? positiveCount : betterDirection === "lower" ? negativeCount : 0,
    candidateWorseCount: betterDirection === "higher" ? negativeCount : betterDirection === "lower" ? positiveCount : 0,
    tiedCount: betterDirection === "neither" ? sampleCount : tiedCount,
  };
}

function toExperimentReference(entry: ExperimentForComparison): Schemas["ExperimentReference"] {
  return {
    experimentId: entry.experiment.experimentId,
    name: entry.experiment.name,
    marketWorld: entry.experiment.marketWorld,
    configDigest: entry.experiment.configDigest,
    completedSeeds: [...entry.completedRunsBySeed.keys()].sort((left, right) => compareExactDecimals(left, right)),
  };
}

const ALIGNED_SERIES_KINDS: Schemas["SeriesKind"][] = ["equity", "inventory", "drawdown"];

export function buildExperimentComparison(baseline: ExperimentForComparison, candidate: ExperimentForComparison, maxPoints: number): Schemas["ExperimentComparison"] {
  const baselineReference = toExperimentReference(baseline);
  const candidateReference = toExperimentReference(candidate);
  const pairedSeeds = baselineReference.completedSeeds.filter((seed) => candidate.completedRunsBySeed.has(seed));
  const incomparabilityReasons: string[] = [];
  if (baseline.experiment.marketWorld !== candidate.experiment.marketWorld) {
    incomparabilityReasons.push(`market worlds differ: ${baseline.experiment.marketWorld} vs ${candidate.experiment.marketWorld}`);
  }
  if (pairedSeeds.length === 0) incomparabilityReasons.push("no master seed has a completed run in both experiments");

  const metricComparisons: Schemas["MetricComparison"][] = [];
  const firstPairedSeed = pairedSeeds[0];
  const metricTemplates = firstPairedSeed ? baseline.completedRunsBySeed.get(firstPairedSeed)!.summary.metrics : [];
  for (const template of metricTemplates) {
    const identity = metricIdentity(template);
    const betterDirection = BETTER_DIRECTION_BY_METRIC[template.metricKey];
    const perSeed: Schemas["PairedSeedMetric"][] = pairedSeeds.map((masterSeed) => {
      const baselineRun = baseline.completedRunsBySeed.get(masterSeed)!;
      const candidateRun = candidate.completedRunsBySeed.get(masterSeed)!;
      const baselineValue = baselineRun.summary.metrics.find((value) => metricIdentity(value) === identity)?.value ?? null;
      const candidateValue = candidateRun.summary.metrics.find((value) => metricIdentity(value) === identity)?.value ?? null;
      return {
        masterSeed,
        baselineRunId: baselineRun.runId,
        candidateRunId: candidateRun.runId,
        baselineValue,
        candidateValue,
        difference: baselineValue === null || candidateValue === null ? null : subtractExactDecimals(candidateValue, baselineValue),
      };
    });
    const availableDifferences = perSeed.flatMap((pair) => (pair.difference === null ? [] : [pair.difference]));
    metricComparisons.push({
      metricKey: template.metricKey,
      horizonNs: template.horizonNs,
      unit: template.unit,
      exactness: template.exactness,
      betterDirection,
      perSeed,
      dispersion: computeDifferenceDispersion(availableDifferences, betterDirection),
    });
  }

  const allRuns = [...baseline.completedRunsBySeed.values(), ...candidate.completedRunsBySeed.values()];
  const alignedEndTimeNs = allRuns.reduce((latest, run) => (run.artifacts.endTimeNs > latest ? run.artifacts.endTimeNs : latest), 0n);
  const bucketWidthNs = chooseBucketWidthNs(0n, alignedEndTimeNs, maxPoints);
  const alignedSeries: Schemas["AlignedSeries"][] = ALIGNED_SERIES_KINDS.map((seriesKind) => {
    const alignRun = (run: CompletedRunForComparison): Schemas["AlignedSeedSeries"] => {
      const points = downsampleLastValueInBucket(buildFullResolutionSeries(seriesKind, run.artifacts, run.cutoff), 0n, alignedEndTimeNs, bucketWidthNs);
      return {
        masterSeed: run.masterSeed,
        runId: run.runId,
        values: points.map((point) => (BigInt(point.bucketEndTimeNs) > run.cutoff.observableUntilTimeNs ? null : point.value)),
      };
    };
    const baselineBySeed = pairedSeeds.map((seed) => alignRun(baseline.completedRunsBySeed.get(seed)!));
    const candidateBySeed = pairedSeeds.map((seed) => alignRun(candidate.completedRunsBySeed.get(seed)!));
    const bucketEndTimesNs = downsampleLastValueInBucket([], 0n, alignedEndTimeNs, bucketWidthNs).map((point) => point.bucketEndTimeNs);
    const crossSeedMean = (seriesBySeed: Schemas["AlignedSeedSeries"][]) =>
      bucketEndTimesNs.map((_, bucketIndex) => {
        const values = seriesBySeed.flatMap((seedSeries) => (seedSeries.values[bucketIndex] === null ? [] : [exactDecimalToDerivedFloat(seedSeries.values[bucketIndex]!)]));
        return values.length === 0 ? null : formatDerivedStatistic(values.reduce((total, value) => total + value, 0) / values.length);
      });
    return {
      seriesKind,
      unit: SERIES_UNIT_BY_KIND[seriesKind],
      aggregation: "last_value_in_bucket",
      bucketEndTimesNs,
      baselineBySeed,
      candidateBySeed,
      baselineMean: crossSeedMean(baselineBySeed),
      candidateMean: crossSeedMean(candidateBySeed),
    };
  });

  return {
    schemaVersion: "exsim.comparison/1",
    baseline: baselineReference,
    candidate: candidateReference,
    comparable: incomparabilityReasons.length === 0,
    incomparabilityReasons,
    seedPairing: {
      pairedSeeds,
      baselineOnlySeeds: baselineReference.completedSeeds.filter((seed) => !candidate.completedRunsBySeed.has(seed)),
      candidateOnlySeeds: candidateReference.completedSeeds.filter((seed) => !baseline.completedRunsBySeed.has(seed)),
    },
    assumptionDifferences: listAssumptionDifferences(baseline.experiment.config, candidate.experiment.config),
    metricComparisons,
    alignedSeries,
  };
}
