// The only module in the UI that turns API decimal strings into JavaScript
// numbers. Values produced here are approximations for plotting positions; they
// must never be displayed as exact amounts or fed back into arithmetic that is
// shown to the user. Tooltips display the original decimal strings instead.

import type { ApiSchemas } from "@/lib/api/exsim-api-client";
import { shiftDecimalPointLeft } from "@/lib/exact-decimal-format";

export function decimalTextToChartFloat(decimalText: string | null): number | null {
  return decimalText === null ? null : Number(decimalText);
}

export function nanosecondTextToChartSeconds(nanosecondText: string): number {
  return Number(shiftDecimalPointLeft(nanosecondText, 9));
}

export type AlignedChartRow = {
  /** Plot position only (seconds, approximate). */
  simulationTimeSeconds: number;
  /** Exact bucket end for tooltips. */
  bucketEndTimeNs: string;
  baselineValue: number | null;
  candidateValue: number | null;
  /** Exact or derived strings for tooltips. */
  baselineValueText: string | null;
  candidateValueText: string | null;
};

/**
 * Rows for one aligned comparison chart. With `masterSeed` set, plots that seed's
 * pair of runs (exact strings); otherwise plots the cross-seed means (derived).
 */
export function buildAlignedChartRows(alignedSeries: ApiSchemas["AlignedSeries"], masterSeed: string | null): AlignedChartRow[] {
  const baselineValues = masterSeed === null ? alignedSeries.baselineMean : alignedSeries.baselineBySeed.find((series) => series.masterSeed === masterSeed)?.values ?? [];
  const candidateValues = masterSeed === null ? alignedSeries.candidateMean : alignedSeries.candidateBySeed.find((series) => series.masterSeed === masterSeed)?.values ?? [];
  return alignedSeries.bucketEndTimesNs.map((bucketEndTimeNs, bucketIndex) => {
    const baselineValueText = baselineValues[bucketIndex] ?? null;
    const candidateValueText = candidateValues[bucketIndex] ?? null;
    return {
      simulationTimeSeconds: nanosecondTextToChartSeconds(bucketEndTimeNs),
      bucketEndTimeNs,
      baselineValue: decimalTextToChartFloat(baselineValueText),
      candidateValue: decimalTextToChartFloat(candidateValueText),
      baselineValueText,
      candidateValueText,
    };
  });
}

export type RunSeriesChartRow = {
  simulationTimeSeconds: number;
  bucketEndTimeNs: string;
  value: number | null;
  valueText: string | null;
};

export function buildRunSeriesChartRows(timeSeries: ApiSchemas["TimeSeries"]): RunSeriesChartRow[] {
  return timeSeries.points.map((point) => ({
    simulationTimeSeconds: nanosecondTextToChartSeconds(point.bucketEndTimeNs),
    bucketEndTimeNs: point.bucketEndTimeNs,
    value: decimalTextToChartFloat(point.value),
    valueText: point.value,
  }));
}
