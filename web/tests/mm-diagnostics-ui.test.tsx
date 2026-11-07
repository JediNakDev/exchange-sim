import { expect, test } from "bun:test";
import { renderToStaticMarkup } from "react-dom/server";
import type { ApiSchemas } from "../src/lib/api/exsim-api-client";
import { RequestDiagnosticsPanel } from "../src/components/research/request-diagnostics-panel";
import { LifecycleRepairEvidence } from "../src/components/research/run-inspect-evidence";
import { TraderRunOutcomes } from "../src/components/research/trader-run-outcomes";
import { initialOrderId } from "../src/lib/inspection-order-link";
import { MockResearchService } from "../mock-server/mock-research-service";

const renderEvidence = (evidence: Partial<ApiSchemas["OrderLifecycle"]>) => renderToStaticMarkup(<LifecycleRepairEvidence lifecycle={{ fifoAheadEvidence: [], ...evidence } as ApiSchemas["OrderLifecycle"]} onSelectEventPosition={() => {}} />);

test("505 replace rejects remain separate from 3 new rejects and zero cancel rejects", () => {
  const diagnostics: ApiSchemas["RequestDiagnostic"][] = [
    { requestKind: "new_order", submittedCount: "13", exchangeRejectedCount: "3", rejectReasons: [{ reason: "post_only_would_cross", count: "3" }] },
    { requestKind: "cancel", submittedCount: "4", exchangeRejectedCount: "0", rejectReasons: [] },
    { requestKind: "replace", submittedCount: "9007199254740993", exchangeRejectedCount: "505", rejectReasons: [{ reason: "unknown_order", count: "504" }, { reason: "post_only_would_cross", count: "1" }], arrivalTransit: { sampleCount: "600", minNs: "100000000", maxNs: "100000001", meanNs: "100000000.123456789" } },
  ];
  const html = renderToStaticMarkup(<RequestDiagnosticsPanel diagnostics={diagnostics} />);
  expect(html).toContain("3 / 13");
  expect(html).toContain("0 / 4");
  expect(html).toContain("505 / 9007199254740993");
  expect(html).toContain("unknown_order: 504; post_only_would_cross: 1");
  for (const exact of ["600", "100000000", "100000001", "100000000.123456789"]) expect(html).toContain(exact);
  expect(html).toContain("Pre-trade host risk blocks");
  expect(html).toContain("observed arrivals, including rejected requests");
});

test("missing diagnostics and transit samples are unavailable rather than zero", () => {
  expect(renderToStaticMarkup(<RequestDiagnosticsPanel diagnostics={undefined} />)).toContain("Request diagnostics unavailable");
  const html = renderToStaticMarkup(<RequestDiagnosticsPanel diagnostics={[]} />);
  expect(html.match(/Unavailable/g)?.length).toBe(21);
  const evidence = renderEvidence({});
  expect(evidence).toContain("Replacement transit evidence unavailable");
  expect(evidence).toContain("FIFO truncation status unavailable");
});

test("priority reset retains unavailable replace and nonzero subsequent rest alongside kept reduction", () => {
  const row: ApiSchemas["FifoAheadEvidence"] = {
    eventPosition: "9007199254740993", simulationTimeNs: "100000000", reason: "rested", source: "exact_simulated_truth", price: "99.9900",
    sharesAhead: "125", ordersAhead: "2", levelTotalShares: "225", orderId: "12", replacedOldOrderId: "7", priorityKept: false, queuePositionAvailable: true,
  };
  const html = renderEvidence({ fifoEvidenceTruncated: true, fifoAheadEvidence: [
    { ...row, eventPosition: "41", reason: "replace_priority_reset", sharesAhead: null, ordersAhead: null, levelTotalShares: null, queuePositionAvailable: false },
    row,
    { ...row, eventPosition: "43", reason: "replace_priority_kept", sharesAhead: "123", levelTotalShares: "173", priorityKept: true },
  ] });
  for (const value of ["9007199254740993", "125", "123", "173", "99.9900", "Kept", "Lost", "Unavailable at replacement; see subsequent rest", "truncated at 500 rows"]) expect(html).toContain(value);
  expect(html).toContain('>12</td>');
  expect(html).toContain('>7</td>');
  expect(html.match(/FIFO event/g)?.length).toBe(3);
});

test("only actual related fill evidence labels a causal race and exact jump targets", () => {
  const transit: ApiSchemas["ReplaceTransit"] = { submitEventPosition: "101", arrivalEventPosition: "151", transitNs: "100000000", fillBeforeArrivalEventPosition: null };
  const noFill = renderEvidence({ replaceTransits: [transit] });
  expect(noFill).not.toContain("Observed causal fill race");
  expect(noFill).toContain("No related fill observed");
  const filled = renderEvidence({ replaceTransits: [{ ...transit, fillBeforeArrivalEventPosition: "121" }] });
  for (const text of ["Replace submit #101", "Replace arrival #151", "Fill before replace arrival #121", "Observed causal fill race", "100000000"]) expect(filled).toContain(text);
});

test("direct lifecycle link accepts decimal IDs exactly and rejects malformed IDs", () => {
  for (const id of ["0", "7", "9007199254740993", "18446744073709551615"]) expect(initialOrderId(id)).toBe(id);
  for (const id of [null, "", "-1", "1.0", "1e3", " 7", "07", "NaN"]) expect(initialOrderId(id)).toBeNull();
});

test("markout shares and eligibility explain per-share weighting without inventing decomposition", async () => {
  const service = new MockResearchService();
  const summary: ApiSchemas["RunSummary"] = await (await service.handleRequest(new Request("http://fixture.exsim/v1/runs/run_fsw2seed303/summary"))).json();
  summary.markouts[0].availableShares = "135665";
  let html = renderToStaticMarkup(<TraderRunOutcomes summary={summary} />);
  expect(html).toContain("135665 eligible shares");
  expect(html).toContain("/ available shares");
  expect(html).toContain("no-mark and censored fills are excluded");
  delete summary.markouts[0].availableShares;
  html = renderToStaticMarkup(<TraderRunOutcomes summary={summary} />);
  expect(html).toContain("Unavailable eligible shares");
});

test("original and replacement fills keep their own identity beside latest chain totals", async () => {
  const { FocusPanel } = await import("../src/components/research/run-inspect-view");
  const service = new MockResearchService();
  const fills: ApiSchemas["FillPage"] = await (await service.handleRequest(new Request("http://fixture.exsim/v1/runs/run_fsw2seed303/fills?limit=1"))).json();
  const fill = fills.items[0];
  const lifecycle: ApiSchemas["OrderLifecycle"] = await (await service.handleRequest(new Request(`http://fixture.exsim/v1/runs/run_fsw2seed303/orders/${fill.orderId}/lifecycle`))).json();
  lifecycle.order = { ...lifecycle.order, orderId: "12", quantity: "161", cumulativeQuantity: "161", fillCount: 2 };
  for (const [orderId, eventPosition, matchId] of [["7", "45", "4"], ["12", "75", "5"]]) {
    const selectedFill = { ...fill, orderId, eventPosition, matchId, quantity: "61" };
    const html = renderToStaticMarkup(<FocusPanel focusedFill={selectedFill} lifecycle={lifecycle} lifecycleFailure={null} onRetryLifecycle={() => {}} onSelectEventPosition={() => {}} />);
    expect(html).toContain(`order ${orderId}, event ${eventPosition}, match ${matchId}`);
    expect(html).toContain("Order 12: latest chain state");
    expect(html).toContain("These totals are not a snapshot at the selected fill");
    expect(html).toContain("161 filled in 2 fills across the chain");
  }
});
