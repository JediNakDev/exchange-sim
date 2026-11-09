"use client";
import { useApiResource } from "@/hooks/use-api-resource";
import { getBrowserApiClient, unwrapApiResult, type ApiSchemas } from "@/lib/api/exsim-api-client";
import { executionDecisionChartRows } from "@/components/research/execution-trace-data";
import { SeriesLineChart } from "@/components/research/series-line-chart";
import { ApiFailureAlert } from "@/components/research/api-failure-alert";
const LINES = [
  { valueKey: "target", textKey: "targetText", label: "Scheduled target (shares)", color: "var(--chart-2)" },
  { valueKey: "filled", textKey: "filledText", label: "Strategy-known filled (shares)", color: "var(--chart-1)" },
];
export function ExecutionProgressChart({ run }: { run: ApiSchemas["Run"] }) {
  const terminal = ["completed", "failed", "cancelled"].includes(run.state);
  const resource = useApiResource(terminal ? `execution-progress:${run.runId}:${run.state}` : null, async (signal) => unwrapApiResult(await getBrowserApiClient().GET("/v1/runs/{runId}/execution-trace", { params: { path: { runId: run.runId }, query: { limit: 1000 } }, signal })), null);
  if (!terminal) return <p className="border p-4 text-sm text-muted-foreground">Schedule evidence becomes available when this run is terminal. Active simulated time is not parent completion.</p>;
  if (resource.failure) return <ApiFailureAlert failure={resource.failure} title="Execution progress evidence unavailable" onRetry={resource.reload} />;
  if (!resource.data) return <p role="status">Loading execution progress…</p>;
  const trace = resource.data;
  if (!trace.items.some((row) => row.kind === "decision")) return <p className="border p-4 text-sm text-muted-foreground">No decision callbacks in this trace response. This does not imply zero targets or zero fills. Inspect the exact trace and observation status.</p>;
  return <div className="space-y-3"><SeriesLineChart title="Execution progress (shares)" description={`${trace.page.nextCursor ? "First 1,000 trace records only, more evidence exists" : "All recorded decision callbacks"}. Strategy-known fills at decision times, not an exchange-fill path. Observation deadline ${trace.windowEndTimeNs} ns; ${trace.observationStatus}. ${trace.verification}`} rows={executionDecisionChartRows(trace.items)} lines={LINES} /><p className="max-w-prose text-xs text-muted-foreground">A target that advances while known fills stop is a reason to inspect decisions, not proof of why liquidity was missing. Open Explain execution for exact skip reasons and child orders.</p></div>;
}
