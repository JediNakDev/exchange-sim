"use client";

import { DefinitionList } from "@/components/research/definition-list";

import { useState } from "react";
import Link from "next/link";
import { ApiFailureAlert } from "@/components/research/api-failure-alert";
import { ArtifactPageControls } from "@/components/research/artifact-page-controls";
import { useComparisonContext } from "@/components/research/run-research-context";
import { SeriesLineChart } from "@/components/research/series-line-chart";
import { Button } from "@/components/ui/button";
import { ResearchSection } from "@/components/research/research-section";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { useApiResource } from "@/hooks/use-api-resource";
import { useCursorHistory } from "@/hooks/use-cursor-history";
import { getBrowserApiClient, type ApiSchemas } from "@/lib/api/exsim-api-client";
import { executionDecisionChartRows, executionOrderLink, readExecutionTracePage } from "./execution-trace-data";

const TRACE_LINES = [
  { valueKey: "target", textKey: "targetText", label: "Cumulative target due (shares)", color: "var(--chart-1)" },
  { valueKey: "filled", textKey: "filledText", label: "Known filled (shares)", color: "var(--chart-2)" },
];

// Strings below remain the service's exact values, including ns, IDs and prices.
function ExactCell({ value }: { value: string | null }) {
  return <TableCell className="text-right font-mono text-xs tabular-nums">{value ?? "Unavailable"}</TableCell>;
}

export function ExecutionTraceEvidence({ trace, run, comparisonQuery }: {
  trace: ApiSchemas["ExecutionTracePage"];
  run: ApiSchemas["Run"];
  comparisonQuery: string;
}) {
  const decisions = trace.items.filter((row) => row.kind === "decision");
  const reports = trace.items.filter((row) => row.kind === "child_report");
  return <div className="space-y-4">
    <p className="text-sm font-medium" role="status">{trace.verification === "recorded_terminal_partial"
      ? "Recorded terminal partial evidence. Not reproduction verified."
      : trace.verification === "native_reproduction_and_replay_verified"
        ? "Native reproduction and independent replay verified."
        : "Verification unavailable; no reproduction claim."} <code>{trace.verification ?? "unavailable"}</code></p>
    <DefinitionList className="grid-cols-[minmax(0,1fr)_minmax(0,1fr)]" rows={[
        ["Run", trace.runId], ["Run state", run.state], ["Master seed", run.masterSeed],
        ["Parent quantity (shares)", trace.parentQuantity], ["Start (ns)", trace.startTimeNs],
        ["Final target (shares)", trace.finalTargetQuantity], ["Final target due (ns)", trace.finalTargetTimeNs],
        ["Observation deadline (ns)", trace.windowEndTimeNs], ["Observation status", trace.observationStatus],
        ["Total trace records in run", trace.totalCount],
      ]} />
    <p className="text-xs text-muted-foreground">{trace.observationStatus === "window_observed"
      ? "Full execution window observed. Residual shares can remain despite a completed simulation."
      : trace.observationStatus === "censored_run_end"
        ? "Censored at run end before the observation deadline. This is not a full-window execution outcome."
        : "Full-window observation is not established by this status."} The final target is due at the scheduled time above; the deadline is not an extra final retry. TWAP does not retry after its final scheduled slice.</p>
    <p className="text-xs text-muted-foreground">Current page only: {trace.items.length} exact records ({decisions.length} decisions, {reports.length} delivered child reports) out of {trace.totalCount} total. Tables and chart do not imply coverage of the entire run. Each decision is a scheduled TWAP callback or sweep retry on delivered data. Target due, known fills, outstanding reservation and residual are strategy-known shares at that callback, not final exchange fills. Requested shares are the positive target deficit before cap/lot rounding; headroom is worst-case same-side position capacity. Visible touch and source time refer only to delivered market data.</p>
    {decisions.length ? <SeriesLineChart title="Execution decisions on this page (shares)" description="Current-page decision callbacks only, in simulated time; no interpolation to the final target or observation deadline. Child reports are excluded" rows={executionDecisionChartRows(trace.items)} lines={TRACE_LINES} /> : <p>No decision rows on this page. No target/fill chart is inferred from child reports.</p>}
    <div className="min-w-0 overflow-x-auto border bg-card">
      <Table aria-label="Exact execution decisions on current page"><TableHeader><TableRow>
        {["Index", "Callback (ns)", "Target due (shares)", "Known filled", "Parent residual", "Outstanding", "Requested", "Child cap", "Position headroom", "Submitted", "Reason / skip", "Client ID", "Source sequence", "Source time (ns)", "Visible touch (USD)", "Limit (USD)"].map((label) => <TableHead key={label}>{label}</TableHead>)}
      </TableRow></TableHeader><TableBody>{decisions.map((row) => <TableRow key={row.index}>
        {[row.index, row.simulationTimeNs, row.cumulativeTargetQuantity, row.knownFilledQuantity, row.parentResidualQuantity, row.outstandingQuantity, row.requestedQuantity, row.childCapQuantity, row.positionHeadroom, row.submittedQuantity, row.reason, row.childClientOrderId, row.deliveredSourceSequence, row.deliveredSourceTimeNs, row.deliveredTouch, row.limitPrice].map((value, index) => <ExactCell key={index} value={value} />)}
      </TableRow>)}</TableBody></Table>
    </div>
    {!decisions.length ? <p className="text-xs">No decisions on this page.</p> : null}
    <h3 className="text-sm font-medium">Delivered child reports on this page</h3>
    <p className="text-xs text-muted-foreground">Delivery time differs from exchange time. Request-only zero/null fields on these reports are unused and excluded from the decision chart. Decision rows identify client ID and callback time; reports link to the exchange lifecycle when an order ID is available.</p>
    <div className="min-w-0 overflow-x-auto border bg-card">
      <Table aria-label="Exact delivered child reports on current page"><TableHeader><TableRow>
        {["Index", "Delivered (ns)", "Client ID", "Report kind", "Exchange (ns)", "Exchange order / lifecycle", "Match ID", "Quantity", "Leaves", "Cumulative", "Reject reason", "Cancel reason", "Trace reason"].map((label) => <TableHead key={label}>{label}</TableHead>)}
      </TableRow></TableHeader><TableBody>{reports.map((row) => <TableRow key={row.index}>
        <ExactCell value={row.index} /><ExactCell value={row.simulationTimeNs} /><ExactCell value={row.childClientOrderId} />
        <ExactCell value={row.report?.kind ?? null} /><ExactCell value={row.report?.exchangeTimeNs ?? null} />
        <TableCell className="font-mono text-xs">{row.report && row.report.orderId !== "0" ? <Link className="underline" href={executionOrderLink(run.runId, row.report.orderId, comparisonQuery)}>{row.report.orderId}</Link> : "Unavailable; use client ID / delivery time"}</TableCell>
        {[row.report?.matchId ?? null, row.report?.quantity ?? null, row.report?.leavesQuantity ?? null, row.report?.cumulativeQuantity ?? null, row.report?.rejectReason ?? null, row.report?.cancelReason ?? null, row.reason].map((value, index) => <ExactCell key={index} value={value} />)}
      </TableRow>)}</TableBody></Table>
    </div>
    {!reports.length ? <p className="text-xs">No delivered child reports on this page.</p> : null}
    {!trace.items.length ? <p role="status">This trace page is empty. Refresh to retry, or use Previous to return to a visited page.</p> : null}
  </div>;
}

function TerminalExecutionTrace({ run }: { run: ApiSchemas["Run"] }) {
  const comparisonQuery = useComparisonContext();
  const history = useCursorHistory();
  // Refresh changes identity, so useApiResource clears old evidence while loading
  // and on failure rather than presenting retained data as a fresh response.
  const [refresh, setRefresh] = useState(0);
  const resource = useApiResource(`execution-trace:${run.runId}:${run.state}:${history.cursor ?? "first"}:${refresh}`,
    (signal) => readExecutionTracePage(getBrowserApiClient(), run.runId, history.cursor, signal), null);
  return <>
    <Button size="sm" variant="outline" disabled={resource.isLoading} onClick={() => setRefresh((value) => value + 1)}>Refresh execution trace</Button>
    <p className="text-xs text-muted-foreground">One page per request, up to 100 records. No automatic refresh. Refresh replaces this page; previous evidence is hidden until the request succeeds.</p>
    {resource.isLoading ? <p role="status">Loading execution trace page; no current evidence available.</p> : null}
    {resource.failure ? <ApiFailureAlert title="Execution trace unavailable" failure={resource.failure} onRetry={() => setRefresh((value) => value + 1)} /> : null}
    {resource.data ? <ExecutionTraceEvidence trace={resource.data} run={run} comparisonQuery={comparisonQuery} /> : null}
    {resource.data || resource.isLoading ? <ArtifactPageControls label="Execution trace" pageNumber={history.pageNumber} count={resource.data?.items.length ?? 0} nextCursor={resource.data?.page.nextCursor ?? null} isLoading={resource.isLoading} onPrevious={history.previous} onNext={history.next} /> : <div className="space-y-2">
      <p className="text-xs">Page {history.pageNumber} has no current response. End of trace is unknown.</p>
      <Button size="sm" variant="outline" disabled={history.pageNumber === 1} onClick={history.previous}>Previous execution trace</Button>
    </div>}
  </>;
}

export function ExecutionTracePanel({ run }: { run: ApiSchemas["Run"] }) {
  const terminal = run.state === "completed" || run.state === "cancelled" || run.state === "failed";
  return <ResearchSection className="xl:col-span-2" title="Native execution trace" description={<> Schedule decisions and delivered reports explaining parent-order residuals. </>}>
<div className="space-y-4">{terminal ? <TerminalExecutionTrace key={`${run.runId}:${run.state}`} run={run} /> : <p role="status">Execution trace is available only after the run reaches a terminal state. Resume a paused run or let the active run finish, then refresh run status. Active runs do not have reproduction-verified trace evidence.</p>}</div>
  </ResearchSection>;
}
