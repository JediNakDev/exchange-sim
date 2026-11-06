import { expect, test } from "bun:test";
import { renderToStaticMarkup } from "react-dom/server";
import { MockResearchService } from "../mock-server/mock-research-service";
import { ExecutionSummaryPanel } from "../src/components/research/execution-summary-panel";
import { createExsimApiClient, type ApiSchemas } from "../src/lib/api/exsim-api-client";
import { readSelectedFill } from "../src/lib/api/research-workflow";

async function executionSummary(overrides: Partial<ApiSchemas["ExecutionSummary"]> = {}): Promise<ApiSchemas["RunSummary"]> {
  const service = new MockResearchService();
  const response = await service.handleRequest(new Request("http://fixture.exsim/v1/runs/run_fsw2seed303/summary"));
  const summary: ApiSchemas["RunSummary"] = await response.json();
  // Replace this panel's inputs explicitly; do not depend on mock execution math.
  return {
    ...summary,
    execution: {
      parentSide: "sell", parentQuantity: "9007199254740993", parentDeclared: true,
      observationStatus: "censored_run_end", benchmark: "reference_exchange_mid_at_decision",
      benchmarkMid: "100.00005", visibleMidAtDecision: null, decisionTimeNs: "0",
      firstFillTimeNs: null, lastFillTimeNs: null, filledQuantity: "0",
      fillNotional: "9007199254740993.1234", netFees: "-0.0001",
      participationMarketQuantity: null, unexecutedOpportunityCost: null, ...overrides,
    },
    metrics: [
      { metricKey: "decision_slippage_total", horizonNs: null, value: null, unit: "currency", exactness: "exact_fixed_point", unavailableReason: "no_parent_fills" },
      { metricKey: "implementation_shortfall_total", horizonNs: null, value: "-0.00005", unit: "currency", exactness: "exact_fixed_point", unavailableReason: null },
    ],
  };
}

test("actual execution panel renderer preserves exact money, unavailable values and window disclosure", async () => {
  const html = renderToStaticMarkup(<ExecutionSummaryPanel summary={await executionSummary()} />);
  for (const text of ["9,007,199,254,740,993.1234", "-0.0001", "-0.00005", "100.00005", "n/a", "no parent fills", "Execution window cut short", "Costs cover filled quantity only", "residual opportunity cost is unavailable", "including own trades"]) {
    expect(html).toContain(text);
  }
});

test("zero parent fill is distinct from an undeclared parent in the real renderer", async () => {
  const zero = renderToStaticMarkup(<ExecutionSummaryPanel summary={await executionSummary({ filledQuantity: "0" })} />);
  const unavailable = renderToStaticMarkup(<ExecutionSummaryPanel summary={await executionSummary({ parentDeclared: false, filledQuantity: null, fillNotional: null })} />);
  expect(zero).toMatch(/Filled parent shares<\/dt><dd[^>]*>0<\/dd>/);
  expect(unavailable).toMatch(/Filled parent shares<\/dt><dd[^>]*>n\/a<\/dd>/);
  expect(unavailable).toContain("Parent metrics are unavailable before the scheduled decision.");
});

test("old summaries with no execution block render no execution panel", async () => {
  const summary = await executionSummary();
  delete summary.execution;
  expect(renderToStaticMarkup(<ExecutionSummaryPanel summary={summary} />)).toBe("");
});

test("selected-fill search stops on a passed exact event position", async () => {
  let reads = 0;
  const client = createExsimApiClient("http://fixture.exsim", async () => {
    reads++;
    return Response.json({ items: [{ orderId: "10", eventPosition: "9007199254740994" }], page: { nextCursor: "unnecessary", limit: 200 } });
  });
  await expect(readSelectedFill(client, "run_fixture", "10", "9007199254740993", new AbortController().signal)).rejects.toThrow("selected fill is missing");
  expect(reads).toBe(1);
});

test("selected-fill search propagates an in-flight abort to the fetch signal", async () => {
  const controller = new AbortController();
  let propagated = false;
  const client = createExsimApiClient("http://fixture.exsim", async (request) => {
    const aborted = new Promise<Response>((_resolve, reject) => request.signal.addEventListener("abort", () => {
      propagated = true;
      reject(request.signal.reason);
    }, { once: true }));
    controller.abort(new Error("selection changed"));
    return aborted;
  });
  await expect(readSelectedFill(client, "run_fixture", "10", "55", controller.signal)).rejects.toThrow("selection changed");
  expect(propagated).toBe(true);
});

test("selected-fill search rejects a repeated cursor after bounded reads", async () => {
  let reads = 0;
  const client = createExsimApiClient("http://fixture.exsim", async () => {
    reads++;
    if (reads > 2) throw new Error("unbounded search");
    return Response.json({ items: [], page: { nextCursor: "same", limit: 200 } });
  });
  await expect(readSelectedFill(client, "run_fixture", "10", "55", new AbortController().signal)).rejects.toThrow("repeated a fill cursor");
  expect(reads).toBe(2);
});
