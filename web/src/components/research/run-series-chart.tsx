"use client";

import { SeriesLineChart } from "@/components/research/series-line-chart";
import type { ApiSchemas } from "@/lib/api/exsim-api-client";
import { buildRunSeriesChartRows } from "@/lib/chart-derived-floats";
import { formatDurationNs, subtractDecimalTexts } from "@/lib/exact-decimal-format";
import { METRIC_UNIT_LABEL, SERIES_LABEL_BY_KIND } from "@/lib/research-labels";

export function RunSeriesChart({ timeSeries, initialCash }: { timeSeries: ApiSchemas["TimeSeries"]; initialCash?: string }) {
  const relative = timeSeries.seriesKind === "equity" && initialCash !== undefined;
  const label = relative ? "Equity change from initial cash" : SERIES_LABEL_BY_KIND[timeSeries.seriesKind];
  const displayedSeries = relative ? { ...timeSeries, points: timeSeries.points.map((point) => ({ ...point, value: point.value === null ? null : subtractDecimalTexts(point.value, initialCash) })) } : timeSeries;
  return (
    <SeriesLineChart
      title={`${label} (${METRIC_UNIT_LABEL[timeSeries.unit]})`}
      description={`${relative ? "Derived display: recorded equity minus initial cash. Valuations remain those of the native ledger. " : timeSeries.seriesKind === "equity" ? "Equity includes initial capital. " : ""}${timeSeries.aggregation.replaceAll("_", " ")}, buckets of ${formatDurationNs(timeSeries.bucketWidthNs)}`}
      rows={buildRunSeriesChartRows(displayedSeries)}
      lines={[{ valueKey: "value", textKey: "valueText", label, color: "var(--chart-1)" }]}
    />
  );
}
