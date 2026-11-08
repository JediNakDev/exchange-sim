"use client";

import Link from "next/link";
import { useParams } from "next/navigation";
import { useRef, useState } from "react";
import { toast } from "sonner";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card";
import { Input } from "@/components/ui/input";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs";
import { ApiFailureAlert } from "@/components/research/api-failure-alert";
import { PageHeading } from "@/components/research/page-heading";
import { isTerminalRunState, RunStateBadge } from "@/components/research/run-state-badge";
import { ExecutionSummaryPanel } from "@/components/research/execution-summary-panel";
import { RunResearchContext, useComparisonContext } from "@/components/research/run-research-context";
import { ExecutionTracePanel } from "@/components/research/execution-trace-panel";
import { RequestDiagnosticsPanel } from "@/components/research/request-diagnostics-panel";
import { RunAuditControls } from "@/components/research/run-audit-controls";
import { TraderRunOutcomes } from "@/components/research/trader-run-outcomes";
import { RunSeriesChart } from "@/components/research/run-series-chart";
import { validStepEventCount } from "@/lib/api/research-workflow";
import { useApiResource } from "@/hooks/use-api-resource";
import { createIdempotencyKey, describeApiFailure, getBrowserApiClient, unwrapApiResult, type ApiSchemas } from "@/lib/api/exsim-api-client";
import { formatDecimalForDisplay, formatDurationNs, formatIntegerText, formatSimulationTimeNs, formatWallClockTime } from "@/lib/exact-decimal-format";
import { formatMetricValue, humanizeEnumValue, metricLabel, METRIC_UNIT_LABEL } from "@/lib/research-labels";

const ACTIVE_RUN_POLL_INTERVAL_MS = 1000;
const DEFAULT_STEP_EVENT_COUNT = "100";


function DefinitionRows({ rows }: { rows: [string, React.ReactNode][] }) {
  return (
    <dl className="grid grid-cols-[max-content_minmax(0,1fr)] gap-x-4 gap-y-1 text-sm">
      {rows.map(([label, value]) => (
        <div key={label} className="contents">
          <dt className="text-muted-foreground">{label}</dt>
          <dd className="font-mono text-xs break-all tabular-nums">{value}</dd>
        </div>
      ))}
    </dl>
  );
}

function RunControls({ run, onRunChanged, stale }: { run: ApiSchemas["Run"]; onRunChanged: () => void; stale: boolean }) {
  const apiClient = getBrowserApiClient();
  const [stepEventCountText, setStepEventCountText] = useState(DEFAULT_STEP_EVENT_COUNT);
  const [controlFailure, setControlFailure] = useState<ApiSchemas["ApiError"] | null>(null);
  const inFlight = useRef(false);
  const retryRequest = useRef<{ fingerprint: string; key: string } | null>(null);
  const [pendingAction, setPendingAction] = useState<ApiSchemas["RunControlAction"] | null>(null);

  const sendControl = async (action: ApiSchemas["RunControlAction"]) => {
    if (stale || inFlight.current || (action === "step" && !validStepEventCount(stepEventCountText))) return;
    inFlight.current = true;
    setPendingAction(action);
    setControlFailure(null);
    const fingerprint = JSON.stringify([run.runId, action, action === "step" ? stepEventCountText : null]);
    if (retryRequest.current?.fingerprint !== fingerprint) retryRequest.current = { fingerprint, key: createIdempotencyKey(action) };
    const requestOptions = { params: { path: { runId: run.runId }, header: { "Idempotency-Key": retryRequest.current.key } } };
    try {
      let updatedRun: ApiSchemas["Run"];
      if (action === "step") {
        const canonicalEventCount = Number.parseInt(stepEventCountText, 10);
        updatedRun = unwrapApiResult(await apiClient.POST("/v1/runs/{runId}/step", { ...requestOptions, body: { canonicalEventCount } }));
      } else if (action === "pause") {
        updatedRun = unwrapApiResult(await apiClient.POST("/v1/runs/{runId}/pause", { ...requestOptions, body: { reason: "paused from the research UI" } }));
      } else if (action === "resume") {
        updatedRun = unwrapApiResult(await apiClient.POST("/v1/runs/{runId}/resume", { ...requestOptions, body: { reason: "resumed from the research UI" } }));
      } else {
        updatedRun = unwrapApiResult(await apiClient.POST("/v1/runs/{runId}/cancel", { ...requestOptions, body: { reason: "cancelled from the research UI" } }));
      }
      retryRequest.current = null;
      toast.success(`${action} accepted; run is ${updatedRun.state}`);
    } catch (caught) {
      const failure = describeApiFailure(caught);
      setControlFailure(failure);
      if (!failure.retryable) retryRequest.current = null;
    } finally {
      inFlight.current = false;
      setPendingAction(null);
      onRunChanged();
    }
  };

  if (run.allowedActions.length === 0) return null;
  return (
    <div className="flex flex-col gap-2">
      <fieldset disabled={stale} className="flex flex-wrap items-center gap-2">
        {run.allowedActions.includes("pause") ? (
          <Button size="sm" variant="outline" disabled={pendingAction !== null} onClick={() => void sendControl("pause")}>
            Pause
          </Button>
        ) : null}
        {run.allowedActions.includes("resume") ? (
          <Button size="sm" variant="outline" disabled={pendingAction !== null} onClick={() => void sendControl("resume")}>
            Resume
          </Button>
        ) : null}
        {run.allowedActions.includes("step") ? (
          <form
            className="flex items-center gap-1"
            onSubmit={(submitEvent) => {
              submitEvent.preventDefault();
              void sendControl("step");
            }}
          >
            <Input aria-label="Events to step" className="h-8 w-24 font-mono" inputMode="numeric" disabled={pendingAction !== null} value={stepEventCountText} onChange={(changeEvent) => setStepEventCountText(changeEvent.target.value)} />
            <Button size="sm" variant="outline" type="submit" disabled={pendingAction !== null || !validStepEventCount(stepEventCountText)}>
              Step events
            </Button>
          </form>
        ) : null}
        {run.allowedActions.includes("cancel") ? (
          <Button size="sm" variant="destructive" disabled={pendingAction !== null} onClick={() => void sendControl("cancel")}>
            Cancel
          </Button>
        ) : null}
      </fieldset>
      {controlFailure ? <ApiFailureAlert failure={controlFailure} title="Control request rejected" /> : null}
    </div>
  );
}

function SummaryPanel({ summary }: { summary: ApiSchemas["RunSummary"] }) {
  const accounting = summary.accounting;
  return (
    <div className="grid min-w-0 grid-cols-1 gap-4 xl:grid-cols-2">
      <ExecutionSummaryPanel summary={summary} />
      <div className="min-w-0 xl:col-span-2"><RequestDiagnosticsPanel diagnostics={summary.orders.requestDiagnostics} /></div>
      <Card size="sm">
        <CardHeader>
          <CardTitle>Ledger</CardTitle>
          <CardDescription>
            Equity = cash + position x mark ({humanizeEnumValue(accounting.markConvention)}). Net P&L = realized + unrealized - fees.
          </CardDescription>
        </CardHeader>
        <CardContent>
          <DefinitionRows
            rows={[
              ["Initial cash", formatDecimalForDisplay(accounting.initialCash)],
              ["Cash", formatDecimalForDisplay(accounting.cash)],
              ["Position (shares)", formatDecimalForDisplay(accounting.positionShares)],
              ["Mark", formatDecimalForDisplay(accounting.markPrice)],
              ["Equity", formatDecimalForDisplay(accounting.equity)],
              ["Gross realized P&L", formatDecimalForDisplay(accounting.grossRealizedPnl, { signDisplay: "always" })],
              ["Unrealized P&L", formatDecimalForDisplay(accounting.unrealizedPnl, { signDisplay: "always" })],
              ["Net fees paid", formatDecimalForDisplay(accounting.netFees)],
              ["Net P&L", formatDecimalForDisplay(accounting.netPnl, { signDisplay: "always" })],
              ["Average cost basis (derived)", formatDecimalForDisplay(accounting.averageCostBasis)],
              ["Spread capture total", formatDecimalForDisplay(summary.spreadCaptureTotal, { signDisplay: "always" })],
            ]}
          />
          {accounting.markUnavailableReason ? <p className="mt-2 text-xs text-muted-foreground">{accounting.markUnavailableReason}</p> : null}
        </CardContent>
      </Card>
      <Card size="sm">
        <CardHeader>
          <CardTitle>Orders and risk</CardTitle>
          <CardDescription>{summary.orders.fillRateByShares.definition}</CardDescription>
        </CardHeader>
        <CardContent>
          <DefinitionRows
            rows={[
              ["New orders submitted / accepted / rejected", `${summary.orders.submittedOrderCount} / ${summary.orders.acceptedOrderCount} / ${summary.orders.rejectedOrderCount}`],
              ["Canceled (cancel races lost)", `${summary.orders.canceledOrderCount} (${summary.orders.cancelRaceLostCount})`],
              ["Fills (maker / taker)", `${summary.orders.fillCount} (${summary.orders.makerFillCount} / ${summary.orders.takerFillCount})`],
              ["Filled / submitted shares", `${formatIntegerText(summary.orders.filledShares)} / ${formatIntegerText(summary.orders.submittedShares)}`],
              ["Fill rate by shares (derived)", formatDecimalForDisplay(summary.orders.fillRateByShares.ratio)],
              ["Quote exposure time", formatDurationNs(summary.orders.quoteExposureTimeNs)],
              ["Max long / short (shares)", `${summary.risk.maxLongPositionShares} / ${summary.risk.maxShortPositionShares}`],
              ["Orders blocked by risk", summary.risk.blockedOrderCount],
              ["Max drawdown", formatDecimalForDisplay(summary.risk.maxDrawdown)],
              ["Drawdown peak to trough", `${formatSimulationTimeNs(summary.risk.maxDrawdownPeakTimeNs, 3)} to ${formatSimulationTimeNs(summary.risk.maxDrawdownTroughTimeNs, 3)}`],
            ]}
          />
        </CardContent>
      </Card>
      <Card size="sm">
        <CardHeader>
          <CardTitle>Markouts</CardTitle>
          <CardDescription>Signed markout uses exchange mid: buy = future mid - fill price; sell = fill price - future mid. It includes the initial spread edge, so it is not pure post-fill mid-price drift. The mean weights eligible shares, not fills. Totals include available fills only. Horizons not yet reached or past run end are censored, not zero.</CardDescription>
        </CardHeader>
        <CardContent>
          <Table>
            <TableHeader>
              <TableRow>
                <TableHead>Horizon</TableHead>
                <TableHead className="text-right">Eligible fills</TableHead>
                <TableHead className="text-right">Eligible shares</TableHead>
                <TableHead className="text-right">No mark</TableHead>
                <TableHead className="text-right">Censored</TableHead>
                <TableHead className="text-right">Total (USD)</TableHead>
                <TableHead className="text-right">Mean per share (derived)</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              {summary.markouts.map((markout) => (
                <TableRow key={markout.horizonNs}>
                  <TableCell>{formatDurationNs(markout.horizonNs)}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{markout.availableFillCount}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{markout.availableShares ?? "Unavailable"}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{markout.unavailableFillCount}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{markout.censoredFillCount}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{markout.availableFillCount === "0" ? "Unavailable: no eligible fills" : formatDecimalForDisplay(markout.totalMarkout, { signDisplay: "always" })}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{formatDecimalForDisplay(markout.meanMarkoutPerShare, { signDisplay: "always" })}</TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        </CardContent>
      </Card>
      <Card size="sm">
        <CardHeader>
          <CardTitle>Latency observed</CardTitle>
          <CardDescription>Configured value versus what the event log shows.</CardDescription>
        </CardHeader>
        <CardContent>
          <Table>
            <TableHeader>
              <TableRow>
                <TableHead>Path</TableHead>
                <TableHead className="text-right">Configured</TableHead>
                <TableHead className="text-right">Samples</TableHead>
                <TableHead className="text-right">Min</TableHead>
                <TableHead className="text-right">Max</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              {summary.latency.map((measurement) => (
                <TableRow key={measurement.component}>
                  <TableCell>{humanizeEnumValue(measurement.component)}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{formatDurationNs(measurement.configuredNs)}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{measurement.sampleCount}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{formatDurationNs(measurement.observedMinNs)}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{formatDurationNs(measurement.observedMaxNs)}</TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        </CardContent>
      </Card>
      <Card size="sm" className="xl:col-span-2">
        <CardHeader>
          <CardTitle>All metrics</CardTitle>
          <CardDescription>Exact metrics come from the fixed-point ledger; derived ones are rounded half-to-even after the fact.</CardDescription>
        </CardHeader>
        <CardContent>
          <Table>
            <TableHeader>
              <TableRow>
                <TableHead>Metric</TableHead>
                <TableHead className="text-right">Value</TableHead>
                <TableHead>Unit</TableHead>
                <TableHead>Exactness</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              {summary.metrics.filter((metric) => metric.unavailableReason !== "not_a_parent_order_strategy").map((metric) => (
                <TableRow key={`${metric.metricKey}:${metric.horizonNs ?? ""}`}>
                  <TableCell>{metricLabel(metric.metricKey, metric.horizonNs)}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{metric.value === null ? metric.unavailableReason ?? "n/a" : formatMetricValue(metric.value, metric.unit)}</TableCell>
                  <TableCell className="text-muted-foreground">{METRIC_UNIT_LABEL[metric.unit]}</TableCell>
                  <TableCell>
                    <Badge variant={metric.exactness === "exact_fixed_point" ? "secondary" : "outline"}>{metric.exactness === "exact_fixed_point" ? "exact" : "derived"}</Badge>
                  </TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        </CardContent>
      </Card>
    </div>
  );
}

function ManifestPanel({ manifest }: { manifest: ApiSchemas["RunManifest"] }) {
  return (
    <div className="grid gap-4 xl:grid-cols-2">
      <Card size="sm">
        <CardHeader>
          <CardTitle>Provenance</CardTitle>
          <CardDescription>Recorded assumptions and artifact identities. Reproduction also requires the original executable; format version labels alone do not identify it. API run IDs and wall-clock envelopes can differ between repeats.</CardDescription>
        </CardHeader>
        <CardContent className="flex flex-col gap-3">
          <DefinitionRows
            rows={[
              ["Schema", manifest.schemaVersion],
              ["Mode / world", `${manifest.mode} / ${manifest.marketWorld}`],
              ["Recorded source revision", manifest.provenance?.sourceRevision ?? "Unavailable for this run"],
              ["Recorded release", manifest.provenance?.releaseId ?? "Unavailable for this run"],
              ["Executable SHA-256", manifest.provenance?.executableSha256 ?? "Unavailable for this run"],
              ["Engine format / family", manifest.engineBuildId],
              ["Runner build", manifest.runnerBuildId],
              ["Strategy build", manifest.strategyBuildId],
              ["Config digest", `${manifest.configDigest.algorithm} ${manifest.configDigest.hexDigest}`],
              ["Matching rule", manifest.matchingRule],
              ["Queue model", manifest.queueModel],
              ["Event order", `${manifest.eventOrderPolicy.primaryKey}, then ${manifest.eventOrderPolicy.stageOrderForEqualTime.join(" > ")}, then ${manifest.eventOrderPolicy.tieBreaker}`],
              ["Cost basis / fee sign", `${manifest.accounting.costBasisMethod} / ${manifest.accounting.feeSign}`],
              ["Cash scale", manifest.accounting.cashScale],
            ]}
          />
        </CardContent>
      </Card>
      <Card size="sm">
        <CardHeader>
          <CardTitle>Seeds</CardTitle>
          <CardDescription className="font-mono text-xs">{manifest.seeds.derivationFunction}</CardDescription>
        </CardHeader>
        <CardContent>
          <DefinitionRows rows={[["master", manifest.seeds.masterSeed], ...manifest.seeds.derivedSeeds.map((derivedSeed): [string, string] => [derivedSeed.streamName, derivedSeed.seed])]} />
        </CardContent>
      </Card>
      <Card size="sm" className="xl:col-span-2">
        <CardHeader>
          <CardTitle>Full manifest</CardTitle>
        </CardHeader>
        <CardContent>
          <pre className="max-h-[32rem] overflow-auto rounded-md bg-muted p-3 font-mono text-xs">{JSON.stringify(manifest, null, 2)}</pre>
        </CardContent>
      </Card>
    </div>
  );
}

export function RunDetailView() {
  const { runId } = useParams<{ runId: string }>();
  const comparisonQuery = useComparisonContext();
  const apiClient = getBrowserApiClient();
  const runResource = useApiResource(`run:${runId}`, async () => unwrapApiResult(await apiClient.GET("/v1/runs/{runId}", { params: { path: { runId } } })), (latestRun) =>
    isTerminalRunState(latestRun.state) ? null : ACTIVE_RUN_POLL_INTERVAL_MS,
  );
  const run = runResource.data;
  const summaryKey = run ? `summary:${runId}:${run.state}:${run.progress.processedCanonicalEventCount}` : null;
  const summaryResource = useApiResource(summaryKey, async () => unwrapApiResult(await apiClient.GET("/v1/runs/{runId}/summary", { params: { path: { runId } } })), null);
  const manifestResource = useApiResource(`manifest:${runId}`, async () => unwrapApiResult(await apiClient.GET("/v1/runs/{runId}/manifest", { params: { path: { runId } } })), null);
  const seriesResource = useApiResource(summaryKey ? `series:${summaryKey}` : null, async () => unwrapApiResult(await apiClient.GET("/v1/runs/{runId}/series", { params: { path: { runId }, query: { seriesKinds: ["equity", "inventory", "drawdown"], maxPoints: 300 } } })), null);

  if (runResource.failure && !run) return <ApiFailureAlert failure={runResource.failure} title="Could not load run" onRetry={runResource.reload} />;
  if (!run) return <p className="text-sm text-muted-foreground">Loading run...</p>;

  return (
    <>
      <PageHeading
        title={
          <span className="flex items-center gap-2">
            <span>Seed {run.masterSeed}</span>
            <RunStateBadge runState={run.state} />
          </span>
        }
        description={
          <span>
            <span className="font-mono text-xs">{run.runId}</span>{" / "}
            <Link className="underline" href={`/experiments/${run.experimentId}`}>
              {run.experimentId}
            </Link>
            , {humanizeEnumValue(run.mode)}, {humanizeEnumValue(run.marketWorld)}
          </span>
        }
        actions={
          <>
            <RunControls
              key={run.runId}
              stale={runResource.failure !== null}
              run={run}
              onRunChanged={() => runResource.reload()}
            />
            <Button asChild size="sm">
              <Link href={`/runs/${run.runId}/inspect${comparisonQuery}`}>Inspect events</Link>
            </Button>
          </>
        }
      />
      <RunResearchContext experimentId={run.experimentId} />
      <div className="run-progress-strip">
        <span>
          Events processed <span className="font-mono tabular-nums">{formatIntegerText(run.progress.processedCanonicalEventCount)}</span>
        </span>
        <span>
          Simulated time <span className="font-mono tabular-nums">{formatSimulationTimeNs(run.progress.simulationTimeNs, 3)}</span> of{" "}
          <span className="font-mono tabular-nums">{formatSimulationTimeNs(run.progress.endSimulationTimeNs, 0)}</span>
        </span>
        {summaryResource.data ? (
          <Badge variant="outline">
            summary {summaryResource.data.availability} at event {summaryResource.data.asOfEventPosition}
          </Badge>
        ) : null}
      </div>
      {runResource.failure ? <ApiFailureAlert failure={runResource.failure} title="Run status is stale. Refresh before changing the run." onRetry={runResource.reload} /> : null}
      {seriesResource.failure ? <ApiFailureAlert failure={seriesResource.failure} title="Could not load run series" onRetry={seriesResource.reload} /> : null}
      {run.terminalDiagnostics ? (
        <p className="run-diagnostics">
          {run.state === "completed" ? "Run complete." : run.terminalDiagnostics.message} Residual position{" "}
          <span className="font-mono">{run.terminalDiagnostics.residualPositionShares}</span> shares, residual open orders <span className="font-mono">{run.terminalDiagnostics.residualOpenOrderCount}</span>.
        </p>
      ) : null}
      {summaryResource.failure ? <ApiFailureAlert failure={summaryResource.failure} title="Summary unavailable or stale. Refresh before interpreting outcomes." onRetry={summaryResource.reload} /> : null}
      {summaryResource.data ? <TraderRunOutcomes summary={summaryResource.data} /> : null}
      <nav aria-label="Continue research" className="flex flex-wrap items-center gap-3 rounded-md border p-3 text-sm">
        <span className="text-muted-foreground">Next decision</span>
        <Button asChild size="sm" variant="outline"><Link href={`/runs/${run.runId}/inspect${comparisonQuery}`}>Explain fills and queue events</Link></Button>
        <Button asChild size="sm" variant="outline"><Link href={`/experiments/new?from=${encodeURIComponent(run.experimentId)}`}>Create variant</Link></Button>
        <Button asChild size="sm" variant="outline"><Link href={`/compare?baseline=${encodeURIComponent(run.experimentId)}`}>Compare against this experiment</Link></Button>
      </nav>
      {summaryResource.data?.execution ? <ExecutionTracePanel key={run.runId} run={run} /> : null}
      <RunAuditControls key={`audit:${run.runId}`} run={run} />
      {seriesResource.data ? (
        <div className="grid gap-4 lg:grid-cols-3">
          {seriesResource.data.series.map((timeSeries) => (
            <RunSeriesChart key={timeSeries.seriesKind} timeSeries={timeSeries} />
          ))}
        </div>
      ) : null}
      <Tabs defaultValue="summary">
        <TabsList>
          <TabsTrigger value="summary">Summary</TabsTrigger>
          <TabsTrigger value="manifest">Manifest</TabsTrigger>
          <TabsTrigger value="history">State history</TabsTrigger>
        </TabsList>
        <TabsContent value="summary">
          {summaryResource.data ? <SummaryPanel summary={summaryResource.data} /> : <p className="text-sm text-muted-foreground">Loading summary...</p>}
        </TabsContent>
        <TabsContent value="manifest">
          {manifestResource.failure ? <ApiFailureAlert failure={manifestResource.failure} onRetry={manifestResource.reload} /> : null}
          {manifestResource.data ? <ManifestPanel manifest={manifestResource.data} /> : null}
        </TabsContent>
        <TabsContent value="history">
          <Table>
            <TableHeader>
              <TableRow>
                <TableHead>Wall clock</TableHead>
                <TableHead>Transition</TableHead>
                <TableHead className="text-right">At event</TableHead>
                <TableHead className="text-right">At simulated time</TableHead>
                <TableHead>Reason</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              {run.stateHistory.map((transition, transitionIndex) => (
                <TableRow key={transitionIndex}>
                  <TableCell className="font-mono text-xs">{formatWallClockTime(transition.transitionedAt)}</TableCell>
                  <TableCell>
                    {transition.fromState ?? "(created)"} to {transition.toState}
                  </TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{transition.atEventPosition}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{formatSimulationTimeNs(transition.atSimulationTimeNs, 3)}</TableCell>
                  <TableCell>{transition.reason}</TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
          <DefinitionRows
            rows={[
              ["Manifest digest", `${run.digests.manifestDigest.algorithm} ${run.digests.manifestDigest.hexDigest}`],
              ["Canonical event digest", run.digests.canonicalEventDigest ? `${run.digests.canonicalEventDigest.algorithm} ${run.digests.canonicalEventDigest.hexDigest}` : "available when terminal"],
              ["Ledger digest", run.digests.ledgerDigest ? `${run.digests.ledgerDigest.algorithm} ${run.digests.ledgerDigest.hexDigest}` : "available when terminal"],
            ]}
          />
        </TabsContent>
      </Tabs>
    </>
  );
}
