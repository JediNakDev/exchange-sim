// Recomputes the fixture comparison (fixed spread 2 ticks vs 4 ticks) from the
// per-run summaries: per-seed values must be the runs' own metrics, differences
// must be exact, and the dispersion statistics must match an independent
// calculation. Derived statistics are floats, so they are checked to 1e-6.

import { beforeAll, describe, expect, test } from "bun:test";
import { FIRST_SLICE_MASTER_SEEDS } from "../mock-server/baseline-catalog";
import type { ApiSchemas } from "../src/lib/api/exsim-api-client";
import { compareDecimalTexts, decimalTextSign, subtractDecimalTexts } from "../src/lib/exact-decimal-format";
import { createMockApiHarness, expectContractResponse } from "./mock-api-test-harness";

const harness = createMockApiHarness();
const { apiClient } = harness;
const DERIVED_STATISTIC_TOLERANCE = 1e-6;
const BASELINE_EXPERIMENT_ID = "exp_fixedspreadw2";
const CANDIDATE_EXPERIMENT_ID = "exp_fixedspreadw4";

let comparison: ApiSchemas["ExperimentComparison"];
const summaryByRunId = new Map<string, ApiSchemas["RunSummary"]>();

function findSummaryMetric(summary: ApiSchemas["RunSummary"], metricKey: string, horizonNs: string | null): ApiSchemas["MetricValue"] {
  const metric = summary.metrics.find((candidateMetric) => candidateMetric.metricKey === metricKey && candidateMetric.horizonNs === horizonNs);
  if (!metric) throw new Error(`run ${summary.runId} has no metric ${metricKey} (${horizonNs})`);
  return metric;
}

function expectDerivedClose(actualText: string | null, expectedValue: number | null): void {
  if (expectedValue === null) {
    expect(actualText).toBeNull();
    return;
  }
  expect(actualText).not.toBeNull();
  expect(Math.abs(Number(actualText) - expectedValue)).toBeLessThanOrEqual(DERIVED_STATISTIC_TOLERANCE * Math.max(1, Math.abs(expectedValue)));
}

beforeAll(async () => {
  comparison = expectContractResponse(
    harness,
    await apiClient.GET("/v1/comparisons", { params: { query: { baselineExperimentId: BASELINE_EXPERIMENT_ID, candidateExperimentId: CANDIDATE_EXPERIMENT_ID } } }),
    "get",
    "/v1/comparisons",
    200,
  );
  const runIds = new Set(comparison.metricComparisons.flatMap((metricComparison) => metricComparison.perSeed.flatMap((pairedSeed) => [pairedSeed.baselineRunId, pairedSeed.candidateRunId])));
  for (const runId of runIds) {
    summaryByRunId.set(runId, expectContractResponse(harness, await apiClient.GET("/v1/runs/{runId}/summary", { params: { path: { runId } } }), "get", "/v1/runs/{runId}/summary", 200));
  }
});

describe("fixture comparison", () => {
  test("pairs all five seeds and is comparable", () => {
    expect(comparison.comparable).toBe(true);
    expect(comparison.incomparabilityReasons).toEqual([]);
    expect([...comparison.seedPairing.pairedSeeds].sort()).toEqual([...FIRST_SLICE_MASTER_SEEDS].sort());
    expect(comparison.seedPairing.baselineOnlySeeds).toEqual([]);
    expect(comparison.seedPairing.candidateOnlySeeds).toEqual([]);
    expect(comparison.metricComparisons.length).toBeGreaterThan(5);
    expect(comparison.assumptionDifferences.some((difference) => difference.category === "strategy")).toBe(true);
  });

  test("per-seed values are the runs' own summary metrics and differences are exact", () => {
    for (const metricComparison of comparison.metricComparisons) {
      expect(metricComparison.perSeed.map((pairedSeed) => pairedSeed.masterSeed).sort()).toEqual([...comparison.seedPairing.pairedSeeds].sort());
      for (const pairedSeed of metricComparison.perSeed) {
        const baselineMetric = findSummaryMetric(summaryByRunId.get(pairedSeed.baselineRunId)!, metricComparison.metricKey, metricComparison.horizonNs);
        const candidateMetric = findSummaryMetric(summaryByRunId.get(pairedSeed.candidateRunId)!, metricComparison.metricKey, metricComparison.horizonNs);
        expect(pairedSeed.baselineValue).toBe(baselineMetric.value);
        expect(pairedSeed.candidateValue).toBe(candidateMetric.value);
        expect(metricComparison.unit).toBe(baselineMetric.unit);
        if (pairedSeed.baselineValue === null || pairedSeed.candidateValue === null) {
          expect(pairedSeed.difference).toBeNull();
        } else {
          expect(pairedSeed.difference).not.toBeNull();
          expect(compareDecimalTexts(pairedSeed.difference!, subtractDecimalTexts(pairedSeed.candidateValue, pairedSeed.baselineValue))).toBe(0);
        }
      }
    }
  });

  test("dispersion statistics match an independent recomputation", () => {
    for (const metricComparison of comparison.metricComparisons) {
      const differenceTexts = metricComparison.perSeed.flatMap((pairedSeed) => (pairedSeed.difference === null ? [] : [pairedSeed.difference]));
      const dispersion = metricComparison.dispersion;
      expect(dispersion.sampleCount).toBe(differenceTexts.length);

      const differenceValues = differenceTexts.map((differenceText) => Number(differenceText));
      const sampleCount = differenceValues.length;
      const mean = sampleCount === 0 ? null : differenceValues.reduce((total, value) => total + value, 0) / sampleCount;
      const sampleStandardDeviation =
        sampleCount < 2 || mean === null ? null : Math.sqrt(differenceValues.reduce((total, value) => total + (value - mean) ** 2, 0) / (sampleCount - 1));
      const standardErrorOfMean = sampleStandardDeviation === null ? null : sampleStandardDeviation / Math.sqrt(sampleCount);
      expectDerivedClose(dispersion.meanDifference, mean);
      expectDerivedClose(dispersion.sampleStandardDeviation, sampleStandardDeviation);
      expectDerivedClose(dispersion.standardErrorOfMean, standardErrorOfMean);

      const sortedDifferences = [...differenceTexts].sort(compareDecimalTexts);
      if (sampleCount === 0) {
        expect(dispersion.minimumDifference).toBeNull();
        expect(dispersion.maximumDifference).toBeNull();
      } else {
        expect(compareDecimalTexts(dispersion.minimumDifference!, sortedDifferences[0])).toBe(0);
        expect(compareDecimalTexts(dispersion.maximumDifference!, sortedDifferences[sampleCount - 1])).toBe(0);
      }

      let candidateBetterCount = 0;
      let candidateWorseCount = 0;
      let tiedCount = 0;
      for (const differenceText of differenceTexts) {
        const sign = decimalTextSign(differenceText);
        if (sign === 0 || metricComparison.betterDirection === "neither") tiedCount++;
        else if ((sign > 0) === (metricComparison.betterDirection === "higher")) candidateBetterCount++;
        else candidateWorseCount++;
      }
      expect({ metricKey: metricComparison.metricKey, candidateBetterCount, candidateWorseCount, tiedCount }).toEqual({
        metricKey: metricComparison.metricKey,
        candidateBetterCount: dispersion.candidateBetterCount,
        candidateWorseCount: dispersion.candidateWorseCount,
        tiedCount: dispersion.tiedCount,
      });
    }
  });

  test("aligned series share one bucket grid and the means average the paired seeds", () => {
    expect(comparison.alignedSeries.length).toBeGreaterThan(0);
    for (const alignedSeries of comparison.alignedSeries) {
      const bucketCount = alignedSeries.bucketEndTimesNs.length;
      expect(bucketCount).toBeGreaterThan(1);
      for (let bucketIndex = 1; bucketIndex < bucketCount; bucketIndex++) {
        expect(BigInt(alignedSeries.bucketEndTimesNs[bucketIndex])).toBeGreaterThan(BigInt(alignedSeries.bucketEndTimesNs[bucketIndex - 1]));
      }
      const sides = [
        { seedSeries: alignedSeries.baselineBySeed, meanValues: alignedSeries.baselineMean },
        { seedSeries: alignedSeries.candidateBySeed, meanValues: alignedSeries.candidateMean },
      ];
      for (const { seedSeries, meanValues } of sides) {
        expect(seedSeries.map((series) => series.masterSeed).sort()).toEqual([...comparison.seedPairing.pairedSeeds].sort());
        expect(meanValues.length).toBe(bucketCount);
        for (const series of seedSeries) expect(series.values.length).toBe(bucketCount);
        for (let bucketIndex = 0; bucketIndex < bucketCount; bucketIndex++) {
          const availableValues = seedSeries.flatMap((series) => (series.values[bucketIndex] === null ? [] : [Number(series.values[bucketIndex])]));
          const expectedMean = availableValues.length === 0 ? null : availableValues.reduce((total, value) => total + value, 0) / availableValues.length;
          expectDerivedClose(meanValues[bucketIndex], expectedMean);
        }
      }
    }
  });
});
