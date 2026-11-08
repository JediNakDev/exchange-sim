"use client";

import Link from "next/link";
import { useParams } from "next/navigation";
import { useRef, useState } from "react";
import { toast } from "sonner";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { DefinitionList } from "@/components/research/definition-list";
import { ResearchSection } from "@/components/research/research-section";
import { SignedFigure, type FavourableDirection } from "@/components/research/signed-figure";
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
    <div className="grid min-w-0 grid-cols-1 gap-8 xl:grid-cols-2">
      <ExecutionSummaryPanel summary={summary} />
      <div className="min-w-0 xl:col-span-2"><RequestDiagnosticsPanel diagnostics={summary.orders.requestDiagnostics} /></div>
      <ResearchSection title="Ledger" description={<> Equity = cash + position x mark ({humanizeEnumValue(accounting.markConvention)}). Net P&L = realized + unrealized - fees. </>}>
          <DefinitionList className="grid-cols-[minmax(0,1fr)_minmax(0,1fr)]"
            rows={[
              ["Initial cash", formatDecimalForDisplay(accounting.initialCash)],
              ["Cash", formatDecimalForDisplay(accounting.cash)],
              ["Position (shares)", <SignedFigure key="Position (shares)" formattedValue={formatDecimalForDisplay(accounting.positionShares)} favourableDirection="neutral" />],
              ["Mark", formatDecimalForDisplay(accounting.markPrice)],
              ["Equity", formatDecimalForDisplay(accounting.equity)],
              ["Gross realized P&L", <SignedFigure key="Gross realized P&L" formattedValue={formatDecimalForDisplay(accounting.grossRealizedPnl, { signDisplay: "always" })} favourableDirection="higher" />],
              ["Unrealized P&L", <SignedFigure key="Unrealized P&L" formattedValue={formatDecimalForDisplay(accounting.unrealizedPnl, { signDisplay: "always" })} favourableDirection="higher" />],
              ["Net fees paid", <SignedFigure key="Net fees paid" formattedValue={formatDecimalForDisplay(accounting.netFees)} favourableDirection="lower" />],
              ["Net P&L", <SignedFigure key="Net P&L" formattedValue={formatDecimalForDisplay(accounting.netPnl, { signDisplay: "always" })} favourableDirection="higher" />],
              ["Average cost basis (derived)", formatDecimalForDisplay(accounting.averageCostBasis)],
              ["Spread capture total", <SignedFigure key="Spread capture total" formattedValue={formatDecimalForDisplay(summary.spreadCaptureTotal, { signDisplay: "always" })} favourableDirection="higher" />],
            ]}
          />
          {accounting.markUnavailableReason ? <p className="mt-2 text-xs text-muted-foreground">{accounting.markUnavailableReason}</p> : null}
        </ResearchSection>
      <ResearchSection title="Orders and risk" description={<> {summary.orders.fillRateByShares.definition} </>}>
          <DefinitionList className="grid-cols-[minmax(0,1fr)_minmax(0,1fr)]"
            rows={[
              ["New orders submitted / accepted / rejected", `${summary.orders.submittedOrderCount} / ${summary.orders.acceptedOrderCount} / ${summary.orders.rejectedOrderCount}`],
              ["Canceled (cancel races lost)", `${summary.orders.canceledOrderCount} (${summary.orders.cancelRaceLostCount})`],
              ["Fills (maker / taker)", `${summary.orders.fillCount} (${summary.orders.makerFillCount} / ${summary.orders.takerFillCount})`],
              ["Filled / submitted shares", `${formatIntegerText(summary.orders.filledShares)} / ${formatIntegerText(summary.orders.submittedShares)}`],
              ["Fill rate by shares (derived)", formatDecimalForDisplay(summary.orders.fillRateByShares.ratio)],
              ["Quote exposure time", formatDurationNs(summary.orders.quoteExposureTimeNs)],
              ["Max long / short (shares)", `${summary.risk.maxLongPositionShares} / ${summary.risk.maxShortPositionShares}`],
              ["Orders blocked by risk", summary.risk.blockedOrderCount],
              ["Max drawdown", <SignedFigure key="Max drawdown" formattedValue={formatDecimalForDisplay(summary.risk.maxDrawdown)} favourableDirection="lower" />],
              ["Drawdown peak to trough", `${formatSimulationTimeNs(summary.risk.maxDrawdownPeakTimeNs, 3)} to ${formatSimulationTimeNs(summary.risk.maxDrawdownTroughTimeNs, 3)}`],
            ]}
          />
        </ResearchSection>
      <ResearchSection title="Markouts" description={<> Signed markout uses exchange mid: buy = future mid - fill price; sell = fill price - future mid. It includes the initial spread edge, so it is not pure post-fill mid-price drift. The mean weights eligible shares, not fills. Totals include available fills only. Horizons not yet reached or past run end are censored, not zero. </>}>
          <div className="min-w-0 overflow-x-auto border bg-card"><Table aria-label="Markouts">
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
                  <TableCell className="text-right font-mono tabular-nums">{formatDurationNs(markout.horizonNs)}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{markout.availableFillCount}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{markout.availableShares ?? "Unavailable"}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{markout.unavailableFillCount}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{markout.censoredFillCount}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{markout.availableFillCount === "0" ? "Unavailable: no eligible fills" : <SignedFigure formattedValue={formatDecimalForDisplay(markout.totalMarkout, { signDisplay: "always" })} favourableDirection="higher" />}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums"><SignedFigure formattedValue={formatDecimalForDisplay(markout.meanMarkoutPerShare, { signDisplay: "always" })} favourableDirection="higher" /></TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table></div>
        </ResearchSection>
      <ResearchSection title="Latency observed" description={<> Configured value versus what the event log shows. </>}>
          <div className="min-w-0 overflow-x-auto border bg-card"><Table aria-label="Latency observed">
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
          </Table></div>
        </ResearchSection>
      <ResearchSection className="xl:col-span-2" title="All metrics" description={<> Exact metrics come from the fixed-point ledger; derived ones are rounded half-to-even after the fact. </>}>
          <div className="min-w-0 overflow-x-auto border bg-card"><Table aria-label="All metrics">
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
                  <TableCell className="text-right font-mono tabular-nums">{metric.value === null ? metric.unavailableReason ?? "n/a" : <SignedFigure formattedValue={formatMetricValue(metric.value, metric.unit, METRIC_KEYS_WITH_A_FAVOURABLE_DIRECTION.has(metric.metricKey) ? { signDisplay: "always" } : {})} favourableDirection={favourableDirectionForMetric(metric.metricKey)} />}</TableCell>
                  <TableCell className="text-muted-foreground">{METRIC_UNIT_LABEL[metric.unit]}</TableCell>
                  <TableCell>
                    <Badge variant={metric.exactness === "exact_fixed_point" ? "secondary" : "outline"}>{metric.exactness === "exact_fixed_point" ? "exact" : "derived"}</Badge>
                  </TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table></div>
        </ResearchSection>
    </div>
  );
}

// Signed costs only. Drawdown is a non-negative magnitude and the fee sign follows the manifest convention, so both stay neutral.
const METRIC_KEYS_WHERE_LOWER_IS_FAVOURABLE = new Set(["decision_slippage_total", "decision_slippage_per_share", "implementation_shortfall_total", "implementation_shortfall_per_share"]);
const METRIC_KEYS_WHERE_HIGHER_IS_FAVOURABLE = new Set(["net_pnl", "gross_realized_pnl", "unrealized_pnl", "spread_capture_total", "markout_total", "markout_per_share"]);
const METRIC_KEYS_WITH_A_FAVOURABLE_DIRECTION = new Set([...METRIC_KEYS_WHERE_LOWER_IS_FAVOURABLE, ...METRIC_KEYS_WHERE_HIGHER_IS_FAVOURABLE]);

function favourableDirectionForMetric(metricKey: string): FavourableDirection {
  if (METRIC_KEYS_WHERE_LOWER_IS_FAVOURABLE.has(metricKey)) return "lower";
  if (METRIC_KEYS_WHERE_HIGHER_IS_FAVOURABLE.has(metricKey)) return "higher";
  return "neutral";
}

function ManifestPanel({ manifest }: { manifest: ApiSchemas["RunManifest"] }) {
  return (
    <div className="grid gap-8 xl:grid-cols-2">
      <ResearchSection title="Provenance" description={<> Recorded assumptions and artifact identities. Reproduction also requires the original executable; format version labels alone do not identify it. API run IDs and wall-clock envelopes can differ between repeats. </>}>
          <DefinitionList className="grid-cols-[minmax(0,1fr)_minmax(0,1fr)]"
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
        </ResearchSection>
      <ResearchSection title="Seeds" description={<> {manifest.seeds.derivationFunction} </>}>
          <DefinitionList className="grid-cols-[minmax(0,1fr)_minmax(0,1fr)]" rows={[["master", manifest.seeds.masterSeed], ...manifest.seeds.derivedSeeds.map((derivedSeed): [string, string] => [derivedSeed.streamName, derivedSeed.seed])]} />
        </ResearchSection>
      <ResearchSection title="Full manifest" className="xl:col-span-2">
        <pre className="max-h-[32rem] overflow-auto border bg-muted p-3 font-mono text-xs">{JSON.stringify(manifest, null, 2)}</pre>
      </ResearchSection>
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
          <span className="flex flex-wrap items-center gap-2">
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
      <div className="flex flex-wrap items-center gap-x-6 gap-y-2 text-xs text-muted-foreground">
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
        <p className="text-xs text-muted-foreground">
          {run.state === "completed" ? "Run complete." : run.terminalDiagnostics.message} Residual position{" "}
          <span className="font-mono">{run.terminalDiagnostics.residualPositionShares}</span> shares, residual open orders <span className="font-mono">{run.terminalDiagnostics.residualOpenOrderCount}</span>.
        </p>
      ) : null}
      {summaryResource.failure ? <ApiFailureAlert failure={summaryResource.failure} title="Summary unavailable or stale. Refresh before interpreting outcomes." onRetry={summaryResource.reload} /> : null}
      {summaryResource.data ? <TraderRunOutcomes summary={summaryResource.data} /> : null}
      <nav aria-label="Continue research" className="flex flex-wrap items-center gap-2 text-xs">
        <span className="text-muted-foreground">Next decision</span>
        <Button asChild size="sm" variant="outline"><Link href={`/runs/${run.runId}/inspect${comparisonQuery}`}>Explain fills and queue events</Link></Button>
        <Button asChild size="sm" variant="outline"><Link href={`/experiments/new?from=${encodeURIComponent(run.experimentId)}`}>Create variant</Link></Button>
        <Button asChild size="sm" variant="outline"><Link href={`/compare?baseline=${encodeURIComponent(run.experimentId)}`}>Compare against this experiment</Link></Button>
      </nav>
      {summaryResource.data?.execution ? <ExecutionTracePanel key={run.runId} run={run} /> : null}
      <RunAuditControls key={`audit:${run.runId}`} run={run} />
      {seriesResource.data ? (
        <div className="grid min-w-0 gap-4 xl:grid-cols-3">
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
        <TabsContent value="history" className="space-y-4">
          <div className="min-w-0 overflow-x-auto border bg-card"><Table aria-label="State history">
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
          </Table></div>
          <DefinitionList className="grid-cols-[minmax(0,1fr)_minmax(0,1fr)]"
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
