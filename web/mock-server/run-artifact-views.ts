// Mock-only: turns simulated artifacts into the API's summary, series, fill,
// order-lifecycle and book views, truncated to the run's processed event
// count. Accounting follows the published convention: cash and fees at 4
// fractional digits, marks and markouts at 5, FIFO cost basis.

import type { components } from "../src/lib/api/generated-schema";
import {
  formatDerivedStatistic,
  formatScaledInteger,
  MID_SCALE_DIGITS,
  parseScaledDecimal,
  PRICE_SCALE_DIGITS,
} from "./fixed-point-decimal";
import type { ExperimentConfig, MidPricePoint, OwnFillRecord, OwnOrderRecord, SimulatedRunArtifacts } from "./synthetic-market-simulation";

type Schemas = components["schemas"];

const formatPrice = (units: bigint): string => formatScaledInteger(units, PRICE_SCALE_DIGITS);
const formatMid = (tenthUnits: bigint): string => formatScaledInteger(tenthUnits, MID_SCALE_DIGITS);

export interface ArtifactCutoff {
  /** Events with eventPosition < processedEventCount are included. */
  processedEventCount: number;
  /** Simulated time up to which markouts can be observed. */
  observableUntilTimeNs: bigint;
}

export function createArtifactCutoff(artifacts: SimulatedRunArtifacts, processedEventCount: number): ArtifactCutoff {
  const isComplete = processedEventCount >= artifacts.events.length;
  const lastIncludedEvent = artifacts.events[processedEventCount - 1];
  return {
    processedEventCount,
    observableUntilTimeNs: isComplete ? artifacts.endTimeNs : lastIncludedEvent ? BigInt(lastIncludedEvent.simulationTimeNs) : 0n,
  };
}

function isIncluded(eventPosition: number | null, cutoff: ArtifactCutoff): eventPosition is number {
  return eventPosition !== null && eventPosition < cutoff.processedEventCount;
}

export function findMidAtOrBefore(midPricePoints: MidPricePoint[], timeNs: bigint, cutoff: ArtifactCutoff): bigint | null {
  let midTenthUnits: bigint | null = null;
  for (const point of midPricePoints) {
    if (point.timeNs > timeNs || point.eventPosition >= cutoff.processedEventCount) break;
    // Mark convention: the last two-sided exchange mid at or before the time.
    // A one-sided book does not erase the mark.
    if (point.midTenthUnits !== null) midTenthUnits = point.midTenthUnits;
  }
  return midTenthUnits;
}

export interface EquitySample {
  eventPosition: number;
  timeNs: bigint;
  positionShares: bigint;
  cashUnits: bigint;
  midTenthUnits: bigint | null;
  equityTenthUnits: bigint | null;
}

export function buildEquitySamples(artifacts: SimulatedRunArtifacts, cutoff: ArtifactCutoff): EquitySample[] {
  const samples: EquitySample[] = [];
  let positionShares = 0n;
  let cashUnits = artifacts.initialCashUnits;
  let midTenthUnits: bigint | null = null;
  let fillIndex = 0;
  let midIndex = 0;
  const fills = artifacts.ownFills.filter((fill) => isIncluded(fill.eventPosition, cutoff));
  const mids = artifacts.midPricePoints.filter((point) => isIncluded(point.eventPosition, cutoff));
  while (fillIndex < fills.length || midIndex < mids.length) {
    const nextFill = fills[fillIndex];
    const nextMid = mids[midIndex];
    let eventPosition: number;
    let timeNs: bigint;
    if (nextMid && (!nextFill || nextMid.eventPosition <= nextFill.eventPosition)) {
      if (nextMid.midTenthUnits !== null) midTenthUnits = nextMid.midTenthUnits;
      eventPosition = nextMid.eventPosition;
      timeNs = nextMid.timeNs;
      midIndex++;
    } else {
      positionShares = nextFill.ledgerAfter.positionShares;
      cashUnits = nextFill.ledgerAfter.cashUnits;
      eventPosition = nextFill.eventPosition;
      timeNs = nextFill.timeNs;
      fillIndex++;
    }
    samples.push({
      eventPosition,
      timeNs,
      positionShares,
      cashUnits,
      midTenthUnits,
      equityTenthUnits: midTenthUnits === null ? null : cashUnits * 10n + positionShares * midTenthUnits,
    });
  }
  return samples;
}

interface OpenLotsState {
  signedOpenCostUnits: bigint;
  positionShares: bigint;
}

/** Recomputes FIFO open lots from fills, independently of the simulation's running ledger. */
export function recomputeOpenLots(fills: OwnFillRecord[]): OpenLotsState {
  const lots: { side: Schemas["Side"]; priceUnits: bigint; shares: bigint }[] = [];
  let positionShares = 0n;
  for (const fill of fills) {
    let remainingShares = fill.quantity;
    while (remainingShares > 0n && lots.length > 0 && lots[0].side !== fill.side) {
      const closedShares = remainingShares < lots[0].shares ? remainingShares : lots[0].shares;
      lots[0].shares -= closedShares;
      remainingShares -= closedShares;
      if (lots[0].shares === 0n) lots.shift();
    }
    if (remainingShares > 0n) lots.push({ side: fill.side, priceUnits: fill.price, shares: remainingShares });
    positionShares += fill.side === "buy" ? fill.quantity : -fill.quantity;
  }
  const signedOpenCostUnits = lots.reduce((total, lot) => total + lot.priceUnits * (lot.side === "buy" ? lot.shares : -lot.shares), 0n);
  return { signedOpenCostUnits, positionShares };
}

function computeMaxDrawdown(samples: EquitySample[]): { drawdownTenthUnits: bigint; peakTimeNs: bigint | null; troughTimeNs: bigint | null } {
  let peakEquity: bigint | null = null;
  let peakTimeNs: bigint | null = null;
  let worst = { drawdownTenthUnits: 0n, peakTimeNs: null as bigint | null, troughTimeNs: null as bigint | null };
  for (const sample of samples) {
    if (sample.equityTenthUnits === null) continue;
    if (peakEquity === null || sample.equityTenthUnits > peakEquity) {
      peakEquity = sample.equityTenthUnits;
      peakTimeNs = sample.timeNs;
    }
    const drawdown = peakEquity - sample.equityTenthUnits;
    if (drawdown > worst.drawdownTenthUnits) worst = { drawdownTenthUnits: drawdown, peakTimeNs, troughTimeNs: sample.timeNs };
  }
  return worst;
}

export function computeFillMarkouts(fill: OwnFillRecord, artifacts: SimulatedRunArtifacts, horizonsNs: string[], cutoff: ArtifactCutoff): Schemas["FillMarkout"][] {
  return horizonsNs.map((horizonText) => {
    const targetTimeNs = fill.timeNs + BigInt(horizonText);
    if (targetTimeNs > cutoff.observableUntilTimeNs) {
      return { horizonNs: horizonText, status: "censored_past_run_end", referenceMidPrice: null, markoutPerShare: null, markoutTotal: null };
    }
    const midTenthUnits = findMidAtOrBefore(artifacts.midPricePoints, targetTimeNs, cutoff);
    if (midTenthUnits === null) {
      return { horizonNs: horizonText, status: "unavailable_no_mark", referenceMidPrice: null, markoutPerShare: null, markoutTotal: null };
    }
    const perShare = fill.side === "buy" ? midTenthUnits - fill.price * 10n : fill.price * 10n - midTenthUnits;
    return {
      horizonNs: horizonText,
      status: "available",
      referenceMidPrice: formatMid(midTenthUnits),
      markoutPerShare: formatMid(perShare),
      markoutTotal: formatMid(perShare * fill.quantity),
    };
  });
}

function spreadCapturePerShareTenthUnits(fill: OwnFillRecord): bigint | null {
  if (fill.midBeforeFillTenthUnits === null) return null;
  return fill.side === "buy" ? fill.midBeforeFillTenthUnits - fill.price * 10n : fill.price * 10n - fill.midBeforeFillTenthUnits;
}

function metric(metricKey: Schemas["MetricKey"], value: string | null, unit: Schemas["MetricUnit"], exactness: Schemas["MetricExactness"] = "exact_fixed_point", horizonNs: string | null = null, unavailableReason: string | null = null): Schemas["MetricValue"] {
  return { metricKey, horizonNs, value, unit, exactness, unavailableReason: value === null ? unavailableReason ?? "unavailable" : null };
}

function summarizeLatency(component: Schemas["LatencyMeasurement"]["component"], configuredNs: string, samples: bigint[]): Schemas["LatencyMeasurement"] {
  let minimum: bigint | null = null;
  let maximum: bigint | null = null;
  for (const sample of samples) {
    if (minimum === null || sample < minimum) minimum = sample;
    if (maximum === null || sample > maximum) maximum = sample;
  }
  return { component, configuredNs, sampleCount: String(samples.length), observedMinNs: minimum === null ? null : String(minimum), observedMaxNs: maximum === null ? null : String(maximum) };
}

export function buildRunSummary(runId: string, config: ExperimentConfig, artifacts: SimulatedRunArtifacts, cutoff: ArtifactCutoff, availability: "final" | "partial"): Schemas["RunSummary"] {
  const includedFills = artifacts.ownFills.filter((fill) => isIncluded(fill.eventPosition, cutoff));
  const ledgerAfter = includedFills.at(-1)?.ledgerAfter ?? { positionShares: 0n, cashUnits: artifacts.initialCashUnits, cumulativeRealizedPnlUnits: 0n, cumulativeFeesUnits: 0n };
  const lastEvent = artifacts.events[cutoff.processedEventCount - 1];
  const asOfTimeNs = lastEvent ? BigInt(lastEvent.simulationTimeNs) : 0n;
  const markTenthUnits = config.markConvention === "exchange_mid" ? findMidAtOrBefore(artifacts.midPricePoints, asOfTimeNs, cutoff) : null;
  const markUnavailableReason = config.markConvention !== "exchange_mid"
    ? `mark convention ${config.markConvention} is not produced by this service build`
    : markTenthUnits === null ? "no two-sided exchange book at the summary time" : null;
  const openLots = recomputeOpenLots(includedFills);
  const unrealizedTenthUnits = markTenthUnits === null ? null : openLots.positionShares * markTenthUnits - openLots.signedOpenCostUnits * 10n;
  const equityTenthUnits = markTenthUnits === null ? null : ledgerAfter.cashUnits * 10n + ledgerAfter.positionShares * markTenthUnits;
  const netPnlTenthUnits = equityTenthUnits === null ? null : equityTenthUnits - artifacts.initialCashUnits * 10n;
  const absolutePosition = openLots.positionShares < 0n ? -openLots.positionShares : openLots.positionShares;
  const averageCostBasis = absolutePosition === 0n
    ? null
    : formatDerivedStatistic(Number(openLots.signedOpenCostUnits < 0n ? -openLots.signedOpenCostUnits : openLots.signedOpenCostUnits) / Number(absolutePosition) / 10_000);

  const submittedOrders = artifacts.ownOrdersInSubmitOrder.filter((order) => isIncluded(order.submitEventPosition, cutoff));
  const acceptedCount = submittedOrders.filter((order) => isIncluded(order.acceptedEventPosition, cutoff)).length;
  const canceledCount = submittedOrders.filter((order) => order.stages.some((stage) => stage.stageKind === "canceled" && Number(stage.eventPosition) < cutoff.processedEventCount)).length;
  const cancelRaceLostCount = artifacts.cancelRaceLostEventPositions.filter((position) => position < cutoff.processedEventCount).length;
  const submittedShares = submittedOrders.reduce((total, order) => total + order.quantity, 0n);
  const filledShares = includedFills.reduce((total, fill) => total + fill.quantity, 0n);
  const makerFillCount = includedFills.filter((fill) => fill.liquidityRole === "maker").length;
  let quoteExposureTimeNs = 0n;
  for (const order of submittedOrders) {
    if (!isIncluded(order.restedEventPosition, cutoff) || order.restedTimeNs === null) continue;
    const removedTimeNs = isIncluded(order.removedFromBookEventPosition, cutoff) && order.removedFromBookTimeNs !== null ? order.removedFromBookTimeNs : asOfTimeNs;
    quoteExposureTimeNs += removedTimeNs - order.restedTimeNs;
  }

  let maxLong = 0n;
  let maxShort = 0n;
  for (const fill of includedFills) {
    if (fill.ledgerAfter.positionShares > maxLong) maxLong = fill.ledgerAfter.positionShares;
    if (fill.ledgerAfter.positionShares < maxShort) maxShort = fill.ledgerAfter.positionShares;
  }
  const drawdown = computeMaxDrawdown(buildEquitySamples(artifacts, cutoff));
  const blockedOrderCount = artifacts.blockedOrderEventPositions.filter((position) => position < cutoff.processedEventCount).length;

  let spreadCaptureTenthUnits = 0n;
  for (const fill of includedFills) {
    const perShare = spreadCapturePerShareTenthUnits(fill);
    if (perShare !== null) spreadCaptureTenthUnits += perShare * fill.quantity;
  }

  const markoutSummaries: Schemas["MarkoutHorizonSummary"][] = config.markoutHorizonsNs.map((horizonText) => {
    let availableFillCount = 0;
    let censoredFillCount = 0;
    let unavailableFillCount = 0;
    let totalTenthUnits = 0n;
    let availableShares = 0n;
    for (const fill of includedFills) {
      const markout = computeFillMarkouts(fill, artifacts, [horizonText], cutoff)[0];
      if (markout.status === "available" && markout.markoutTotal !== null) {
        availableFillCount++;
        availableShares += fill.quantity;
        totalTenthUnits += parseScaledDecimal(markout.markoutTotal, MID_SCALE_DIGITS);
      } else if (markout.status === "censored_past_run_end") censoredFillCount++;
      else if (markout.status === "unavailable_no_mark") unavailableFillCount++;
    }
    return {
      horizonNs: horizonText,
      availableFillCount: String(availableFillCount),
      unavailableFillCount: String(unavailableFillCount),
      censoredFillCount: String(censoredFillCount),
      totalMarkout: formatMid(totalTenthUnits),
      meanMarkoutPerShare: availableShares === 0n ? null : formatDerivedStatistic(Number(totalTenthUnits) / Number(availableShares) / 100_000),
    };
  });

  // Latency measured from the canonical events themselves.
  const latencyByComponent: Record<Schemas["LatencyMeasurement"]["component"], bigint[]> = { feed: [], order_entry: [], acknowledgment: [], cancel: [] };
  for (const event of artifacts.events.slice(0, cutoff.processedEventCount)) {
    if (event.eventKind === "market_data_delivered" && event.sourceEventPosition !== null) {
      latencyByComponent.feed.push(BigInt(event.simulationTimeNs) - BigInt(artifacts.events[Number(event.sourceEventPosition)].simulationTimeNs));
    }
  }
  for (const order of submittedOrders) {
    if (isIncluded(order.arrivalEventPosition, cutoff) && order.arrivalTimeNs !== null) latencyByComponent.order_entry.push(order.arrivalTimeNs - order.submitTimeNs);
    if (isIncluded(order.acknowledgmentDeliveredEventPosition, cutoff) && order.acknowledgmentDeliveredTimeNs !== null && order.acceptedTimeNs !== null) {
      latencyByComponent.acknowledgment.push(order.acknowledgmentDeliveredTimeNs - order.acceptedTimeNs);
    }
    const cancelArrivalStage = order.stages.find((stage) => stage.stageKind === "cancel_arrived_at_exchange");
    if (cancelArrivalStage && Number(cancelArrivalStage.eventPosition) < cutoff.processedEventCount && order.cancelSubmitTimeNs !== null) {
      latencyByComponent.cancel.push(BigInt(cancelArrivalStage.simulationTimeNs) - order.cancelSubmitTimeNs);
    }
  }

  const accounting: Schemas["AccountingSummary"] = {
    initialCash: formatPrice(artifacts.initialCashUnits),
    cash: formatPrice(ledgerAfter.cashUnits),
    positionShares: String(ledgerAfter.positionShares),
    averageCostBasis,
    grossRealizedPnl: formatPrice(ledgerAfter.cumulativeRealizedPnlUnits),
    unrealizedPnl: unrealizedTenthUnits === null ? null : formatMid(unrealizedTenthUnits),
    netFees: formatPrice(ledgerAfter.cumulativeFeesUnits),
    markPrice: markTenthUnits === null ? null : formatMid(markTenthUnits),
    markConvention: config.markConvention,
    equity: equityTenthUnits === null ? null : formatMid(equityTenthUnits),
    netPnl: netPnlTenthUnits === null ? null : formatMid(netPnlTenthUnits),
    markUnavailableReason,
  };
  const fillRateRatio = submittedShares === 0n ? null : formatDerivedStatistic(Number(filledShares) / Number(submittedShares));
  const orders: Schemas["OrderActivitySummary"] = {
    submittedOrderCount: String(submittedOrders.length),
    acceptedOrderCount: String(acceptedCount),
    rejectedOrderCount: "0",
    canceledOrderCount: String(canceledCount),
    cancelRaceLostCount: String(cancelRaceLostCount),
    fillCount: String(includedFills.length),
    makerFillCount: String(makerFillCount),
    takerFillCount: String(includedFills.length - makerFillCount),
    filledShares: String(filledShares),
    submittedShares: String(submittedShares),
    fillRateByShares: {
      numerator: String(filledShares),
      denominator: String(submittedShares),
      ratio: fillRateRatio,
      definition: "filled shares of own orders divided by shares of own orders submitted (including orders later cancelled)",
    },
    quoteExposureTimeNs: String(quoteExposureTimeNs),
  };
  const risk: Schemas["RiskSummary"] = {
    maxLongPositionShares: String(maxLong),
    maxShortPositionShares: String(maxShort),
    blockedOrderCount: String(blockedOrderCount),
    maxDrawdown: formatMid(drawdown.drawdownTenthUnits),
    maxDrawdownPeakTimeNs: drawdown.peakTimeNs === null ? null : String(drawdown.peakTimeNs),
    maxDrawdownTroughTimeNs: drawdown.troughTimeNs === null ? null : String(drawdown.troughTimeNs),
  };
  const markUnavailable = markUnavailableReason ?? undefined;
  const metrics: Schemas["MetricValue"][] = [
    metric("net_pnl", accounting.netPnl, "currency", "exact_fixed_point", null, markUnavailable),
    metric("equity", accounting.equity, "currency", "exact_fixed_point", null, markUnavailable),
    metric("gross_realized_pnl", accounting.grossRealizedPnl, "currency"),
    metric("unrealized_pnl", accounting.unrealizedPnl, "currency", "exact_fixed_point", null, markUnavailable),
    metric("net_fees", accounting.netFees, "currency"),
    metric("ending_position", accounting.positionShares, "shares"),
    metric("max_long_position", risk.maxLongPositionShares, "shares"),
    metric("max_short_position", risk.maxShortPositionShares, "shares"),
    metric("max_drawdown", risk.maxDrawdown, "currency"),
    metric("fill_count", orders.fillCount, "count"),
    metric("filled_shares", orders.filledShares, "shares"),
    metric("maker_fill_count", orders.makerFillCount, "count"),
    metric("taker_fill_count", orders.takerFillCount, "count"),
    metric("fill_rate", fillRateRatio, "ratio", "derived_float", null, "no shares submitted"),
    metric("submitted_order_count", orders.submittedOrderCount, "count"),
    metric("canceled_order_count", orders.canceledOrderCount, "count"),
    metric("cancel_race_lost_count", orders.cancelRaceLostCount, "count"),
    metric("spread_capture_total", formatMid(spreadCaptureTenthUnits), "currency"),
    ...markoutSummaries.map((summary) => metric("markout_total", summary.totalMarkout, "currency", "exact_fixed_point", summary.horizonNs)),
    metric("quote_exposure_time", orders.quoteExposureTimeNs, "nanoseconds"),
  ];
  return {
    schemaVersion: "exsim.run-summary/1",
    runId,
    availability,
    asOfEventPosition: String(cutoff.processedEventCount),
    asOfSimulationTimeNs: String(asOfTimeNs),
    accounting,
    orders,
    risk,
    spreadCaptureTotal: formatMid(spreadCaptureTenthUnits),
    markouts: markoutSummaries,
    latency: [
      summarizeLatency("feed", config.latency.feedLatencyNs, latencyByComponent.feed),
      summarizeLatency("order_entry", config.latency.orderEntryLatencyNs, latencyByComponent.order_entry),
      summarizeLatency("acknowledgment", config.latency.acknowledgmentLatencyNs, latencyByComponent.acknowledgment),
      summarizeLatency("cancel", config.latency.cancelLatencyNs, latencyByComponent.cancel),
    ],
    metrics,
  };
}

// ---------- series ----------

export interface ExactTimedValue {
  timeNs: bigint;
  value: string | null;
}

export const SERIES_UNIT_BY_KIND: Record<Schemas["SeriesKind"], Schemas["MetricUnit"]> = {
  equity: "currency",
  inventory: "shares",
  exchange_mid: "currency_per_share",
  quoted_spread: "currency_per_share",
  drawdown: "currency",
};

export function buildFullResolutionSeries(seriesKind: Schemas["SeriesKind"], artifacts: SimulatedRunArtifacts, cutoff: ArtifactCutoff): ExactTimedValue[] {
  if (seriesKind === "quoted_spread") {
    return artifacts.ownQuotePoints
      .filter((point) => isIncluded(point.eventPosition, cutoff))
      .map((point) => ({ timeNs: point.timeNs, value: point.quotedSpreadUnits === null ? null : formatPrice(point.quotedSpreadUnits) }));
  }
  if (seriesKind === "exchange_mid") {
    return artifacts.midPricePoints
      .filter((point) => isIncluded(point.eventPosition, cutoff))
      .map((point) => ({ timeNs: point.timeNs, value: point.midTenthUnits === null ? null : formatMid(point.midTenthUnits) }));
  }
  const samples = buildEquitySamples(artifacts, cutoff);
  if (seriesKind === "inventory") return samples.map((sample) => ({ timeNs: sample.timeNs, value: String(sample.positionShares) }));
  if (seriesKind === "equity") return samples.map((sample) => ({ timeNs: sample.timeNs, value: sample.equityTenthUnits === null ? null : formatMid(sample.equityTenthUnits) }));
  let peakEquity: bigint | null = null;
  return samples.map((sample) => {
    if (sample.equityTenthUnits === null) return { timeNs: sample.timeNs, value: null };
    if (peakEquity === null || sample.equityTenthUnits > peakEquity) peakEquity = sample.equityTenthUnits;
    return { timeNs: sample.timeNs, value: formatMid(peakEquity - sample.equityTenthUnits) };
  });
}

export function chooseBucketWidthNs(fromTimeNs: bigint, toTimeNs: bigint, maxPoints: number): bigint {
  const span = toTimeNs - fromTimeNs;
  const width = (span + BigInt(maxPoints) - 1n) / BigInt(maxPoints);
  return width > 0n ? width : 1n;
}

/** Last value at or before each bucket end; buckets end at from + k * width, the last capped at `to`. */
export function downsampleLastValueInBucket(samples: ExactTimedValue[], fromTimeNs: bigint, toTimeNs: bigint, bucketWidthNs: bigint): Schemas["SeriesPoint"][] {
  const points: Schemas["SeriesPoint"][] = [];
  let sampleIndex = 0;
  let lastValue: string | null = null;
  for (let bucketEnd = fromTimeNs + bucketWidthNs; ; bucketEnd += bucketWidthNs) {
    const cappedEnd = bucketEnd > toTimeNs ? toTimeNs : bucketEnd;
    while (sampleIndex < samples.length && samples[sampleIndex].timeNs <= cappedEnd) {
      lastValue = samples[sampleIndex].value;
      sampleIndex++;
    }
    points.push({ bucketEndTimeNs: String(cappedEnd), value: lastValue });
    if (cappedEnd >= toTimeNs) break;
  }
  return points;
}

export function buildRunSeries(runId: string, artifacts: SimulatedRunArtifacts, cutoff: ArtifactCutoff, availability: "final" | "partial", seriesKinds: Schemas["SeriesKind"][], fromTimeNs: bigint, toTimeNs: bigint, maxPoints: number): Schemas["RunSeriesResponse"] {
  const bucketWidthNs = chooseBucketWidthNs(fromTimeNs, toTimeNs, maxPoints);
  return {
    runId,
    availability,
    series: seriesKinds.map((seriesKind) => {
      const fullResolution = buildFullResolutionSeries(seriesKind, artifacts, cutoff);
      return {
        seriesKind,
        unit: SERIES_UNIT_BY_KIND[seriesKind],
        aggregation: "last_value_in_bucket",
        bucketWidthNs: String(bucketWidthNs),
        fromSimulationTimeNs: String(fromTimeNs),
        toSimulationTimeNs: String(toTimeNs),
        sourceSampleCount: String(fullResolution.filter((sample) => sample.timeNs >= fromTimeNs && sample.timeNs <= toTimeNs).length),
        points: downsampleLastValueInBucket(fullResolution, fromTimeNs, toTimeNs, bucketWidthNs),
      };
    }),
  };
}

// ---------- fills, orders, lifecycle, book ----------

export function buildFill(fill: OwnFillRecord, artifacts: SimulatedRunArtifacts, config: ExperimentConfig, cutoff: ArtifactCutoff): Schemas["Fill"] {
  const order = fill.ownOrder;
  const spreadCapture = spreadCapturePerShareTenthUnits(fill);
  return {
    matchId: String(fill.matchId),
    eventPosition: String(fill.eventPosition),
    simulationTimeNs: String(fill.timeNs),
    orderId: String(order.orderId),
    clientOrderId: String(order.clientOrderId),
    side: fill.side,
    price: formatPrice(fill.price),
    quantity: String(fill.quantity),
    liquidityRole: fill.liquidityRole,
    counterpartyActor: fill.counterpartyActor,
    feePerShare: formatPrice(fill.feePerShareUnits),
    midAtFill: fill.midBeforeFillTenthUnits === null ? null : formatMid(fill.midBeforeFillTenthUnits),
    spreadCapturePerShare: spreadCapture === null ? null : formatMid(spreadCapture),
    queueAheadAtRestShares: fill.liquidityRole === "maker" && order.queueAheadAtRestShares !== null ? String(order.queueAheadAtRestShares) : null,
    queueEvidenceSource: "exact_simulated_truth",
    relatedEvents: {
      strategySubmitEventPosition: String(order.submitEventPosition),
      exchangeArrivalEventPosition: String(order.arrivalEventPosition),
      acceptedEventPosition: String(order.acceptedEventPosition),
      acknowledgmentDeliveredEventPosition: isIncluded(order.acknowledgmentDeliveredEventPosition, cutoff) ? String(order.acknowledgmentDeliveredEventPosition) : null,
      fillEventPosition: String(fill.eventPosition),
      fillReportDeliveredEventPosition: isIncluded(fill.fillReportDeliveredEventPosition, cutoff) ? String(fill.fillReportDeliveredEventPosition) : null,
      aggressorArrivalEventPosition: fill.aggressorArrivalEventPosition === null ? null : String(fill.aggressorArrivalEventPosition),
    },
    ledgerEffect: {
      positionBeforeShares: String(fill.ledgerBefore.positionShares),
      positionAfterShares: String(fill.ledgerAfter.positionShares),
      cashBefore: formatPrice(fill.ledgerBefore.cashUnits),
      cashAfter: formatPrice(fill.ledgerAfter.cashUnits),
      fee: formatPrice(fill.feeUnits),
      realizedPnlChange: formatPrice(fill.realizedPnlChangeUnits),
      cumulativeRealizedPnlAfter: formatPrice(fill.ledgerAfter.cumulativeRealizedPnlUnits),
      cumulativeFeesAfter: formatPrice(fill.ledgerAfter.cumulativeFeesUnits),
    },
    markouts: computeFillMarkouts(fill, artifacts, config.markoutHorizonsNs, cutoff),
  };
}

export function buildOwnOrderSummary(order: OwnOrderRecord, cutoff: ArtifactCutoff): Schemas["OwnOrderSummary"] | null {
  if (!isIncluded(order.submitEventPosition, cutoff) || !isIncluded(order.acceptedEventPosition, cutoff) || order.orderId === null) return null;
  const includedStages = order.stages.filter((stage) => Number(stage.eventPosition) < cutoff.processedEventCount);
  const includedFills = order.fills.filter((fill) => isIncluded(fill.eventPosition, cutoff));
  const cumulativeQuantity = includedFills.reduce((total, fill) => total + fill.quantity, 0n);
  const wasCanceled = includedStages.some((stage) => stage.stageKind === "canceled");
  const finalStatus: Schemas["OrderFinalStatus"] = cumulativeQuantity === order.quantity
    ? "filled"
    : wasCanceled ? (cumulativeQuantity > 0n ? "partially_filled_then_canceled" : "canceled") : "open_at_run_end";
  return {
    orderId: String(order.orderId),
    clientOrderId: String(order.clientOrderId),
    side: order.side,
    price: formatPrice(order.price),
    quantity: String(order.quantity),
    cumulativeQuantity: String(cumulativeQuantity),
    finalStatus,
    submitEventPosition: String(order.submitEventPosition),
    submitSimulationTimeNs: String(order.submitTimeNs),
    lastEventPosition: includedStages.at(-1)?.eventPosition ?? String(order.submitEventPosition),
    fillCount: includedFills.length,
  };
}

export function buildOrderLifecycle(runId: string, order: OwnOrderRecord, cutoff: ArtifactCutoff): Schemas["OrderLifecycle"] | null {
  const summary = buildOwnOrderSummary(order, cutoff);
  if (!summary) return null;
  const stages = order.stages
    .filter((stage) => Number(stage.eventPosition) < cutoff.processedEventCount)
    .sort((left, right) => Number(left.eventPosition) - Number(right.eventPosition));
  const cancelSubmit = stages.find((stage) => stage.stageKind === "cancel_submitted");
  const cancelArrival = stages.find((stage) => stage.stageKind === "cancel_arrived_at_exchange");
  const firstFill = order.fills.find((fill) => isIncluded(fill.eventPosition, cutoff));
  return {
    runId,
    order: summary,
    matchingRule: "price_time_fifo",
    stages,
    fifoAheadEvidence: order.fifoAheadEvidence.filter((evidence) => Number(evidence.eventPosition) < cutoff.processedEventCount),
    latency: {
      orderEntryNs: order.arrivalTimeNs === null ? null : String(order.arrivalTimeNs - order.submitTimeNs),
      acknowledgmentNs: isIncluded(order.acknowledgmentDeliveredEventPosition, cutoff) && order.acknowledgmentDeliveredTimeNs !== null && order.acceptedTimeNs !== null
        ? String(order.acknowledgmentDeliveredTimeNs - order.acceptedTimeNs)
        : null,
      cancelNs: cancelSubmit && cancelArrival ? String(BigInt(cancelArrival.simulationTimeNs) - BigInt(cancelSubmit.simulationTimeNs)) : null,
      submitToFirstFillNs: firstFill ? String(firstFill.timeNs - order.submitTimeNs) : null,
    },
    fillMatchIds: order.fills.filter((fill) => isIncluded(fill.eventPosition, cutoff)).map((fill) => String(fill.matchId)),
    relatedEventPositions: [...new Set(order.relatedEventPositions.filter((position) => position < cutoff.processedEventCount))]
      .sort((left, right) => left - right)
      .map(String),
  };
}

function findSnapshotAtOrBefore(artifacts: SimulatedRunArtifacts, eventPosition: number) {
  let found = null;
  for (const snapshot of artifacts.bookSnapshots) {
    if (snapshot.eventPosition > eventPosition) break;
    found = snapshot;
  }
  return found;
}

export function buildBookView(runId: string, artifacts: SimulatedRunArtifacts, eventPosition: number, view: Schemas["EventVisibility"], depth: number): Schemas["BookView"] {
  let snapshotPosition = eventPosition;
  let lastDeliveredSourceEventPosition: string | null = null;
  if (view === "strategy_visible") {
    snapshotPosition = -1;
    for (let position = eventPosition; position >= 0; position--) {
      const event = artifacts.events[position];
      if (event.eventKind === "market_data_delivered" && event.sourceEventPosition !== null) {
        snapshotPosition = Number(event.sourceEventPosition);
        lastDeliveredSourceEventPosition = event.sourceEventPosition;
        break;
      }
    }
  }
  const snapshot = snapshotPosition < 0 ? null : findSnapshotAtOrBefore(artifacts, snapshotPosition);
  const formatLevels = (levels: NonNullable<typeof snapshot>["bids"]) =>
    levels.slice(0, depth).map((level) => ({ price: formatPrice(level.price), totalShares: String(level.totalShares), orderCount: String(level.orderCount), ownShares: String(level.ownShares) }));
  return {
    runId,
    eventPosition: String(eventPosition),
    simulationTimeNs: artifacts.events[eventPosition].simulationTimeNs,
    view,
    source: view === "exchange_truth" ? "exact_simulated_truth" : "estimated_from_market_data",
    bids: snapshot ? formatLevels(snapshot.bids) : [],
    asks: snapshot ? formatLevels(snapshot.asks) : [],
    lastDeliveredSourceEventPosition,
  };
}
