// Mock-only fixture generator. It produces internally consistent artifacts
// (canonical events, own-order lifecycles, fills, ledger effects, book
// snapshots) for the mock API so the UI can be built before the C++ research
// runner exists. It is NOT part of the product: the real results come from the
// C++ runner, and the Next.js app never imports this module.
//
// Model: one instrument, a background market maker quoting two levels around a
// random-walk fair value, partially informed noise takers sending market
// orders, and the fixed-spread research strategy, all trading in one
// price-time FIFO book. Strategy information arrives after the declared feed
// and acknowledgment latencies; its orders and cancels arrive after the
// declared order-entry and cancel latencies.

import type { components } from "../../src/lib/api/generated-schema";
import { ceilDivide, floorDivide, parseScaledDecimal, PRICE_SCALE_DIGITS } from "./fixed-point-decimal";
import { deriveStreamSeed, SeededRandomStream } from "./seeded-random-streams";

type Schemas = components["schemas"];
export type ExperimentConfig = Schemas["ExperimentConfig"];
export type CanonicalEvent = Schemas["CanonicalEvent"];
export type Side = Schemas["Side"];
type Actor = Schemas["Actor"];
type PipelineStage = Schemas["PipelineStage"];
type LifecycleStage = Schemas["LifecycleStage"];
type FifoAheadEvidence = Schemas["FifoAheadEvidence"];

export const PIPELINE_STAGE_ORDER_FOR_EQUAL_TIME: PipelineStage[] = [
  "exchange_processing",
  "feed_delivery",
  "report_delivery",
  "strategy_processing",
  "outbound_order",
  "runner_control",
];

interface BookOrder {
  orderId: bigint;
  clientOrderId: bigint;
  actor: Actor;
  side: Side;
  price: bigint;
  leavesQuantity: bigint;
  cumulativeQuantity: bigint;
  ownOrderRecord: OwnOrderRecord | null;
}

export interface OwnOrderRecord {
  clientOrderId: bigint;
  orderId: bigint | null;
  side: Side;
  price: bigint;
  quantity: bigint;
  submitEventPosition: number;
  submitTimeNs: bigint;
  arrivalEventPosition: number | null;
  arrivalTimeNs: bigint | null;
  acceptedEventPosition: number | null;
  acceptedTimeNs: bigint | null;
  acknowledgmentDeliveredEventPosition: number | null;
  acknowledgmentDeliveredTimeNs: bigint | null;
  restedTimeNs: bigint | null;
  restedEventPosition: number | null;
  removedFromBookTimeNs: bigint | null;
  removedFromBookEventPosition: number | null;
  cancelSubmitTimeNs: bigint | null;
  cancelArrivalTimeNs: bigint | null;
  firstFillTimeNs: bigint | null;
  stages: LifecycleStage[];
  fifoAheadEvidence: FifoAheadEvidence[];
  relatedEventPositions: number[];
  fills: OwnFillRecord[];
  queueAheadAtRestShares: bigint | null;
}

export interface LedgerSnapshot {
  positionShares: bigint;
  cashUnits: bigint;
  cumulativeRealizedPnlUnits: bigint;
  cumulativeFeesUnits: bigint;
}

export interface OwnFillRecord {
  matchId: bigint;
  eventPosition: number;
  timeNs: bigint;
  ownOrder: OwnOrderRecord;
  side: Side;
  price: bigint;
  quantity: bigint;
  liquidityRole: Schemas["LiquidityRole"];
  counterpartyActor: Actor;
  feePerShareUnits: bigint;
  feeUnits: bigint;
  realizedPnlChangeUnits: bigint;
  midBeforeFillTenthUnits: bigint | null;
  aggressorArrivalEventPosition: number | null;
  ledgerBefore: LedgerSnapshot;
  ledgerAfter: LedgerSnapshot;
  fillReportDeliveredEventPosition: number | null;
}

export interface MidPricePoint {
  eventPosition: number;
  timeNs: bigint;
  /** (best bid + best ask) * 5, i.e. the mid in units of 0.00001; null without a two-sided book. */
  midTenthUnits: bigint | null;
}

export interface OwnQuotePoint {
  eventPosition: number;
  timeNs: bigint;
  /** Own best ask minus own best bid in price units; null unless both sides rest. */
  quotedSpreadUnits: bigint | null;
}

export interface BookLevelSnapshot {
  price: bigint;
  totalShares: bigint;
  orderCount: bigint;
  ownShares: bigint;
}

export interface BookSnapshot {
  eventPosition: number;
  timeNs: bigint;
  bids: BookLevelSnapshot[];
  asks: BookLevelSnapshot[];
}

export interface SimulatedRunArtifacts {
  masterSeed: bigint;
  derivedSeeds: { streamName: Schemas["DerivedSeed"]["streamName"]; seed: bigint }[];
  endTimeNs: bigint;
  initialCashUnits: bigint;
  tickSizeUnits: bigint;
  events: CanonicalEvent[];
  ownOrdersInSubmitOrder: OwnOrderRecord[];
  ownFills: OwnFillRecord[];
  midPricePoints: MidPricePoint[];
  ownQuotePoints: OwnQuotePoint[];
  bookSnapshots: BookSnapshot[];
  blockedOrderEventPositions: number[];
  cancelRaceLostEventPositions: number[];
  latencySamples: Record<Schemas["LatencyMeasurement"]["component"], bigint[]>;
  exceededEventBound: boolean;
}

interface ScheduledAction {
  timeNs: bigint;
  stageRank: number;
  enqueueSequence: bigint;
  stage: PipelineStage;
  perform: () => void;
}

class ScheduledActionQueue {
  private readonly heap: ScheduledAction[] = [];

  private static isEarlier(left: ScheduledAction, right: ScheduledAction): boolean {
    if (left.timeNs !== right.timeNs) return left.timeNs < right.timeNs;
    if (left.stageRank !== right.stageRank) return left.stageRank < right.stageRank;
    return left.enqueueSequence < right.enqueueSequence;
  }

  get size(): number {
    return this.heap.length;
  }

  push(action: ScheduledAction): void {
    this.heap.push(action);
    let childIndex = this.heap.length - 1;
    while (childIndex > 0) {
      const parentIndex = (childIndex - 1) >> 1;
      if (!ScheduledActionQueue.isEarlier(this.heap[childIndex], this.heap[parentIndex])) break;
      [this.heap[childIndex], this.heap[parentIndex]] = [this.heap[parentIndex], this.heap[childIndex]];
      childIndex = parentIndex;
    }
  }

  pop(): ScheduledAction | undefined {
    const earliest = this.heap[0];
    const last = this.heap.pop();
    if (this.heap.length > 0 && last) {
      this.heap[0] = last;
      let parentIndex = 0;
      for (;;) {
        const leftIndex = parentIndex * 2 + 1;
        const rightIndex = leftIndex + 1;
        let earliestIndex = parentIndex;
        if (leftIndex < this.heap.length && ScheduledActionQueue.isEarlier(this.heap[leftIndex], this.heap[earliestIndex])) earliestIndex = leftIndex;
        if (rightIndex < this.heap.length && ScheduledActionQueue.isEarlier(this.heap[rightIndex], this.heap[earliestIndex])) earliestIndex = rightIndex;
        if (earliestIndex === parentIndex) break;
        [this.heap[parentIndex], this.heap[earliestIndex]] = [this.heap[earliestIndex], this.heap[parentIndex]];
        parentIndex = earliestIndex;
      }
    }
    return earliest;
  }
}

class PriceTimeFifoBook {
  readonly bidQueuesByPrice = new Map<bigint, BookOrder[]>();
  readonly askQueuesByPrice = new Map<bigint, BookOrder[]>();

  queuesFor(side: Side): Map<bigint, BookOrder[]> {
    return side === "buy" ? this.bidQueuesByPrice : this.askQueuesByPrice;
  }

  bestPriceExcludingOwnOrders(side: Side): bigint | null {
    let best: bigint | null = null;
    for (const [price, queue] of this.queuesFor(side)) {
      if (!queue.some((queuedOrder) => queuedOrder.ownOrderRecord === null)) continue;
      if (best === null || (side === "buy" ? price > best : price < best)) best = price;
    }
    return best;
  }

  bestPrice(side: Side): bigint | null {
    let best: bigint | null = null;
    for (const price of this.queuesFor(side).keys()) {
      if (best === null || (side === "buy" ? price > best : price < best)) best = price;
    }
    return best;
  }

  append(order: BookOrder): void {
    const queues = this.queuesFor(order.side);
    const queue = queues.get(order.price);
    if (queue) queue.push(order);
    else queues.set(order.price, [order]);
  }

  remove(order: BookOrder): void {
    const queues = this.queuesFor(order.side);
    const queue = queues.get(order.price);
    if (!queue) return;
    const index = queue.indexOf(order);
    if (index >= 0) queue.splice(index, 1);
    if (queue.length === 0) queues.delete(order.price);
  }

  sharesAndOrdersAhead(order: BookOrder): { sharesAhead: bigint; ordersAhead: bigint; levelTotalShares: bigint } {
    const queue = this.queuesFor(order.side).get(order.price) ?? [];
    let sharesAhead = 0n;
    let ordersAhead = 0n;
    let levelTotalShares = 0n;
    let reachedOrder = false;
    for (const queuedOrder of queue) {
      if (queuedOrder === order) reachedOrder = true;
      else if (!reachedOrder) {
        sharesAhead += queuedOrder.leavesQuantity;
        ordersAhead += 1n;
      }
      levelTotalShares += queuedOrder.leavesQuantity;
    }
    return { sharesAhead, ordersAhead, levelTotalShares };
  }

  snapshotLevels(side: Side, depth: number): BookLevelSnapshot[] {
    const prices = [...this.queuesFor(side).keys()].sort((left, right) =>
      side === "buy" ? (left > right ? -1 : 1) : left < right ? -1 : 1,
    );
    return prices.slice(0, depth).map((price) => {
      const queue = this.queuesFor(side).get(price) ?? [];
      let totalShares = 0n;
      let ownShares = 0n;
      for (const queuedOrder of queue) {
        totalShares += queuedOrder.leavesQuantity;
        if (queuedOrder.ownOrderRecord) ownShares += queuedOrder.leavesQuantity;
      }
      return { price, totalShares, orderCount: BigInt(queue.length), ownShares };
    });
  }
}

function findModelParameter(config: ExperimentConfig, agentKind: Schemas["BackgroundAgentKind"], parameterName: string, defaultValue: string): string {
  const agent = config.scenario.backgroundAgents.find((candidate) => candidate.agentKind === agentKind);
  return agent?.parameters.find((parameter) => parameter.name === parameterName)?.value ?? defaultValue;
}

function createEmptyCanonicalEvent(): Omit<CanonicalEvent, "eventPosition" | "simulationTimeNs" | "enqueueSequence" | "stage" | "eventKind" | "visibility" | "actor" | "description"> {
  return {
    isOwnOrder: false,
    orderId: null,
    clientOrderId: null,
    side: null,
    price: null,
    quantity: null,
    leavesQuantity: null,
    cumulativeQuantity: null,
    matchId: null,
    liquidityRole: null,
    rejectReason: null,
    rejectedCommand: null,
    cancelReason: null,
    reportKind: null,
    bestBidPrice: null,
    bestAskPrice: null,
    sourceEventPosition: null,
    newPhase: null,
  };
}

export function simulateSyntheticMarketRun(config: ExperimentConfig, masterSeedText: string): SimulatedRunArtifacts {
  if (config.strategy.strategyKind !== "fixed_spread_market_maker") {
    throw new Error("the mock fixture generator only executes fixed_spread_market_maker");
  }
  const formatPrice = (priceUnits: bigint): string => {
    const text = priceUnits.toString().padStart(PRICE_SCALE_DIGITS + 1, "0");
    return `${text.slice(0, -PRICE_SCALE_DIGITS)}.${text.slice(-PRICE_SCALE_DIGITS)}`;
  };

  const masterSeed = BigInt(masterSeedText);
  const marketMakerRandom = new SeededRandomStream(deriveStreamSeed(masterSeed, "background_market_maker"));
  const noiseTakerRandom = new SeededRandomStream(deriveStreamSeed(masterSeed, "background_noise_taker"));
  const latencyJitterRandom = new SeededRandomStream(deriveStreamSeed(masterSeed, "latency_jitter"));

  const tickSizeUnits = parseScaledDecimal(config.instrument.tickSize, PRICE_SCALE_DIGITS);
  const referencePriceUnits = parseScaledDecimal(config.instrument.referencePrice, PRICE_SCALE_DIGITS);
  const endTimeNs = BigInt(config.bounds.endSimulationTimeNs);
  const maxCanonicalEventCount = Number(config.bounds.maxCanonicalEventCount);
  // The mock approximates the runner's timer-driven requoting: it requotes on every
  // strategy-visible mid change, at most once per requoteIntervalNs. Its fixtures do not
  // depend on requoteThresholdTicks or postOnly.
  const quoteWidthTicks = 2n * BigInt(config.strategy.halfSpreadTicks);
  const quoteQuantity = BigInt(config.strategy.quoteQuantity);
  const requoteOnVisibleMidChange = true;
  const minimumRequoteIntervalNs = BigInt(config.strategy.requoteIntervalNs);
  const makerFeePerShareUnits = parseScaledDecimal(config.fees.makerFeePerShare, PRICE_SCALE_DIGITS);
  const takerFeePerShareUnits = parseScaledDecimal(config.fees.takerFeePerShare, PRICE_SCALE_DIGITS);
  const maxAbsolutePositionShares = BigInt(config.risk.maxAbsolutePositionShares);
  const maxOpenOrders = BigInt(config.risk.maxOpenOrders);
  const initialCashUnits = parseScaledDecimal(config.initialCash, PRICE_SCALE_DIGITS);

  const fairValueStepIntervalNs = BigInt(findModelParameter(config, "market_maker", "fairValueStepIntervalNs", "500000000"));
  const bestLevelShares = BigInt(findModelParameter(config, "market_maker", "bestLevelShares", "300"));
  const deeperLevelShares = BigInt(findModelParameter(config, "market_maker", "deeperLevelShares", "500"));
  const fairValueMoveProbability = Number(findModelParameter(config, "market_maker", "fairValueMoveProbability", "0.5"));
  const takerMeanInterarrivalNs = BigInt(findModelParameter(config, "noise_taker", "meanInterarrivalNs", "350000000"));
  const takerMinimumLots = Number(findModelParameter(config, "noise_taker", "minimumOrderLots", "1"));
  const takerMaximumLots = Number(findModelParameter(config, "noise_taker", "maximumOrderLots", "6"));
  const takerLotShares = BigInt(findModelParameter(config, "noise_taker", "sharesPerLot", "100"));
  const informedTakerProbability = Number(findModelParameter(config, "noise_taker", "informedProbability", "0.6"));

  const drawLatency = (configuredNs: string): bigint => {
    const base = BigInt(configuredNs);
    if (config.latency.latencyModel !== "uniform_jitter") return base;
    const jitter = BigInt(config.latency.jitterNs);
    const offset = BigInt(latencyJitterRandom.nextIntegerInclusive(0, Number(jitter * 2n))) - jitter;
    return base + offset > 0n ? base + offset : 0n;
  };

  // Pre-draw the fair value path so takers can be partially informed about the next move.
  const fairValueStepCount = Number(endTimeNs / fairValueStepIntervalNs);
  const fairValueMovesInTicks: bigint[] = [];
  for (let stepIndex = 0; stepIndex <= fairValueStepCount; stepIndex++) {
    const moves = marketMakerRandom.nextUniform() < fairValueMoveProbability;
    fairValueMovesInTicks.push(moves ? (marketMakerRandom.nextUniform() < 0.5 ? -1n : 1n) : 0n);
  }

  const events: CanonicalEvent[] = [];
  const book = new PriceTimeFifoBook();
  const queue = new ScheduledActionQueue();
  let nextEnqueueSequence = 0n;
  let currentEnqueueSequence = 0n;
  let currentStage: PipelineStage = "runner_control";
  let nextOrderId = 1n;
  let nextMatchId = 1n;
  let nextBackgroundClientOrderId = 1n;
  let nextStrategyClientOrderId = 1n;
  let exceededEventBound = false;
  let simulationFinished = false;

  const ownOrdersInSubmitOrder: OwnOrderRecord[] = [];
  const ownFills: OwnFillRecord[] = [];
  const midPricePoints: MidPricePoint[] = [];
  const ownQuotePoints: OwnQuotePoint[] = [];
  const bookSnapshots: BookSnapshot[] = [];
  const blockedOrderEventPositions: number[] = [];
  const cancelRaceLostEventPositions: number[] = [];
  const latencySamples: SimulatedRunArtifacts["latencySamples"] = { feed: [], order_entry: [], acknowledgment: [], cancel: [] };
  const liveOwnBookOrdersByClientOrderId = new Map<bigint, BookOrder>();
  const marketMakerOrdersByPrice = new Map<bigint, BookOrder[]>();

  const ledger: LedgerSnapshot = { positionShares: 0n, cashUnits: initialCashUnits, cumulativeRealizedPnlUnits: 0n, cumulativeFeesUnits: 0n };
  const openLots: { side: Side; priceUnits: bigint; shares: bigint }[] = [];

  const schedule = (timeNs: bigint, stage: PipelineStage, perform: () => void): void => {
    queue.push({ timeNs, stage, stageRank: PIPELINE_STAGE_ORDER_FOR_EQUAL_TIME.indexOf(stage), enqueueSequence: nextEnqueueSequence++, perform });
  };

  let currentTimeNs = 0n;
  const appendEvent = (
    fields: Partial<CanonicalEvent> & Pick<CanonicalEvent, "eventKind" | "visibility" | "actor" | "description">,
  ): number => {
    const eventPosition = events.length;
    events.push({
      ...createEmptyCanonicalEvent(),
      eventPosition: String(eventPosition),
      simulationTimeNs: String(currentTimeNs),
      enqueueSequence: String(currentEnqueueSequence),
      stage: currentStage,
      ...fields,
    });
    if (events.length >= maxCanonicalEventCount) exceededEventBound = true;
    return eventPosition;
  };

  const exchangeMidTenthUnits = (): bigint | null => {
    const bestBid = book.bestPrice("buy");
    const bestAsk = book.bestPrice("sell");
    return bestBid === null || bestAsk === null ? null : (bestBid + bestAsk) * 5n;
  };

  // ---------- ledger (FIFO lots, same identities as exsim::research::PositionLedger) ----------
  const recordOwnFillInLedger = (side: Side, priceUnits: bigint, shares: bigint, feePerShareUnits: bigint): { before: LedgerSnapshot; after: LedgerSnapshot; feeUnits: bigint; realizedChangeUnits: bigint } => {
    const before = { ...ledger };
    const feeUnits = feePerShareUnits * shares;
    let remainingShares = shares;
    let realizedChangeUnits = 0n;
    while (remainingShares > 0n && openLots.length > 0 && openLots[0].side !== side) {
      const lot = openLots[0];
      const closedShares = remainingShares < lot.shares ? remainingShares : lot.shares;
      realizedChangeUnits += (side === "buy" ? lot.priceUnits - priceUnits : priceUnits - lot.priceUnits) * closedShares;
      lot.shares -= closedShares;
      remainingShares -= closedShares;
      if (lot.shares === 0n) openLots.shift();
    }
    if (remainingShares > 0n) openLots.push({ side, priceUnits, shares: remainingShares });
    ledger.positionShares += side === "buy" ? shares : -shares;
    ledger.cashUnits += (side === "buy" ? -priceUnits : priceUnits) * shares - feeUnits;
    ledger.cumulativeFeesUnits += feeUnits;
    ledger.cumulativeRealizedPnlUnits += realizedChangeUnits;
    return { before, after: { ...ledger }, feeUnits, realizedChangeUnits };
  };

  // ---------- strategy state (only what has been delivered to it) ----------
  interface StrategyWorkingOrder {
    record: OwnOrderRecord;
    side: Side;
    price: bigint;
    believedLeaves: bigint;
  }
  const strategyWorkingOrders = new Map<bigint, StrategyWorkingOrder>();
  let strategyVisibleBestBid: bigint | null = null;
  let strategyVisibleBestAsk: bigint | null = null;
  let strategyVisibleMidTimesTwo: bigint | null = null;
  let strategyBelievedPositionShares = 0n;
  let strategyLastRequoteTimeNs: bigint | null = null;
  let strategyReevaluationScheduled = false;

  const scheduleExecutionReport = (record: OwnOrderRecord, sourceEventPosition: number, reportKind: Schemas["ExecutionReportKind"], onDelivered: (deliveredEventPosition: number) => void, fields: Partial<CanonicalEvent>): void => {
    const acknowledgmentLatencyNs = drawLatency(config.latency.acknowledgmentLatencyNs);
    latencySamples.acknowledgment.push(acknowledgmentLatencyNs);
    schedule(currentTimeNs + acknowledgmentLatencyNs, "report_delivery", () => {
      const deliveredEventPosition = appendEvent({
        ...fields,
        eventKind: "execution_report_delivered",
        visibility: "strategy_visible",
        actor: "research_strategy",
        isOwnOrder: true,
        orderId: record.orderId === null ? null : String(record.orderId),
        clientOrderId: String(record.clientOrderId),
        side: record.side,
        reportKind,
        sourceEventPosition: String(sourceEventPosition),
        description: `report ${reportKind} delivered for own order ${record.clientOrderId}`,
      });
      record.relatedEventPositions.push(deliveredEventPosition);
      onDelivered(deliveredEventPosition);
    });
  };

  const recordFifoEvidence = (order: BookOrder, eventPosition: number, reason: Schemas["FifoEvidenceReason"]): void => {
    const record = order.ownOrderRecord;
    if (!record) return;
    const { sharesAhead, ordersAhead, levelTotalShares } = book.sharesAndOrdersAhead(order);
    record.fifoAheadEvidence.push({
      eventPosition: String(eventPosition),
      simulationTimeNs: String(currentTimeNs),
      reason,
      source: "exact_simulated_truth",
      price: formatPrice(order.price),
      sharesAhead: String(sharesAhead),
      ordersAhead: String(ordersAhead),
      levelTotalShares: String(levelTotalShares),
    });
  };

  const lastRecordedSharesAhead = new Map<BookOrder, bigint>();
  const refreshOwnQueueEvidence = (eventPosition: number, reason: Schemas["FifoEvidenceReason"]): void => {
    for (const order of liveOwnBookOrdersByClientOrderId.values()) {
      const { sharesAhead } = book.sharesAndOrdersAhead(order);
      if (lastRecordedSharesAhead.get(order) !== sharesAhead) {
        recordFifoEvidence(order, eventPosition, reason);
        lastRecordedSharesAhead.set(order, sharesAhead);
      }
    }
  };

  let lastPublishedBestBid: bigint | null = null;
  let lastPublishedBestAsk: bigint | null = null;
  let lastRecordedMid: bigint | null | undefined;
  let lastRecordedQuotedSpread: bigint | null | undefined;

  const removeFromBook = (order: BookOrder, eventPosition: number): void => {
    book.remove(order);
    if (order.ownOrderRecord) {
      liveOwnBookOrdersByClientOrderId.delete(order.clientOrderId);
      lastRecordedSharesAhead.delete(order);
      order.ownOrderRecord.removedFromBookTimeNs = currentTimeNs;
      order.ownOrderRecord.removedFromBookEventPosition = eventPosition;
    }
    if (order.actor === "background_market_maker") {
      const remainingAtPrice = (marketMakerOrdersByPrice.get(order.price) ?? []).filter((candidate) => candidate !== order);
      if (remainingAtPrice.length === 0) marketMakerOrdersByPrice.delete(order.price);
      else marketMakerOrdersByPrice.set(order.price, remainingAtPrice);
    }
  };

  const finishExchangeCommand = (queueChangeReason: Schemas["FifoEvidenceReason"]): void => {
    if (events.length === 0) return;
    const lastEventPosition = events.length - 1;
    refreshOwnQueueEvidence(lastEventPosition, queueChangeReason);
    const midTenthUnits = exchangeMidTenthUnits();
    if (midTenthUnits !== lastRecordedMid) {
      midPricePoints.push({ eventPosition: lastEventPosition, timeNs: currentTimeNs, midTenthUnits });
      lastRecordedMid = midTenthUnits;
    }
    let ownBestBid: bigint | null = null;
    let ownBestAsk: bigint | null = null;
    for (const order of liveOwnBookOrdersByClientOrderId.values()) {
      if (order.side === "buy" && (ownBestBid === null || order.price > ownBestBid)) ownBestBid = order.price;
      if (order.side === "sell" && (ownBestAsk === null || order.price < ownBestAsk)) ownBestAsk = order.price;
    }
    const quotedSpreadUnits = ownBestBid !== null && ownBestAsk !== null ? ownBestAsk - ownBestBid : null;
    if (quotedSpreadUnits !== lastRecordedQuotedSpread) {
      ownQuotePoints.push({ eventPosition: lastEventPosition, timeNs: currentTimeNs, quotedSpreadUnits });
      lastRecordedQuotedSpread = quotedSpreadUnits;
    }
    bookSnapshots.push({ eventPosition: lastEventPosition, timeNs: currentTimeNs, bids: book.snapshotLevels("buy", 10), asks: book.snapshotLevels("sell", 10) });

    const bestBid = book.bestPriceExcludingOwnOrders("buy");
    const bestAsk = book.bestPriceExcludingOwnOrders("sell");
    if (bestBid !== lastPublishedBestBid || bestAsk !== lastPublishedBestAsk) {
      lastPublishedBestBid = bestBid;
      lastPublishedBestAsk = bestAsk;
      const feedLatencyNs = drawLatency(config.latency.feedLatencyNs);
      latencySamples.feed.push(feedLatencyNs);
      schedule(currentTimeNs + feedLatencyNs, "feed_delivery", () => {
        appendEvent({
          eventKind: "market_data_delivered",
          visibility: "strategy_visible",
          actor: "research_strategy",
          bestBidPrice: bestBid === null ? null : formatPrice(bestBid),
          bestAskPrice: bestAsk === null ? null : formatPrice(bestAsk),
          sourceEventPosition: String(lastEventPosition),
          description: `other participants' top of book delivered: ${bestBid === null ? "-" : formatPrice(bestBid)} / ${bestAsk === null ? "-" : formatPrice(bestAsk)}`,
        });
        strategyVisibleBestBid = bestBid;
        strategyVisibleBestAsk = bestAsk;
        const visibleMidTimesTwo = bestBid !== null && bestAsk !== null ? bestBid + bestAsk : null;
        const visibleMidChanged = visibleMidTimesTwo !== strategyVisibleMidTimesTwo;
        strategyVisibleMidTimesTwo = visibleMidTimesTwo;
        if (visibleMidChanged && requoteOnVisibleMidChange) evaluateStrategyQuotes();
        else if (strategyWorkingOrders.size === 0) evaluateStrategyQuotes();
      });
    }
  };

  // ---------- exchange: matching ----------
  const processIncomingOrderAtExchange = (incoming: {
    actor: Actor;
    side: Side;
    limitPrice: bigint | null;
    quantity: bigint;
    clientOrderId: bigint;
    ownOrderRecord: OwnOrderRecord | null;
    arrivalEventPosition: number | null;
  }): void => {
    const orderId = nextOrderId++;
    const ownRecord = incoming.ownOrderRecord;
    const acceptedEventPosition = appendEvent({
      eventKind: "order_accepted",
      visibility: "exchange_truth",
      actor: incoming.actor,
      isOwnOrder: ownRecord !== null,
      orderId: String(orderId),
      clientOrderId: String(incoming.clientOrderId),
      side: incoming.side,
      price: incoming.limitPrice === null ? null : formatPrice(incoming.limitPrice),
      quantity: String(incoming.quantity),
      leavesQuantity: String(incoming.quantity),
      cumulativeQuantity: "0",
      description: `${incoming.actor} ${incoming.side} ${incoming.quantity} ${incoming.limitPrice === null ? "market" : `@ ${formatPrice(incoming.limitPrice)}`} accepted as order ${orderId}`,
    });
    const aggressor: BookOrder = {
      orderId,
      clientOrderId: incoming.clientOrderId,
      actor: incoming.actor,
      side: incoming.side,
      price: incoming.limitPrice ?? 0n,
      leavesQuantity: incoming.quantity,
      cumulativeQuantity: 0n,
      ownOrderRecord: ownRecord,
    };
    if (ownRecord) {
      ownRecord.orderId = orderId;
      ownRecord.acceptedEventPosition = acceptedEventPosition;
      ownRecord.acceptedTimeNs = currentTimeNs;
      ownRecord.relatedEventPositions.push(acceptedEventPosition);
      ownRecord.stages.push({ stageKind: "accepted", eventPosition: String(acceptedEventPosition), simulationTimeNs: String(currentTimeNs), visibility: "exchange_truth", quantity: String(incoming.quantity), leavesQuantity: String(incoming.quantity), matchId: null, detail: `exchange assigned order id ${orderId}` });
      scheduleExecutionReport(ownRecord, acceptedEventPosition, "accepted", (deliveredEventPosition) => {
        ownRecord.acknowledgmentDeliveredEventPosition = deliveredEventPosition;
        ownRecord.acknowledgmentDeliveredTimeNs = currentTimeNs;
        ownRecord.stages.push({ stageKind: "acknowledgment_delivered", eventPosition: String(deliveredEventPosition), simulationTimeNs: String(currentTimeNs), visibility: "strategy_visible", quantity: null, leavesQuantity: null, matchId: null, detail: "acceptance report delivered to the strategy" });
      }, {});
    }

    const oppositeSide: Side = incoming.side === "buy" ? "sell" : "buy";
    let queueChangeReason: Schemas["FifoEvidenceReason"] = "queue_advanced_by_fill";
    for (;;) {
      if (aggressor.leavesQuantity === 0n) break;
      const oppositeBest = book.bestPrice(oppositeSide);
      if (oppositeBest === null) break;
      const crosses = incoming.limitPrice === null || (incoming.side === "buy" ? incoming.limitPrice >= oppositeBest : incoming.limitPrice <= oppositeBest);
      if (!crosses) break;
      const resting = book.queuesFor(oppositeSide).get(oppositeBest)![0];
      if (ownRecord && resting.ownOrderRecord) {
        // Self-trade prevention, CancelResting mode.
        const canceledEventPosition = appendEvent({
          eventKind: "order_canceled",
          visibility: "exchange_truth",
          actor: "exchange",
          isOwnOrder: true,
          orderId: String(resting.orderId),
          clientOrderId: String(resting.clientOrderId),
          side: resting.side,
          price: formatPrice(resting.price),
          quantity: String(resting.leavesQuantity),
          leavesQuantity: "0",
          cumulativeQuantity: String(resting.cumulativeQuantity),
          cancelReason: "self_trade_prevention",
          description: `own order ${resting.orderId} canceled by self-trade prevention`,
        });
        markOwnOrderCanceledAtExchange(resting, canceledEventPosition, "self_trade_prevention");
        removeFromBook(resting, canceledEventPosition);
        queueChangeReason = "queue_advanced_by_cancel";
        continue;
      }
      if (resting.ownOrderRecord) recordFifoEvidence(resting, events.length, "immediately_before_fill");
      const midBeforeFill = exchangeMidTenthUnits();
      const executedShares = aggressor.leavesQuantity < resting.leavesQuantity ? aggressor.leavesQuantity : resting.leavesQuantity;
      aggressor.leavesQuantity -= executedShares;
      aggressor.cumulativeQuantity += executedShares;
      resting.leavesQuantity -= executedShares;
      resting.cumulativeQuantity += executedShares;
      const matchId = nextMatchId++;
      const ownLeg = ownRecord ? aggressor : resting.ownOrderRecord ? resting : null;
      const reportedLeg = ownLeg ?? resting;
      const fillEventPosition = appendEvent({
        eventKind: "order_filled",
        visibility: "exchange_truth",
        actor: "exchange",
        isOwnOrder: ownLeg !== null,
        orderId: String(reportedLeg.orderId),
        clientOrderId: String(reportedLeg.clientOrderId),
        side: reportedLeg.side,
        price: formatPrice(resting.price),
        quantity: String(executedShares),
        leavesQuantity: String(reportedLeg.leavesQuantity),
        cumulativeQuantity: String(reportedLeg.cumulativeQuantity),
        matchId: String(matchId),
        liquidityRole: ownLeg === null ? null : ownLeg === aggressor ? "taker" : "maker",
        description: `match ${matchId}: ${executedShares} @ ${formatPrice(resting.price)} (${aggressor.actor} ${aggressor.side} vs ${resting.actor})`,
      });
      if (ownLeg && ownLeg.ownOrderRecord) {
        const record = ownLeg.ownOrderRecord;
        const liquidityRole = ownLeg === aggressor ? "taker" : "maker";
        const feePerShareUnits = liquidityRole === "maker" ? makerFeePerShareUnits : takerFeePerShareUnits;
        const ledgerChange = recordOwnFillInLedger(ownLeg.side, resting.price, executedShares, feePerShareUnits);
        const fillRecord: OwnFillRecord = {
          matchId,
          eventPosition: fillEventPosition,
          timeNs: currentTimeNs,
          ownOrder: record,
          side: ownLeg.side,
          price: resting.price,
          quantity: executedShares,
          liquidityRole,
          counterpartyActor: ownLeg === aggressor ? resting.actor : aggressor.actor,
          feePerShareUnits,
          feeUnits: ledgerChange.feeUnits,
          realizedPnlChangeUnits: ledgerChange.realizedChangeUnits,
          midBeforeFillTenthUnits: midBeforeFill,
          aggressorArrivalEventPosition: ownLeg === aggressor ? null : incoming.arrivalEventPosition ?? acceptedEventPosition,
          ledgerBefore: ledgerChange.before,
          ledgerAfter: ledgerChange.after,
          fillReportDeliveredEventPosition: null,
        };
        ownFills.push(fillRecord);
        record.fills.push(fillRecord);
        record.firstFillTimeNs ??= currentTimeNs;
        record.relatedEventPositions.push(fillEventPosition);
        const isComplete = ownLeg.leavesQuantity === 0n;
        record.stages.push({ stageKind: isComplete ? "filled" : "partially_filled", eventPosition: String(fillEventPosition), simulationTimeNs: String(currentTimeNs), visibility: "exchange_truth", quantity: String(executedShares), leavesQuantity: String(ownLeg.leavesQuantity), matchId: String(matchId), detail: `${liquidityRole} fill ${executedShares} @ ${formatPrice(resting.price)} against ${fillRecord.counterpartyActor}` });
        const leavesAfterFill = ownLeg.leavesQuantity;
        scheduleExecutionReport(record, fillEventPosition, "filled", (deliveredEventPosition) => {
          fillRecord.fillReportDeliveredEventPosition = deliveredEventPosition;
          record.stages.push({ stageKind: "fill_report_delivered", eventPosition: String(deliveredEventPosition), simulationTimeNs: String(currentTimeNs), visibility: "strategy_visible", quantity: String(executedShares), leavesQuantity: String(leavesAfterFill), matchId: String(matchId), detail: "fill report delivered to the strategy" });
          strategyBelievedPositionShares += fillRecord.side === "buy" ? executedShares : -executedShares;
          const working = strategyWorkingOrders.get(record.clientOrderId);
          if (working) {
            working.believedLeaves = leavesAfterFill;
            if (leavesAfterFill === 0n) strategyWorkingOrders.delete(record.clientOrderId);
          }
          if (leavesAfterFill === 0n) evaluateStrategyQuotes();
        }, { matchId: String(matchId), price: formatPrice(resting.price), quantity: String(executedShares), leavesQuantity: String(leavesAfterFill) });
      }
      if (resting.leavesQuantity === 0n) removeFromBook(resting, fillEventPosition);
    }

    if (aggressor.leavesQuantity > 0n && incoming.limitPrice === null) {
      appendEvent({
        eventKind: "order_canceled",
        visibility: "exchange_truth",
        actor: "exchange",
        isOwnOrder: false,
        orderId: String(orderId),
        clientOrderId: String(incoming.clientOrderId),
        side: incoming.side,
        quantity: String(aggressor.leavesQuantity),
        leavesQuantity: "0",
        cumulativeQuantity: String(aggressor.cumulativeQuantity),
        cancelReason: "immediate_or_cancel",
        description: `market order ${orderId} remainder ${aggressor.leavesQuantity} canceled (no liquidity)`,
      });
    } else if (aggressor.leavesQuantity > 0n) {
      book.append(aggressor);
      const restedEventPosition = appendEvent({
        eventKind: "order_rested",
        visibility: "exchange_truth",
        actor: incoming.actor,
        isOwnOrder: ownRecord !== null,
        orderId: String(orderId),
        clientOrderId: String(incoming.clientOrderId),
        side: incoming.side,
        price: formatPrice(aggressor.price),
        leavesQuantity: String(aggressor.leavesQuantity),
        cumulativeQuantity: String(aggressor.cumulativeQuantity),
        description: `order ${orderId} rests ${aggressor.leavesQuantity} @ ${formatPrice(aggressor.price)}`,
      });
      if (incoming.actor === "background_market_maker") {
        marketMakerOrdersByPrice.set(aggressor.price, [...(marketMakerOrdersByPrice.get(aggressor.price) ?? []), aggressor]);
      }
      if (ownRecord) {
        liveOwnBookOrdersByClientOrderId.set(aggressor.clientOrderId, aggressor);
        ownRecord.restedTimeNs = currentTimeNs;
        ownRecord.restedEventPosition = restedEventPosition;
        ownRecord.relatedEventPositions.push(restedEventPosition);
        const { sharesAhead } = book.sharesAndOrdersAhead(aggressor);
        ownRecord.queueAheadAtRestShares = sharesAhead;
        ownRecord.stages.push({ stageKind: "rested", eventPosition: String(restedEventPosition), simulationTimeNs: String(currentTimeNs), visibility: "exchange_truth", quantity: null, leavesQuantity: String(aggressor.leavesQuantity), matchId: null, detail: `joined the queue at ${formatPrice(aggressor.price)} behind ${sharesAhead} shares` });
        recordFifoEvidence(aggressor, restedEventPosition, "rested");
        lastRecordedSharesAhead.set(aggressor, sharesAhead);
      }
    }
    finishExchangeCommand(queueChangeReason);
  };

  const markOwnOrderCanceledAtExchange = (order: BookOrder, canceledEventPosition: number, reason: Schemas["CancelReason"]): void => {
    const record = order.ownOrderRecord;
    if (!record) return;
    recordFifoEvidence(order, canceledEventPosition, "immediately_before_cancel");
    record.relatedEventPositions.push(canceledEventPosition);
    record.stages.push({ stageKind: "canceled", eventPosition: String(canceledEventPosition), simulationTimeNs: String(currentTimeNs), visibility: "exchange_truth", quantity: String(order.leavesQuantity), leavesQuantity: "0", matchId: null, detail: `canceled (${reason}) with ${order.leavesQuantity} shares unfilled` });
    if (reason !== "end_of_run") {
      scheduleExecutionReport(record, canceledEventPosition, "canceled", (deliveredEventPosition) => {
        record.stages.push({ stageKind: "cancel_report_delivered", eventPosition: String(deliveredEventPosition), simulationTimeNs: String(currentTimeNs), visibility: "strategy_visible", quantity: null, leavesQuantity: "0", matchId: null, detail: "cancel report delivered to the strategy" });
        strategyWorkingOrders.delete(record.clientOrderId);
      }, { cancelReason: reason });
    }
  };

  // ---------- strategy ----------
  const submitStrategyCancel = (working: StrategyWorkingOrder): void => {
    const record = working.record;
    strategyWorkingOrders.delete(record.clientOrderId);
    record.cancelSubmitTimeNs = currentTimeNs;
    const cancelSubmitEventPosition = appendEvent({
      eventKind: "strategy_cancel_submitted",
      visibility: "strategy_visible",
      actor: "research_strategy",
      isOwnOrder: true,
      orderId: record.orderId === null ? null : String(record.orderId),
      clientOrderId: String(record.clientOrderId),
      side: record.side,
      price: formatPrice(record.price),
      description: `strategy cancels own order ${record.clientOrderId} @ ${formatPrice(record.price)}`,
    });
    record.relatedEventPositions.push(cancelSubmitEventPosition);
    record.stages.push({ stageKind: "cancel_submitted", eventPosition: String(cancelSubmitEventPosition), simulationTimeNs: String(currentTimeNs), visibility: "strategy_visible", quantity: null, leavesQuantity: String(working.believedLeaves), matchId: null, detail: "strategy requested cancel" });
    const cancelLatencyNs = drawLatency(config.latency.cancelLatencyNs);
    latencySamples.cancel.push(cancelLatencyNs);
    schedule(currentTimeNs + cancelLatencyNs, "exchange_processing", () => {
      record.cancelArrivalTimeNs = currentTimeNs;
      const arrivalEventPosition = appendEvent({
        eventKind: "cancel_arrived_at_exchange",
        visibility: "exchange_truth",
        actor: "research_strategy",
        isOwnOrder: true,
        orderId: record.orderId === null ? null : String(record.orderId),
        clientOrderId: String(record.clientOrderId),
        side: record.side,
        description: `cancel for own order ${record.clientOrderId} arrives at exchange`,
      });
      record.relatedEventPositions.push(arrivalEventPosition);
      record.stages.push({ stageKind: "cancel_arrived_at_exchange", eventPosition: String(arrivalEventPosition), simulationTimeNs: String(currentTimeNs), visibility: "exchange_truth", quantity: null, leavesQuantity: null, matchId: null, detail: "cancel reached the exchange" });
      const liveOrder = liveOwnBookOrdersByClientOrderId.get(record.clientOrderId);
      if (!liveOrder) {
        const rejectedEventPosition = appendEvent({
          eventKind: "order_rejected",
          visibility: "exchange_truth",
          actor: "exchange",
          isOwnOrder: true,
          orderId: null,
          clientOrderId: String(record.clientOrderId),
          side: record.side,
          rejectReason: "unknown_order",
          rejectedCommand: "cancel",
          description: `cancel for ${record.clientOrderId} rejected: order already complete (cancel race lost)`,
        });
        cancelRaceLostEventPositions.push(rejectedEventPosition);
        record.relatedEventPositions.push(rejectedEventPosition);
        record.stages.push({ stageKind: "cancel_rejected_too_late", eventPosition: String(rejectedEventPosition), simulationTimeNs: String(currentTimeNs), visibility: "exchange_truth", quantity: null, leavesQuantity: "0", matchId: null, detail: "cancel arrived after the order had fully filled" });
        scheduleExecutionReport(record, rejectedEventPosition, "cancel_rejected", () => {}, { rejectReason: "unknown_order", rejectedCommand: "cancel" });
        finishExchangeCommand("queue_advanced_by_cancel");
        return;
      }
      const canceledEventPosition = appendEvent({
        eventKind: "order_canceled",
        visibility: "exchange_truth",
        actor: "exchange",
        isOwnOrder: true,
        orderId: String(liveOrder.orderId),
        clientOrderId: String(liveOrder.clientOrderId),
        side: liveOrder.side,
        price: formatPrice(liveOrder.price),
        quantity: String(liveOrder.leavesQuantity),
        leavesQuantity: "0",
        cumulativeQuantity: String(liveOrder.cumulativeQuantity),
        cancelReason: "user_requested",
        description: `own order ${liveOrder.orderId} canceled, ${liveOrder.leavesQuantity} shares removed`,
      });
      markOwnOrderCanceledAtExchange(liveOrder, canceledEventPosition, "user_requested");
      removeFromBook(liveOrder, canceledEventPosition);
      finishExchangeCommand("queue_advanced_by_cancel");
    });
  };

  const submitStrategyOrder = (side: Side, priceUnits: bigint): void => {
    const clientOrderId = nextStrategyClientOrderId++;
    const submitEventPosition = appendEvent({
      eventKind: "strategy_order_submitted",
      visibility: "strategy_visible",
      actor: "research_strategy",
      isOwnOrder: true,
      clientOrderId: String(clientOrderId),
      side,
      price: formatPrice(priceUnits),
      quantity: String(quoteQuantity),
      description: `strategy submits ${side} ${quoteQuantity} @ ${formatPrice(priceUnits)}`,
    });
    const record: OwnOrderRecord = {
      clientOrderId,
      orderId: null,
      side,
      price: priceUnits,
      quantity: quoteQuantity,
      submitEventPosition,
      submitTimeNs: currentTimeNs,
      arrivalEventPosition: null,
      arrivalTimeNs: null,
      acceptedEventPosition: null,
      acceptedTimeNs: null,
      acknowledgmentDeliveredEventPosition: null,
      acknowledgmentDeliveredTimeNs: null,
      restedTimeNs: null,
      restedEventPosition: null,
      removedFromBookTimeNs: null,
      removedFromBookEventPosition: null,
      cancelSubmitTimeNs: null,
      cancelArrivalTimeNs: null,
      firstFillTimeNs: null,
      stages: [{ stageKind: "strategy_submitted", eventPosition: String(submitEventPosition), simulationTimeNs: String(currentTimeNs), visibility: "strategy_visible", quantity: String(quoteQuantity), leavesQuantity: null, matchId: null, detail: `limit ${side} ${quoteQuantity} @ ${formatPrice(priceUnits)} from visible mid` }],
      fifoAheadEvidence: [],
      relatedEventPositions: [submitEventPosition],
      fills: [],
      queueAheadAtRestShares: null,
    };
    ownOrdersInSubmitOrder.push(record);
    strategyWorkingOrders.set(clientOrderId, { record, side, price: priceUnits, believedLeaves: quoteQuantity });
    const orderEntryLatencyNs = drawLatency(config.latency.orderEntryLatencyNs);
    latencySamples.order_entry.push(orderEntryLatencyNs);
    schedule(currentTimeNs + orderEntryLatencyNs, "exchange_processing", () => {
      const arrivalEventPosition = appendEvent({
        eventKind: "order_arrived_at_exchange",
        visibility: "exchange_truth",
        actor: "research_strategy",
        isOwnOrder: true,
        clientOrderId: String(clientOrderId),
        side,
        price: formatPrice(priceUnits),
        quantity: String(quoteQuantity),
        description: `own order ${clientOrderId} arrives at exchange`,
      });
      record.arrivalEventPosition = arrivalEventPosition;
      record.arrivalTimeNs = currentTimeNs;
      record.relatedEventPositions.push(arrivalEventPosition);
      record.stages.push({ stageKind: "arrived_at_exchange", eventPosition: String(arrivalEventPosition), simulationTimeNs: String(currentTimeNs), visibility: "exchange_truth", quantity: String(quoteQuantity), leavesQuantity: null, matchId: null, detail: "order reached the exchange" });
      processIncomingOrderAtExchange({ actor: "research_strategy", side, limitPrice: priceUnits, quantity: quoteQuantity, clientOrderId, ownOrderRecord: record, arrivalEventPosition });
    });
  };

  function evaluateStrategyQuotes(): void {
    if (simulationFinished || strategyVisibleMidTimesTwo === null || strategyVisibleBestBid === null || strategyVisibleBestAsk === null) return;
    if (strategyLastRequoteTimeNs !== null && currentTimeNs - strategyLastRequoteTimeNs < minimumRequoteIntervalNs) {
      if (!strategyReevaluationScheduled) {
        strategyReevaluationScheduled = true;
        schedule(strategyLastRequoteTimeNs + minimumRequoteIntervalNs, "strategy_processing", () => {
          strategyReevaluationScheduled = false;
          evaluateStrategyQuotes();
        });
      }
      return;
    }
    const widthUnits = quoteWidthTicks * tickSizeUnits;
    const desiredPriceBySide: Record<Side, bigint> = {
      buy: floorDivide(strategyVisibleMidTimesTwo - widthUnits, 2n * tickSizeUnits) * tickSizeUnits,
      sell: ceilDivide(strategyVisibleMidTimesTwo + widthUnits, 2n * tickSizeUnits) * tickSizeUnits,
    };
    let changedAnything = false;
    for (const side of ["buy", "sell"] as const) {
      const workingOnSide = [...strategyWorkingOrders.values()].filter((working) => working.side === side);
      const keeper = workingOnSide.find((working) => working.price === desiredPriceBySide[side]);
      for (const working of workingOnSide) {
        if (working !== keeper) {
          submitStrategyCancel(working);
          changedAnything = true;
        }
      }
      if (keeper) continue;
      let pendingSameSideShares = 0n;
      for (const working of strategyWorkingOrders.values()) if (working.side === side) pendingSameSideShares += working.believedLeaves;
      const projectedPosition = strategyBelievedPositionShares + (side === "buy" ? pendingSameSideShares + quoteQuantity : -(pendingSameSideShares + quoteQuantity));
      const exceedsPositionLimit = (projectedPosition < 0n ? -projectedPosition : projectedPosition) > maxAbsolutePositionShares;
      const exceedsOpenOrderLimit = BigInt(strategyWorkingOrders.size) >= maxOpenOrders;
      if (exceedsPositionLimit || exceedsOpenOrderLimit) {
        blockedOrderEventPositions.push(
          appendEvent({
            eventKind: "risk_limit_blocked_order",
            visibility: "strategy_visible",
            actor: "research_strategy",
            isOwnOrder: true,
            side,
            price: formatPrice(desiredPriceBySide[side]),
            quantity: String(quoteQuantity),
            description: exceedsPositionLimit
              ? `${side} quote blocked: believed position ${strategyBelievedPositionShares} would exceed ${maxAbsolutePositionShares}`
              : `${side} quote blocked: open order limit ${maxOpenOrders}`,
          }),
        );
        continue;
      }
      submitStrategyOrder(side, desiredPriceBySide[side]);
      changedAnything = true;
    }
    if (changedAnything) strategyLastRequoteTimeNs = currentTimeNs;
  }

  // ---------- background agents ----------
  let fairValueTicks = referencePriceUnits / tickSizeUnits;
  const marketMakerTargetLevels = (): Map<bigint, { side: Side; shares: bigint }> => {
    const targets = new Map<bigint, { side: Side; shares: bigint }>();
    targets.set((fairValueTicks - 1n) * tickSizeUnits, { side: "buy", shares: bestLevelShares });
    targets.set((fairValueTicks - 2n) * tickSizeUnits, { side: "buy", shares: deeperLevelShares });
    targets.set((fairValueTicks + 1n) * tickSizeUnits, { side: "sell", shares: bestLevelShares });
    targets.set((fairValueTicks + 2n) * tickSizeUnits, { side: "sell", shares: deeperLevelShares });
    return targets;
  };

  const performMarketMakerRequote = (stepIndex: number): void => {
    if (stepIndex > 0) fairValueTicks += fairValueMovesInTicks[stepIndex];
    const targets = marketMakerTargetLevels();
    let anyCancel = false;
    for (const order of [...marketMakerOrdersByPrice.values()].flat()) {
      const target = targets.get(order.price);
      if (target && target.side === order.side) continue;
      const canceledEventPosition = appendEvent({
        eventKind: "order_canceled",
        visibility: "exchange_truth",
        actor: "background_market_maker",
        orderId: String(order.orderId),
        clientOrderId: String(order.clientOrderId),
        side: order.side,
        price: formatPrice(order.price),
        quantity: String(order.leavesQuantity),
        leavesQuantity: "0",
        cumulativeQuantity: String(order.cumulativeQuantity),
        cancelReason: "user_requested",
        description: `market maker withdraws ${order.side} ${order.leavesQuantity} @ ${formatPrice(order.price)}`,
      });
      removeFromBook(order, canceledEventPosition);
      anyCancel = true;
    }
    if (anyCancel) refreshOwnQueueEvidence(events.length - 1, "queue_advanced_by_cancel");
    // Withdraw first, then post the new levels from the inside out (the best level first).
    const orderedTargets = [...targets.entries()].sort(([leftPrice], [rightPrice]) => {
      const leftDistance = leftPrice > fairValueTicks * tickSizeUnits ? leftPrice - fairValueTicks * tickSizeUnits : fairValueTicks * tickSizeUnits - leftPrice;
      const rightDistance = rightPrice > fairValueTicks * tickSizeUnits ? rightPrice - fairValueTicks * tickSizeUnits : fairValueTicks * tickSizeUnits - rightPrice;
      return leftDistance === rightDistance ? (leftPrice < rightPrice ? -1 : 1) : leftDistance < rightDistance ? -1 : 1;
    });
    for (const [price, target] of orderedTargets) {
      const existingShares = (marketMakerOrdersByPrice.get(price) ?? []).reduce((total, order) => total + order.leavesQuantity, 0n);
      if (existingShares * 2n >= target.shares) continue;
      processIncomingOrderAtExchange({ actor: "background_market_maker", side: target.side, limitPrice: price, quantity: target.shares, clientOrderId: nextBackgroundClientOrderId++, ownOrderRecord: null, arrivalEventPosition: null });
    }
    if (anyCancel) finishExchangeCommand("queue_advanced_by_cancel");
    const nextStepTimeNs = BigInt(stepIndex + 1) * fairValueStepIntervalNs;
    if (nextStepTimeNs < endTimeNs) schedule(nextStepTimeNs, "exchange_processing", () => performMarketMakerRequote(stepIndex + 1));
  };

  const scheduleNextNoiseTaker = (afterTimeNs: bigint): void => {
    const arrivalTimeNs = afterTimeNs + noiseTakerRandom.nextExponentialNanoseconds(takerMeanInterarrivalNs);
    if (arrivalTimeNs >= endTimeNs) return;
    schedule(arrivalTimeNs, "exchange_processing", () => {
      const upcomingStepIndex = Number(currentTimeNs / fairValueStepIntervalNs) + 1;
      const upcomingMove = fairValueMovesInTicks[upcomingStepIndex] ?? 0n;
      const isInformed = upcomingMove !== 0n && noiseTakerRandom.nextUniform() < informedTakerProbability;
      const side: Side = isInformed ? (upcomingMove > 0n ? "buy" : "sell") : noiseTakerRandom.nextUniform() < 0.5 ? "buy" : "sell";
      const quantity = BigInt(noiseTakerRandom.nextIntegerInclusive(takerMinimumLots, takerMaximumLots)) * takerLotShares;
      const clientOrderId = nextBackgroundClientOrderId++;
      const arrivalEventPosition = appendEvent({
        eventKind: "order_arrived_at_exchange",
        visibility: "exchange_truth",
        actor: "background_noise_taker",
        clientOrderId: String(clientOrderId),
        side,
        quantity: String(quantity),
        description: `noise taker market ${side} ${quantity} arrives`,
      });
      processIncomingOrderAtExchange({ actor: "background_noise_taker", side, limitPrice: null, quantity, clientOrderId, ownOrderRecord: null, arrivalEventPosition });
      scheduleNextNoiseTaker(currentTimeNs);
    });
  };

  schedule(0n, "exchange_processing", () => {
    appendEvent({ eventKind: "phase_changed", visibility: "exchange_truth", actor: "exchange", newPhase: "continuous", description: "instrument enters continuous trading" });
    performMarketMakerRequote(0);
  });
  scheduleNextNoiseTaker(0n);
  schedule(endTimeNs, "runner_control", () => {
    appendEvent({ eventKind: "simulation_bounds_reached", visibility: "exchange_truth", actor: "runner", description: "simulation end time reached; residual own orders are canceled, positions are kept" });
    simulationFinished = true;
    for (const order of [...liveOwnBookOrdersByClientOrderId.values()]) {
      const canceledEventPosition = appendEvent({
        eventKind: "order_canceled",
        visibility: "exchange_truth",
        actor: "runner",
        isOwnOrder: true,
        orderId: String(order.orderId),
        clientOrderId: String(order.clientOrderId),
        side: order.side,
        price: formatPrice(order.price),
        quantity: String(order.leavesQuantity),
        leavesQuantity: "0",
        cumulativeQuantity: String(order.cumulativeQuantity),
        cancelReason: "end_of_run",
        description: `residual own order ${order.orderId} canceled at run end`,
      });
      markOwnOrderCanceledAtExchange(order, canceledEventPosition, "end_of_run");
      removeFromBook(order, canceledEventPosition);
    }
    finishExchangeCommand("queue_advanced_by_cancel");
  });

  while (queue.size > 0 && !simulationFinished && !exceededEventBound) {
    const action = queue.pop()!;
    if (action.timeNs > endTimeNs) break;
    currentTimeNs = action.timeNs;
    currentEnqueueSequence = action.enqueueSequence;
    currentStage = action.stage;
    action.perform();
  }

  return {
    masterSeed,
    derivedSeeds: (["background_market_maker", "background_noise_taker", "strategy", "latency_jitter"] as const).map((streamName) => ({ streamName, seed: deriveStreamSeed(masterSeed, streamName) })),
    endTimeNs,
    initialCashUnits,
    tickSizeUnits,
    events,
    ownOrdersInSubmitOrder,
    ownFills,
    midPricePoints,
    ownQuotePoints,
    bookSnapshots,
    blockedOrderEventPositions,
    cancelRaceLostEventPositions,
    latencySamples,
    exceededEventBound,
  };
}
