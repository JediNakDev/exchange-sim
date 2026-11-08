import type { ApiSchemas, ExsimApiClient } from "@/lib/api/exsim-api-client";
import { unwrapApiResult } from "@/lib/api/exsim-api-client";
import { decimalTextToChartFloat, nanosecondTextToChartSeconds } from "@/lib/chart-derived-floats";

export function executionDecisionChartRows(items: ApiSchemas["ExecutionTraceRow"][]) {
  return items.filter((row) => row.kind === "decision").map((row) => ({
    simulationTimeSeconds: nanosecondTextToChartSeconds(row.simulationTimeNs),
    bucketEndTimeNs: row.simulationTimeNs,
    target: decimalTextToChartFloat(row.cumulativeTargetQuantity),
    targetText: row.cumulativeTargetQuantity,
    filled: decimalTextToChartFloat(row.knownFilledQuantity),
    filledText: row.knownFilledQuantity,
  }));
}

export async function readExecutionTracePage(client: ExsimApiClient, runId: string, cursor: string | undefined, signal: AbortSignal) {
  return unwrapApiResult(await client.GET("/v1/runs/{runId}/execution-trace", {
    params: { path: { runId }, query: { limit: 100, cursor } }, signal,
  }));
}

export function executionOrderLink(runId: string, orderId: string, comparisonQuery: string) {
  const query = new URLSearchParams(comparisonQuery);
  query.set("order", orderId);
  return `/runs/${encodeURIComponent(runId)}/inspect?${query}`;
}
