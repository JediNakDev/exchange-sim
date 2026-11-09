// Independent reconciliation of the seeded fixture runs, using only API
// responses and exact BigInt arithmetic: fills explain positions, the cash
// ledger identity holds, P&L decomposes, and markouts are censored past run end.

import { describe, expect, test } from "bun:test";
import { FIRST_SLICE_MASTER_SEEDS } from "../mock-server/baseline-catalog";
import type { ApiSchemas } from "../../src/lib/api/exsim-api-client";
import { decimalTextToScaledBigInt } from "../../src/lib/exact-decimal-format";
import { createMockApiHarness, expectContractResponse } from "./mock-api-test-harness";

const harness = createMockApiHarness();
const { apiClient } = harness;
const CASH_DIGITS = 4;
const MARK_DIGITS = 5;
const cashUnits = (decimalText: string) => decimalTextToScaledBigInt(decimalText, CASH_DIGITS);
const markUnits = (decimalText: string) => decimalTextToScaledBigInt(decimalText, MARK_DIGITS);

async function loadRunArtifacts(runId: string) {
  const runPath = { params: { path: { runId } } };
  const run = expectContractResponse(harness, await apiClient.GET("/v1/runs/{runId}", runPath), "get", "/v1/runs/{runId}", 200);
  const manifest = expectContractResponse(harness, await apiClient.GET("/v1/runs/{runId}/manifest", runPath), "get", "/v1/runs/{runId}/manifest", 200);
  const summary = expectContractResponse(harness, await apiClient.GET("/v1/runs/{runId}/summary", runPath), "get", "/v1/runs/{runId}/summary", 200);
  const fills: ApiSchemas["Fill"][] = [];
  let cursor: string | undefined;
  do {
    const fillPage = expectContractResponse(harness, await apiClient.GET("/v1/runs/{runId}/fills", { params: { path: { runId }, query: { limit: 25, ...(cursor ? { cursor } : {}) } } }), "get", "/v1/runs/{runId}/fills", 200);
    fills.push(...fillPage.items);
    cursor = fillPage.page.nextCursor ?? undefined;
  } while (cursor);
  return { run, manifest, summary, fills };
}

const FIXTURE_RUN_IDS = ["2", "4"].flatMap((quoteWidthTicks) => FIRST_SLICE_MASTER_SEEDS.map((masterSeed) => `run_fsw${quoteWidthTicks}seed${masterSeed}`));

describe.each(FIXTURE_RUN_IDS)("fixture run %s reconciles", (runId) => {
  test("fills, positions, cash, fees and P&L", async () => {
    const { run, manifest, summary, fills } = await loadRunArtifacts(runId);
    expect(run.state).toBe("completed");
    expect(fills.length).toBeGreaterThan(5);
    const accounting = summary.accounting;

    let positionShares = 0n;
    let cash = cashUnits(manifest.initialCash);
    let totalFees = 0n;
    let maximumLong = 0n;
    let maximumShort = 0n;
    for (const fill of fills) {
      const signedQuantity = fill.side === "buy" ? BigInt(fill.quantity) : -BigInt(fill.quantity);
      const notional = cashUnits(fill.price) * BigInt(fill.quantity);
      const fee = cashUnits(fill.feePerShare) * BigInt(fill.quantity);
      expect(cashUnits(fill.ledgerEffect.fee)).toBe(fee);
      expect(BigInt(fill.ledgerEffect.positionBeforeShares)).toBe(positionShares);
      expect(cashUnits(fill.ledgerEffect.cashBefore)).toBe(cash);
      positionShares += signedQuantity;
      cash += (fill.side === "buy" ? -notional : notional) - fee;
      totalFees += fee;
      maximumLong = positionShares > maximumLong ? positionShares : maximumLong;
      maximumShort = positionShares < maximumShort ? positionShares : maximumShort;
      expect(BigInt(fill.ledgerEffect.positionAfterShares)).toBe(positionShares);
      expect(cashUnits(fill.ledgerEffect.cashAfter)).toBe(cash);
      expect(cashUnits(fill.ledgerEffect.cumulativeFeesAfter)).toBe(totalFees);
      expect(fill.feePerShare).toBe(fill.liquidityRole === "maker" ? manifest.fees.makerFeePerShare : manifest.fees.takerFeePerShare);
    }
    const absoluteLimit = BigInt(manifest.risk.maxAbsolutePositionShares);
    expect(maximumLong <= absoluteLimit && -maximumShort <= absoluteLimit).toBe(true);
    expect(BigInt(summary.risk.maxLongPositionShares)).toBe(maximumLong);
    expect(BigInt(summary.risk.maxShortPositionShares)).toBe(maximumShort);

    // Fills explain the position; the cash ledger identity holds.
    expect(BigInt(accounting.positionShares)).toBe(positionShares);
    expect(BigInt(run.terminalDiagnostics!.residualPositionShares)).toBe(positionShares);
    expect(cashUnits(accounting.cash)).toBe(cash);
    expect(cashUnits(accounting.netFees)).toBe(totalFees);
    expect(cashUnits(accounting.grossRealizedPnl)).toBe(cashUnits(fills.at(-1)!.ledgerEffect.cumulativeRealizedPnlAfter));
    expect(BigInt(summary.orders.fillCount)).toBe(BigInt(fills.length));
    expect(BigInt(summary.orders.filledShares)).toBe(fills.reduce((total, fill) => total + BigInt(fill.quantity), 0n));

    // Equity = cash + position x mark; net P&L = equity - initial = realized + unrealized - fees.
    const equity = markUnits(accounting.equity!);
    expect(equity).toBe(markUnits(accounting.cash) + positionShares * markUnits(accounting.markPrice!));
    expect(markUnits(accounting.netPnl!)).toBe(equity - markUnits(manifest.initialCash));
    expect(markUnits(accounting.netPnl!)).toBe(markUnits(accounting.grossRealizedPnl) + markUnits(accounting.unrealizedPnl!) - markUnits(accounting.netFees));
  });

  test("markouts are censored exactly when the horizon passes run end", async () => {
    const { manifest, summary, fills } = await loadRunArtifacts(runId);
    const runEndNs = BigInt(manifest.bounds.endSimulationTimeNs);
    for (const horizonNs of manifest.markoutHorizonsNs) {
      let availableCount = 0n;
      let censoredCount = 0n;
      let totalMarkout = 0n;
      for (const fill of fills) {
        const markout = fill.markouts.find((candidate) => candidate.horizonNs === horizonNs)!;
        const passesRunEnd = BigInt(fill.simulationTimeNs) + BigInt(horizonNs) > runEndNs;
        expect(markout.status).toBe(passesRunEnd ? "censored_past_run_end" : "available");
        if (markout.status === "censored_past_run_end") {
          expect(markout.markoutPerShare).toBeNull();
          censoredCount++;
          continue;
        }
        availableCount++;
        const perShare = fill.side === "buy" ? markUnits(markout.referenceMidPrice!) - markUnits(fill.price) : markUnits(fill.price) - markUnits(markout.referenceMidPrice!);
        expect(markUnits(markout.markoutPerShare!)).toBe(perShare);
        expect(markUnits(markout.markoutTotal!)).toBe(perShare * BigInt(fill.quantity));
        totalMarkout += perShare * BigInt(fill.quantity);
      }
      const horizonSummary = summary.markouts.find((candidate) => candidate.horizonNs === horizonNs)!;
      expect(BigInt(horizonSummary.availableFillCount)).toBe(availableCount);
      expect(BigInt(horizonSummary.censoredFillCount)).toBe(censoredCount);
      if (availableCount === 0n) expect(horizonSummary.totalMarkout).toBeNull();
      else { expect(horizonSummary.totalMarkout).not.toBeNull(); expect(markUnits(horizonSummary.totalMarkout!)).toBe(totalMarkout); }
    }
    // The 30 s horizon on a 60 s run must censor the second half of fills.
    expect(summary.markouts.find((candidate) => candidate.horizonNs === "30000000000")!.censoredFillCount).not.toBe("0");
  });

  test("fill events and FIFO evidence are consistent with the event log", async () => {
    const { fills } = await loadRunArtifacts(runId);
    const fill = fills.find((candidate) => candidate.liquidityRole === "maker")!;
    const related = fill.relatedEvents;
    const orderedPositions = [related.strategySubmitEventPosition, related.exchangeArrivalEventPosition, related.acceptedEventPosition, related.fillEventPosition].map(BigInt);
    expect([...orderedPositions].sort((left, right) => (left < right ? -1 : 1))).toEqual(orderedPositions);
    const fillEvents = expectContractResponse(harness, await apiClient.GET("/v1/runs/{runId}/events", { params: { path: { runId }, query: { fromEventPosition: fill.eventPosition, toEventPosition: fill.eventPosition } } }), "get", "/v1/runs/{runId}/events", 200);
    expect(fillEvents.items[0]).toMatchObject({ eventKind: "order_filled", orderId: fill.orderId, matchId: fill.matchId, price: fill.price, quantity: fill.quantity, visibility: "exchange_truth" });
    const lifecycle = expectContractResponse(harness, await apiClient.GET("/v1/runs/{runId}/orders/{orderId}/lifecycle", { params: { path: { runId, orderId: fill.orderId } } }), "get", "/v1/runs/{runId}/orders/{orderId}/lifecycle", 200);
    expect(lifecycle.fillMatchIds).toContain(fill.matchId);
    const lastEvidenceBeforeFill = lifecycle.fifoAheadEvidence.filter((evidence) => BigInt(evidence.eventPosition) <= BigInt(fill.eventPosition)).at(-1)!;
    expect(lastEvidenceBeforeFill.reason).toBe("immediately_before_fill");
    expect(lastEvidenceBeforeFill.price).toBe(fill.price);
  });
});
