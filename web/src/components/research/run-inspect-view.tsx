"use client";

// Step through a run's canonical event log and explain individual fills: when
// the order was submitted, arrived, was acknowledged and filled, how much was
// ahead of it in the FIFO queue, and what the fill did to inventory and P&L.

import Link from "next/link";
import { useParams } from "next/navigation";
import { useState, type KeyboardEvent } from "react";
import { ChevronFirstIcon, ChevronLastIcon, ChevronLeftIcon, ChevronRightIcon, ChevronsLeftIcon, ChevronsRightIcon } from "lucide-react";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card";
import { Input } from "@/components/ui/input";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs";
import { ApiFailureAlert } from "@/components/research/api-failure-alert";
import { RunResearchContext, useComparisonContext } from "@/components/research/run-research-context";
import { PageHeading } from "@/components/research/page-heading";
import { isTerminalRunState, RunStateBadge } from "@/components/research/run-state-badge";
import { ArtifactPageControls } from "@/components/research/artifact-page-controls";
import { readSelectedFill } from "@/lib/api/research-workflow";
import { useCursorHistory } from "@/hooks/use-cursor-history";
import { useApiResource } from "@/hooks/use-api-resource";
import { getBrowserApiClient, unwrapApiResult, type ApiSchemas } from "@/lib/api/exsim-api-client";
import { formatDecimalForDisplay, formatDurationNs, formatIntegerText, formatSimulationTimeNs, offsetUnsignedIntegerText } from "@/lib/exact-decimal-format";
import { humanizeEnumValue } from "@/lib/research-labels";
import { cn } from "@/lib/utils";

const EVENT_WINDOW_RADIUS = 12n;
const LARGE_STEP_EVENT_COUNT = 25n;
const BOOK_DEPTH = 5;

function lastEventPosition(processedEventCountText: string): string {
  return processedEventCountText === "0" ? "0" : (BigInt(processedEventCountText) - 1n).toString();
}

function VisibilityBadge({ visibility }: { visibility: ApiSchemas["EventVisibility"] }) {
  return <Badge variant={visibility === "strategy_visible" ? "default" : "outline"}>{visibility === "strategy_visible" ? "strategy-visible" : "exchange truth"}</Badge>;
}

function EventWindowTable({
  events,
  focusedEventPosition,
  relatedEventPositions,
  onSelectEventPosition,
}: {
  events: ApiSchemas["CanonicalEvent"][];
  focusedEventPosition: string;
  relatedEventPositions: Set<string>;
  onSelectEventPosition: (eventPosition: string) => void;
}) {
  return (
    <div className="overflow-x-auto rounded-md border">
      <Table className="text-xs">
        <TableHeader>
          <TableRow>
            <TableHead className="text-right">#</TableHead>
            <TableHead className="text-right">Sim time</TableHead>
            <TableHead>Stage</TableHead>
            <TableHead>Event</TableHead>
            <TableHead>Actor</TableHead>
            <TableHead>Visibility</TableHead>
            <TableHead>Order</TableHead>
            <TableHead>Side</TableHead>
            <TableHead className="text-right">Price</TableHead>
            <TableHead className="text-right">Qty</TableHead>
            <TableHead>Detail</TableHead>
          </TableRow>
        </TableHeader>
        <TableBody>
          {events.map((canonicalEvent) => {
            const isFocused = canonicalEvent.eventPosition === focusedEventPosition;
            const isRelated = relatedEventPositions.has(canonicalEvent.eventPosition);
            return (
              <TableRow
                key={canonicalEvent.eventPosition}
                tabIndex={0}
                aria-selected={isFocused}
                data-state={isFocused ? "selected" : undefined}
                className={cn("cursor-pointer focus-visible:outline-2 focus-visible:outline-ring", isRelated && !isFocused && "bg-amber-500/15", canonicalEvent.isOwnOrder && "font-medium")}
                onClick={() => onSelectEventPosition(canonicalEvent.eventPosition)}
                onKeyDown={(keyboardEvent) => {
                  if (keyboardEvent.key === "Enter") onSelectEventPosition(canonicalEvent.eventPosition);
                }}
              >
                <TableCell className="text-right font-mono tabular-nums">{canonicalEvent.eventPosition}</TableCell>
                <TableCell className="text-right font-mono tabular-nums">{formatSimulationTimeNs(canonicalEvent.simulationTimeNs, 6)}</TableCell>
                <TableCell className="text-muted-foreground">{humanizeEnumValue(canonicalEvent.stage)}</TableCell>
                <TableCell>{humanizeEnumValue(canonicalEvent.eventKind)}</TableCell>
                <TableCell>{canonicalEvent.isOwnOrder ? <Badge variant="secondary">own</Badge> : humanizeEnumValue(canonicalEvent.actor)}</TableCell>
                <TableCell>
                  <VisibilityBadge visibility={canonicalEvent.visibility} />
                </TableCell>
                <TableCell className="font-mono">{canonicalEvent.orderId ?? ""}</TableCell>
                <TableCell>{canonicalEvent.side ?? ""}</TableCell>
                <TableCell className="text-right font-mono tabular-nums">{canonicalEvent.price ?? ""}</TableCell>
                <TableCell className="text-right font-mono tabular-nums">{canonicalEvent.quantity ?? ""}</TableCell>
                <TableCell className="max-w-80 truncate" title={canonicalEvent.description}>
                  {canonicalEvent.description}
                </TableCell>
              </TableRow>
            );
          })}
        </TableBody>
      </Table>
    </div>
  );
}

function BookLadder({ bookView, title }: { bookView: ApiSchemas["BookView"] | null; title: string }) {
  return (
    <Card size="sm">
      <CardHeader>
        <CardTitle>{title}</CardTitle>
        <CardDescription>
          {bookView ? (
            <>
              {humanizeEnumValue(bookView.source)}
              {bookView.lastDeliveredSourceEventPosition !== null ? `, last delivered update from event ${bookView.lastDeliveredSourceEventPosition}` : ""}
              {bookView.view === "strategy_visible" && bookView.bids.every((bookLevel) => bookLevel.orderCount === null) ? ". The feed delivers top of book only, without order counts" : ""}
            </>
          ) : (
            "Loading..."
          )}
        </CardDescription>
      </CardHeader>
      {bookView ? (
        <CardContent>
          <Table className="text-xs">
            <TableHeader>
              <TableRow>
                <TableHead className="text-right">Own</TableHead>
                <TableHead className="text-right">Orders</TableHead>
                <TableHead className="text-right">Shares</TableHead>
                <TableHead className="text-right">Price</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              {[...bookView.asks].reverse().map((bookLevel) => (
                <TableRow key={`ask-${bookLevel.price}`}>
                  <TableCell className="text-right font-mono tabular-nums">{bookLevel.ownShares === "0" ? "" : (bookLevel.ownShares ?? "n/a")}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{bookLevel.orderCount ?? "n/a"}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{formatIntegerText(bookLevel.totalShares)}</TableCell>
                  <TableCell className="text-right font-mono text-red-700 tabular-nums dark:text-red-400">{bookLevel.price}</TableCell>
                </TableRow>
              ))}
              {bookView.bids.map((bookLevel) => (
                <TableRow key={`bid-${bookLevel.price}`}>
                  <TableCell className="text-right font-mono tabular-nums">{bookLevel.ownShares === "0" ? "" : (bookLevel.ownShares ?? "n/a")}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{bookLevel.orderCount ?? "n/a"}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{formatIntegerText(bookLevel.totalShares)}</TableCell>
                  <TableCell className="text-right font-mono text-green-700 tabular-nums dark:text-green-400">{bookLevel.price}</TableCell>
                </TableRow>
              ))}
              {bookView.bids.length === 0 && bookView.asks.length === 0 ? (
                <TableRow>
                  <TableCell colSpan={4} className="text-muted-foreground">
                    Empty book
                  </TableCell>
                </TableRow>
              ) : null}
            </TableBody>
          </Table>
        </CardContent>
      ) : null}
    </Card>
  );
}

function EventPositionButton({ eventPosition, label, onSelectEventPosition }: { eventPosition: string | null; label: string; onSelectEventPosition: (eventPosition: string) => void }) {
  if (eventPosition === null) {
    return (
      <span className="text-muted-foreground">
        {label}: <span className="font-mono">not observed</span>
      </span>
    );
  }
  return (
    <Button size="xs" variant="outline" onClick={() => onSelectEventPosition(eventPosition)}>
      {label} <span className="font-mono">#{eventPosition}</span>
    </Button>
  );
}

function FocusPanel({
  focusedFill,
  lifecycle,
  lifecycleFailure,
  onRetryLifecycle,
  onSelectEventPosition,
}: {
  focusedFill: ApiSchemas["Fill"] | null;
  lifecycle: ApiSchemas["OrderLifecycle"] | null;
  lifecycleFailure: ApiSchemas["ApiError"] | null;
  onRetryLifecycle: () => void;
  onSelectEventPosition: (eventPosition: string) => void;
}) {
  if (lifecycleFailure) return <ApiFailureAlert failure={lifecycleFailure} title="Could not load order lifecycle" onRetry={onRetryLifecycle} />;
  if (!lifecycle) {
    return (
      <Card size="sm">
        <CardHeader>
          <CardTitle>Fill explanation</CardTitle>
          <CardDescription>Select a fill or an own order below to see its lifecycle, queue position and ledger effect.</CardDescription>
        </CardHeader>
      </Card>
    );
  }
  const order = lifecycle.order;
  return (
    <div className="flex flex-col gap-3">
      <Card size="sm">
        <CardHeader>
          <CardTitle>
            Order {order.orderId}: latest chain state
          </CardTitle>
          <CardDescription>
            Client order chain {order.clientOrderId}, as of event {order.lastEventPosition}; latest exchange order {order.orderId}: {order.side} {order.quantity} @ {order.price}. {humanizeEnumValue(order.finalStatus)}, {order.cumulativeQuantity} filled in {order.fillCount} fills across the chain. Matching {lifecycle.matchingRule}. These totals are not a snapshot at the selected fill.
          </CardDescription>
        </CardHeader>
        <CardContent className="flex flex-col gap-3">
          {focusedFill ? (
            <div className="space-y-2">
            <p className="rounded-md border bg-muted/30 p-3 text-sm" aria-label="Selected fill identity"><strong>Selected fill:</strong> order {focusedFill.orderId}, event {focusedFill.eventPosition}, match {focusedFill.matchId}; {focusedFill.side} {focusedFill.quantity} @ {focusedFill.price}. The lifecycle below spans the replacement chain; its latest state can have another exchange order ID.</p>
            <div className="flex flex-wrap gap-1.5 text-xs">
              <EventPositionButton label="Submit" eventPosition={focusedFill.relatedEvents.strategySubmitEventPosition} onSelectEventPosition={onSelectEventPosition} />
              <EventPositionButton label="Arrival" eventPosition={focusedFill.relatedEvents.exchangeArrivalEventPosition} onSelectEventPosition={onSelectEventPosition} />
              <EventPositionButton label="Accepted" eventPosition={focusedFill.relatedEvents.acceptedEventPosition} onSelectEventPosition={onSelectEventPosition} />
              <EventPositionButton label="Ack delivered" eventPosition={focusedFill.relatedEvents.acknowledgmentDeliveredEventPosition} onSelectEventPosition={onSelectEventPosition} />
              <EventPositionButton label="Aggressor arrival" eventPosition={focusedFill.relatedEvents.aggressorArrivalEventPosition} onSelectEventPosition={onSelectEventPosition} />
              <EventPositionButton label="Fill" eventPosition={focusedFill.relatedEvents.fillEventPosition} onSelectEventPosition={onSelectEventPosition} />
              <EventPositionButton label="Fill report delivered" eventPosition={focusedFill.relatedEvents.fillReportDeliveredEventPosition} onSelectEventPosition={onSelectEventPosition} />
            </div></div>
          ) : null}
          <ol className="relative flex flex-col gap-1 border-l pl-3 text-xs" aria-label="Order lifecycle timeline">
            {lifecycle.stages.map((lifecycleStage) => (
              <li key={`${lifecycleStage.stageKind}-${lifecycleStage.eventPosition}`} className="flex flex-wrap items-center gap-2">
                <button type="button" className="font-mono underline-offset-2 hover:underline focus-visible:outline-2 focus-visible:outline-ring" onClick={() => onSelectEventPosition(lifecycleStage.eventPosition)}>
                  #{lifecycleStage.eventPosition}
                </button>
                <span className="font-mono text-muted-foreground tabular-nums">{formatSimulationTimeNs(lifecycleStage.simulationTimeNs, 6)}</span>
                <span className="font-medium">{humanizeEnumValue(lifecycleStage.stageKind)}</span>
                <VisibilityBadge visibility={lifecycleStage.visibility} />
                {lifecycleStage.quantity !== null ? <span>quantity {lifecycleStage.quantity}</span> : null}
                {lifecycleStage.leavesQuantity !== null ? <span>remaining {lifecycleStage.leavesQuantity}</span> : null}
                <span className="text-muted-foreground">{lifecycleStage.detail}</span>
              </li>
            ))}
          </ol>
          <div className="grid grid-cols-2 gap-x-4 gap-y-1 text-xs md:grid-cols-4">
            <span>Order entry {formatDurationNs(lifecycle.latency.orderEntryNs)}</span>
            <span>Acknowledgment {formatDurationNs(lifecycle.latency.acknowledgmentNs)}</span>
            <span>Cancel {formatDurationNs(lifecycle.latency.cancelNs)}</span>
            <span>Submit to first fill {formatDurationNs(lifecycle.latency.submitToFirstFillNs)}</span>
          </div>
        </CardContent>
      </Card>
      <Card size="sm">
        <CardHeader>
          <CardTitle>FIFO queue ahead</CardTitle>
          <CardDescription>Shares resting at the same price with earlier time priority. Exact simulated truth unless marked estimated.</CardDescription>
        </CardHeader>
        <CardContent>
          <Table className="text-xs">
            <TableHeader>
              <TableRow>
                <TableHead>Event</TableHead>
                <TableHead>Reason</TableHead>
                <TableHead className="text-right">Shares ahead</TableHead>
                <TableHead className="text-right">Orders ahead</TableHead>
                <TableHead className="text-right">Level total</TableHead>
                <TableHead>Source</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              {lifecycle.fifoAheadEvidence.map((evidence) => (
                <TableRow key={`${evidence.eventPosition}-${evidence.reason}`} tabIndex={0} className="cursor-pointer" onClick={() => onSelectEventPosition(evidence.eventPosition)} onKeyDown={(keyboardEvent) => keyboardEvent.key === "Enter" && onSelectEventPosition(evidence.eventPosition)}>
                  <TableCell className="font-mono">#{evidence.eventPosition}</TableCell>
                  <TableCell>{humanizeEnumValue(evidence.reason)}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{evidence.sharesAhead}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{evidence.ordersAhead}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{evidence.levelTotalShares}</TableCell>
                  <TableCell>{evidence.source === "exact_simulated_truth" ? "exact" : "estimated"}</TableCell>
                </TableRow>
              ))}
              {lifecycle.fifoAheadEvidence.length === 0 ? (
                <TableRow>
                  <TableCell colSpan={6} className="text-muted-foreground">
                    The order never rested, so it had no queue position.
                  </TableCell>
                </TableRow>
              ) : null}
            </TableBody>
          </Table>
        </CardContent>
      </Card>
      {focusedFill ? (
        <Card size="sm">
          <CardHeader>
            <CardTitle>
              Fill {focusedFill.matchId}: {focusedFill.side} {focusedFill.quantity} @ {focusedFill.price} ({focusedFill.liquidityRole})
            </CardTitle>
            <CardDescription>
              Order {focusedFill.orderId} at event {focusedFill.eventPosition}. Counterparty {humanizeEnumValue(focusedFill.counterpartyActor)}. Mid at fill {formatDecimalForDisplay(focusedFill.midAtFill)}, spread capture {formatDecimalForDisplay(focusedFill.spreadCapturePerShare, { signDisplay: "always" })} per share, fee{" "}
              {formatDecimalForDisplay(focusedFill.feePerShare)} per share.
            </CardDescription>
          </CardHeader>
          <CardContent className="grid gap-3 md:grid-cols-2">
            <Table className="text-xs">
              <TableHeader>
                <TableRow>
                  <TableHead>Ledger</TableHead>
                  <TableHead className="text-right">Before</TableHead>
                  <TableHead className="text-right">After</TableHead>
                </TableRow>
              </TableHeader>
              <TableBody>
                <TableRow>
                  <TableCell>Position (shares)</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{focusedFill.ledgerEffect.positionBeforeShares}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{focusedFill.ledgerEffect.positionAfterShares}</TableCell>
                </TableRow>
                <TableRow>
                  <TableCell>Cash</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{formatDecimalForDisplay(focusedFill.ledgerEffect.cashBefore)}</TableCell>
                  <TableCell className="text-right font-mono tabular-nums">{formatDecimalForDisplay(focusedFill.ledgerEffect.cashAfter)}</TableCell>
                </TableRow>
                <TableRow>
                  <TableCell>Fee on this fill</TableCell>
                  <TableCell />
                  <TableCell className="text-right font-mono tabular-nums">{formatDecimalForDisplay(focusedFill.ledgerEffect.fee, { signDisplay: "always" })}</TableCell>
                </TableRow>
                <TableRow>
                  <TableCell>Realized P&L change</TableCell>
                  <TableCell />
                  <TableCell className="text-right font-mono tabular-nums">{formatDecimalForDisplay(focusedFill.ledgerEffect.realizedPnlChange, { signDisplay: "always" })}</TableCell>
                </TableRow>
                <TableRow>
                  <TableCell>Cumulative realized / fees</TableCell>
                  <TableCell />
                  <TableCell className="text-right font-mono tabular-nums">
                    {formatDecimalForDisplay(focusedFill.ledgerEffect.cumulativeRealizedPnlAfter)} / {formatDecimalForDisplay(focusedFill.ledgerEffect.cumulativeFeesAfter)}
                  </TableCell>
                </TableRow>
              </TableBody>
            </Table>
            <Table className="text-xs">
              <TableHeader>
                <TableRow>
                  <TableHead>Markout</TableHead>
                  <TableHead>Status</TableHead>
                  <TableHead className="text-right">Ref mid</TableHead>
                  <TableHead className="text-right">Per share</TableHead>
                  <TableHead className="text-right">Total</TableHead>
                </TableRow>
              </TableHeader>
              <TableBody>
                {focusedFill.markouts.map((markout) => (
                  <TableRow key={markout.horizonNs}>
                    <TableCell>{formatDurationNs(markout.horizonNs)}</TableCell>
                    <TableCell>
                      <Badge variant={markout.status === "available" ? "secondary" : "outline"}>{humanizeEnumValue(markout.status)}</Badge>
                    </TableCell>
                    <TableCell className="text-right font-mono tabular-nums">{formatDecimalForDisplay(markout.referenceMidPrice)}</TableCell>
                    <TableCell className="text-right font-mono tabular-nums">{formatDecimalForDisplay(markout.markoutPerShare, { signDisplay: "always" })}</TableCell>
                    <TableCell className="text-right font-mono tabular-nums">{formatDecimalForDisplay(markout.markoutTotal, { signDisplay: "always" })}</TableCell>
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          </CardContent>
        </Card>
      ) : null}
    </div>
  );
}

export function RunInspectView() {
  const { runId } = useParams<{ runId: string }>();
  return <RunInspection key={runId} runId={runId} />;
}

function RunInspection({ runId }: { runId: string }) {
  const comparisonQuery = useComparisonContext();
  const apiClient = getBrowserApiClient();
  const [focusedEventPosition, setFocusedEventPosition] = useState("0");
  const [jumpTargetText, setJumpTargetText] = useState("");
  const [focusedOrderId, setFocusedOrderId] = useState<string | null>(null);
  const [selectedFill, setSelectedFill] = useState<ApiSchemas["Fill"] | null>(null);
  const fillsCursor = useCursorHistory();
  const ordersCursor = useCursorHistory();

  const runResource = useApiResource(`inspect-run:${runId}`, async () => unwrapApiResult(await apiClient.GET("/v1/runs/{runId}", { params: { path: { runId } } })), (latestRun) => (isTerminalRunState(latestRun.state) ? null : 2000));
  const run = runResource.data;
  const processedEventCount = run?.progress.processedCanonicalEventCount ?? "0";
  const maximumEventPosition = lastEventPosition(processedEventCount);
  const windowStart = offsetUnsignedIntegerText(focusedEventPosition, -EVENT_WINDOW_RADIUS, "0", maximumEventPosition);
  const windowEnd = offsetUnsignedIntegerText(focusedEventPosition, EVENT_WINDOW_RADIUS, "0", maximumEventPosition);
  const hasEvents = processedEventCount !== "0";

  const eventWindow = useApiResource(
    run && hasEvents ? `events:${runId}:${windowStart}:${windowEnd}` : null,
    async () => unwrapApiResult(await apiClient.GET("/v1/runs/{runId}/events", { params: { path: { runId }, query: { fromEventPosition: windowStart, toEventPosition: windowEnd, limit: 100 } } })),
    null,
  );
  const exchangeBook = useApiResource(
    run && hasEvents ? `book-truth:${runId}:${focusedEventPosition}` : null,
    async () => unwrapApiResult(await apiClient.GET("/v1/runs/{runId}/book", { params: { path: { runId }, query: { eventPosition: focusedEventPosition, view: "exchange_truth", depth: BOOK_DEPTH } } })),
    null,
  );
  const strategyBook = useApiResource(
    run && hasEvents ? `book-visible:${runId}:${focusedEventPosition}` : null,
    async () => unwrapApiResult(await apiClient.GET("/v1/runs/{runId}/book", { params: { path: { runId }, query: { eventPosition: focusedEventPosition, view: "strategy_visible", depth: BOOK_DEPTH } } })),
    null,
  );
  const fillPage = useApiResource(run ? `fills:${runId}:${processedEventCount}:${fillsCursor.cursor ?? "first"}` : null, async () => unwrapApiResult(await apiClient.GET("/v1/runs/{runId}/fills", { params: { path: { runId }, query: { limit: 200, cursor: fillsCursor.cursor } } })), null);
  const orderPage = useApiResource(run ? `orders:${runId}:${processedEventCount}:${ordersCursor.cursor ?? "first"}` : null, async () => unwrapApiResult(await apiClient.GET("/v1/runs/{runId}/orders", { params: { path: { runId }, query: { limit: 200, cursor: ordersCursor.cursor } } })), null);
  const lifecycleResource = useApiResource(
    focusedOrderId ? `lifecycle:${runId}:${focusedOrderId}:${processedEventCount}` : null,
    async () => unwrapApiResult(await apiClient.GET("/v1/runs/{runId}/orders/{orderId}/lifecycle", { params: { path: { runId, orderId: focusedOrderId! } } })),
    null,
  );

  const selectedFillResource = useApiResource(
    selectedFill ? `selected-fill:${runId}:${selectedFill.orderId}:${selectedFill.eventPosition}:${processedEventCount}` : null,
    (signal) => readSelectedFill(apiClient, runId, selectedFill!.orderId, selectedFill!.eventPosition, signal),
    null,
  );
  const refreshedFill = selectedFillResource.data;
  const focusedFill = refreshedFill?.eventPosition === selectedFill?.eventPosition && refreshedFill?.orderId === selectedFill?.orderId
    ? refreshedFill
    : fillPage.data?.items.find((fill) => fill.eventPosition === selectedFill?.eventPosition && fill.orderId === selectedFill?.orderId) ?? null;
  const relatedEventPositions = new Set<string>(lifecycleResource.data?.relatedEventPositions ?? []);
  if (focusedFill) {
    for (const relatedPosition of Object.values(focusedFill.relatedEvents)) if (relatedPosition !== null) relatedEventPositions.add(relatedPosition);
  }

  const selectEventPosition = (eventPosition: string) => setFocusedEventPosition(offsetUnsignedIntegerText(eventPosition, 0n, "0", maximumEventPosition));
  const stepEventPosition = (stepOffset: bigint) => setFocusedEventPosition(offsetUnsignedIntegerText(focusedEventPosition, stepOffset, "0", maximumEventPosition));
  const focusFill = (fill: ApiSchemas["Fill"]) => {
    setSelectedFill(fill);
    setFocusedOrderId(fill.orderId);
    selectEventPosition(fill.eventPosition);
  };
  const focusOrder = (order: ApiSchemas["OwnOrderSummary"]) => {
    setSelectedFill(null);
    setFocusedOrderId(order.orderId);
    selectEventPosition(order.submitEventPosition);
  };

  const handleStepperKeys = (keyboardEvent: KeyboardEvent<HTMLDivElement>) => {
    if (keyboardEvent.target instanceof HTMLInputElement) return;
    const stepSize = keyboardEvent.shiftKey ? LARGE_STEP_EVENT_COUNT : 1n;
    if (keyboardEvent.key === "ArrowRight" || keyboardEvent.key === "j") stepEventPosition(stepSize);
    else if (keyboardEvent.key === "ArrowLeft" || keyboardEvent.key === "k") stepEventPosition(-stepSize);
    else if (keyboardEvent.key === "Home") selectEventPosition("0");
    else if (keyboardEvent.key === "End") selectEventPosition(maximumEventPosition);
    else return;
    keyboardEvent.preventDefault();
  };

  if (runResource.failure && !run) return <ApiFailureAlert failure={runResource.failure} title="Could not load run" onRetry={runResource.reload} />;
  if (!run) return <p className="text-sm text-muted-foreground">Loading run...</p>;

  return (
    <>
      <PageHeading
        title={
          <span className="flex items-center gap-2">
            Inspect seed <span>{run.masterSeed}</span> <RunStateBadge runState={run.state} />
          </span>
        }
        description={
          <span>
            <span className="font-mono text-xs">{run.runId}</span>{" / "} seed <span className="font-mono">{run.masterSeed}</span>, {formatIntegerText(processedEventCount)} events processed.{" "}
            <Link className="underline" href={`/runs/${run.runId}${comparisonQuery}`}>
              Run summary
            </Link>
          </span>
        }
      />
      <RunResearchContext experimentId={run.experimentId} />
      {runResource.failure ? <ApiFailureAlert failure={runResource.failure} title="Run status is stale. Showing the last known event range." onRetry={runResource.reload} /> : null}
      <div
        role="group"
        aria-label="Event position stepper. Arrow keys or j and k step one event, Shift steps 25, Home and End jump to the ends."
        tabIndex={0}
        onKeyDown={handleStepperKeys}
        className="inspection-toolbar flex flex-wrap items-center gap-2 border focus-visible:outline-2 focus-visible:outline-ring"
      >
        <Button size="icon-sm" variant="outline" aria-label="First event" onClick={() => selectEventPosition("0")}>
          <ChevronFirstIcon />
        </Button>
        <Button size="icon-sm" variant="outline" aria-label="Back 25 events" onClick={() => stepEventPosition(-LARGE_STEP_EVENT_COUNT)}>
          <ChevronsLeftIcon />
        </Button>
        <Button size="icon-sm" variant="outline" aria-label="Previous event" onClick={() => stepEventPosition(-1n)}>
          <ChevronLeftIcon />
        </Button>
        <span className="min-w-40 text-center font-mono text-sm tabular-nums">
          event {focusedEventPosition} / {maximumEventPosition}
        </span>
        <Button size="icon-sm" variant="outline" aria-label="Next event" onClick={() => stepEventPosition(1n)}>
          <ChevronRightIcon />
        </Button>
        <Button size="icon-sm" variant="outline" aria-label="Forward 25 events" onClick={() => stepEventPosition(LARGE_STEP_EVENT_COUNT)}>
          <ChevronsRightIcon />
        </Button>
        <Button size="icon-sm" variant="outline" aria-label="Last event" onClick={() => selectEventPosition(maximumEventPosition)}>
          <ChevronLastIcon />
        </Button>
        <form
          className="flex items-center gap-1"
          onSubmit={(submitEvent) => {
            submitEvent.preventDefault();
            if (/^(0|[1-9][0-9]*)$/.test(jumpTargetText)) selectEventPosition(jumpTargetText);
          }}
        >
          <Input aria-label="Jump to event position" placeholder="event #" className="h-8 w-28 font-mono" inputMode="numeric" value={jumpTargetText} onChange={(changeEvent) => setJumpTargetText(changeEvent.target.value)} />
          <Button size="sm" variant="outline" type="submit">
            Go
          </Button>
        </form>
        <span className="text-xs text-muted-foreground">Focus this bar, then use the arrow keys (Shift for 25).</span>
      </div>

      <div className="grid gap-4 2xl:grid-cols-[minmax(0,3fr)_minmax(0,2fr)]">
        <div className="flex min-w-0 flex-col gap-4">
          {eventWindow.failure ? <ApiFailureAlert failure={eventWindow.failure} onRetry={eventWindow.reload} /> : null}
          {eventWindow.data ? (
            <EventWindowTable events={eventWindow.data.items} focusedEventPosition={focusedEventPosition} relatedEventPositions={relatedEventPositions} onSelectEventPosition={selectEventPosition} />
          ) : (
            <p className="text-sm text-muted-foreground">{hasEvents ? "Loading events..." : "No events processed yet."}</p>
          )}
          <div className="grid gap-4 md:grid-cols-2">
            <div>{exchangeBook.failure ? <ApiFailureAlert failure={exchangeBook.failure} onRetry={exchangeBook.reload} /> : <BookLadder title="Exchange truth" bookView={exchangeBook.data} />}</div>
            <div>{strategyBook.failure ? <ApiFailureAlert failure={strategyBook.failure} onRetry={strategyBook.reload} /> : <BookLadder title="Strategy-visible" bookView={strategyBook.data} />}</div>
          </div>
        </div>
        <div className="flex min-w-0 flex-col gap-3">
          {selectedFillResource.failure ? <ApiFailureAlert failure={selectedFillResource.failure} title="Could not refresh the selected fill" onRetry={selectedFillResource.reload} /> : null}
          <FocusPanel focusedFill={focusedFill} lifecycle={lifecycleResource.data} lifecycleFailure={lifecycleResource.failure} onRetryLifecycle={lifecycleResource.reload} onSelectEventPosition={selectEventPosition} />
        </div>
      </div>

      <Tabs defaultValue="fills">
        <TabsList>
          <TabsTrigger value="fills">Fills</TabsTrigger>
          <TabsTrigger value="orders">Own orders</TabsTrigger>
        </TabsList>
        <TabsContent value="fills">
          <ArtifactPageControls label="Fills" pageNumber={fillsCursor.pageNumber} count={fillPage.data?.items.length ?? 0} nextCursor={fillPage.data?.page.nextCursor ?? null} isLoading={fillPage.isLoading} onPrevious={fillsCursor.previous} onNext={fillsCursor.next} />
          {fillPage.failure ? <ApiFailureAlert failure={fillPage.failure} onRetry={fillPage.reload} /> : null}
          <div className="event-tape overflow-hidden rounded-md border">
            <Table aria-label="Recorded fills" className="text-xs">
              <TableHeader>
                <TableRow>
                  <TableHead>Match</TableHead>
                  <TableHead className="text-right">Event</TableHead>
                  <TableHead className="text-right">Sim time</TableHead>
                  <TableHead>Order</TableHead>
                  <TableHead>Side</TableHead>
                  <TableHead className="text-right">Price</TableHead>
                  <TableHead className="text-right">Qty</TableHead>
                  <TableHead>Role</TableHead>
                  <TableHead className="text-right">Queue ahead at rest</TableHead>
                  <TableHead className="text-right">Position after</TableHead>
                  <TableHead className="text-right">Realized change</TableHead>
                </TableRow>
              </TableHeader>
              <TableBody>
                {fillPage.data?.items.length === 0 ? <TableRow><TableCell colSpan={11} className="text-muted-foreground">No fills recorded on this page.</TableCell></TableRow> : null}
                {fillPage.data?.items.map((fill) => (
                  <TableRow
                    key={`${fill.eventPosition}:${fill.orderId}`}
                    tabIndex={0}
                    data-state={fill.eventPosition === selectedFill?.eventPosition && fill.orderId === selectedFill?.orderId ? "selected" : undefined}
                    className="cursor-pointer focus-visible:outline-2 focus-visible:outline-ring"
                    onClick={() => focusFill(fill)}
                    onKeyDown={(keyboardEvent) => keyboardEvent.key === "Enter" && focusFill(fill)}
                  >
                    <TableCell className="font-mono">{fill.matchId}</TableCell>
                    <TableCell className="text-right font-mono tabular-nums">{fill.eventPosition}</TableCell>
                    <TableCell className="text-right font-mono tabular-nums">{formatSimulationTimeNs(fill.simulationTimeNs, 6)}</TableCell>
                    <TableCell className="font-mono">{fill.orderId}</TableCell>
                    <TableCell>{fill.side}</TableCell>
                    <TableCell className="text-right font-mono tabular-nums">{fill.price}</TableCell>
                    <TableCell className="text-right font-mono tabular-nums">{fill.quantity}</TableCell>
                    <TableCell>{fill.liquidityRole}</TableCell>
                    <TableCell className="text-right font-mono tabular-nums">{fill.queueAheadAtRestShares ?? "n/a"}</TableCell>
                    <TableCell className="text-right font-mono tabular-nums">{fill.ledgerEffect.positionAfterShares}</TableCell>
                    <TableCell className="text-right font-mono tabular-nums">{formatDecimalForDisplay(fill.ledgerEffect.realizedPnlChange, { signDisplay: "always" })}</TableCell>
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          </div>
        </TabsContent>
        <TabsContent value="orders">
          <ArtifactPageControls label="Orders" pageNumber={ordersCursor.pageNumber} count={orderPage.data?.items.length ?? 0} nextCursor={orderPage.data?.page.nextCursor ?? null} isLoading={orderPage.isLoading} onPrevious={ordersCursor.previous} onNext={ordersCursor.next} />
          {orderPage.failure ? <ApiFailureAlert failure={orderPage.failure} onRetry={orderPage.reload} /> : null}
          <div className="max-h-[28rem] overflow-auto rounded-md border">
            <Table aria-label="Recorded own orders" className="text-xs">
              <TableHeader>
                <TableRow>
                  <TableHead>Order</TableHead>
                  <TableHead>Client id</TableHead>
                  <TableHead className="text-right">Submitted at</TableHead>
                  <TableHead>Side</TableHead>
                  <TableHead className="text-right">Price</TableHead>
                  <TableHead className="text-right">Qty</TableHead>
                  <TableHead className="text-right">Filled</TableHead>
                  <TableHead>Final status</TableHead>
                </TableRow>
              </TableHeader>
              <TableBody>
                {orderPage.data?.items.length === 0 ? <TableRow><TableCell colSpan={8} className="text-muted-foreground">No own orders recorded on this page.</TableCell></TableRow> : null}
                {orderPage.data?.items.map((order) => (
                  <TableRow
                    key={order.orderId}
                    tabIndex={0}
                    data-state={order.orderId === focusedOrderId ? "selected" : undefined}
                    className="cursor-pointer focus-visible:outline-2 focus-visible:outline-ring"
                    onClick={() => focusOrder(order)}
                    onKeyDown={(keyboardEvent) => keyboardEvent.key === "Enter" && focusOrder(order)}
                  >
                    <TableCell className="font-mono">{order.orderId}</TableCell>
                    <TableCell className="font-mono">{order.clientOrderId}</TableCell>
                    <TableCell className="text-right font-mono tabular-nums">
                      #{order.submitEventPosition} {formatSimulationTimeNs(order.submitSimulationTimeNs, 6)}
                    </TableCell>
                    <TableCell>{order.side}</TableCell>
                    <TableCell className="text-right font-mono tabular-nums">{order.price}</TableCell>
                    <TableCell className="text-right font-mono tabular-nums">{order.quantity}</TableCell>
                    <TableCell className="text-right font-mono tabular-nums">{order.cumulativeQuantity}</TableCell>
                    <TableCell>{humanizeEnumValue(order.finalStatus)}</TableCell>
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          </div>
        </TabsContent>
      </Tabs>
    </>
  );
}
