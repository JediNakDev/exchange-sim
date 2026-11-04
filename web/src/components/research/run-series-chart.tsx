"use client";

import { SeriesLineChart } from "@/components/research/series-line-chart";
import type { ApiSchemas } from "@/lib/api/exsim-api-client";
import { buildRunSeriesChartRows } from "@/lib/chart-derived-floats";
import { formatDurationNs } from "@/lib/exact-decimal-format";
import { METRIC_UNIT_LABEL, SERIES_LABEL_BY_KIND } from "@/lib/research-labels";

export function RunSeriesChart({ timeSeries }: { timeSeries: ApiSchemas["TimeSeries"] }) {
  return (
    <SeriesLineChart
      title={`${SERIES_LABEL_BY_KIND[timeSeries.seriesKind]} (${METRIC_UNIT_LABEL[timeSeries.unit]})`}
      description={`${timeSeries.aggregation.replaceAll("_", " ")}, buckets of ${formatDurationNs(timeSeries.bucketWidthNs)}`}
      rows={buildRunSeriesChartRows(timeSeries)}
      lines={[{ valueKey: "value", textKey: "valueText", label: SERIES_LABEL_BY_KIND[timeSeries.seriesKind], color: "var(--chart-1)" }]}
    />
  );
}
