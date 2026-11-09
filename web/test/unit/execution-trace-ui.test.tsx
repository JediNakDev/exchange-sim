import { expect, test } from "bun:test";
import { readFileSync } from "node:fs";
import { renderToStaticMarkup } from "react-dom/server";
import { ExecutionTraceEvidence, ExecutionTracePanel } from "../../src/components/research/execution-trace-panel";
import { executionDecisionChartRows, executionOrderLink, readExecutionTracePage } from "../../src/components/research/execution-trace-data";
import { createExsimApiClient, type ApiSchemas } from "../../src/lib/api/exsim-api-client";

type Trace = ApiSchemas["ExecutionTracePage"];
const exact = "9007199254740993";
const run = { runId: "run_trace", masterSeed: "18446744073709551615", state: "completed" } as ApiSchemas["Run"];
const decision: ApiSchemas["ExecutionTraceRow"] = {
  index: "0", kind: "decision", simulationTimeNs: exact,
  cumulativeTargetQuantity: exact, knownFilledQuantity: "483", parentResidualQuantity: "17",
  requestedQuantity: "17", outstandingQuantity: "10", positionHeadroom: "-1",
  childCapQuantity: "10", submittedQuantity: "0", childClientOrderId: exact,
  deliveredSourceSequence: exact, deliveredSourceTimeNs: "9007199254740992",
  deliveredTouch: "100.00005", limitPrice: "100.0001", reason: "child_in_flight", report: null,
};
const report: ApiSchemas["ExecutionTraceRow"] = {
  ...decision, index: "1", kind: "child_report", cumulativeTargetQuantity: "0", knownFilledQuantity: "0",
  requestedQuantity: "0", submittedQuantity: "0", report: {
    kind: "fill", exchangeTimeNs: "9007199254740991", orderId: exact, matchId: "18446744073709551614",
    quantity: "3", leavesQuantity: "7", cumulativeQuantity: "3", rejectReason: "none", cancelReason: "none",
  },
};
function page(overrides: Partial<Trace> = {}): Trace {
  return { verification: "native_reproduction_and_replay_verified", runId: run.runId,
    windowEndTimeNs: "9999999999999999", startTimeNs: "0", parentQuantity: "500",
    finalTargetTimeNs: "9500000000000000", finalTargetQuantity: "500", observationStatus: "window_observed",
    totalCount: "1001", items: [decision, report], page: { nextCursor: "opaque_next", limit: 100 }, ...overrides };
}
const render = (trace: Trace) => renderToStaticMarkup(<ExecutionTraceEvidence trace={trace} run={run} comparisonQuery="?baseline=exp_base&candidate=exp_candidate" />);

test("observed page preserves exact evidence/seed and distinguishes target due from observation deadline", () => {
  const html = render(page());
  for (const text of [exact, run.masterSeed, "100.00005", "100.0001", "18446744073709551614", "9500000000000000", "9999999999999999", "Full execution window observed", "Current page only: 2 exact records (1 decisions, 1 delivered child reports) out of 1001 total", "deadline is not an extra final retry", "Native reproduction and independent replay verified", "child_in_flight"]) expect(html).toContain(text);
  expect(html).toContain(`/runs/run_trace/inspect?baseline=exp_base&amp;candidate=exp_candidate&amp;order=${exact}`);
});

test("censored and partial terminal evidence never claim a full-window outcome or reproduction", () => {
  const html = render(page({ observationStatus: "censored_run_end", verification: "recorded_terminal_partial" }));
  expect(html).toContain("Censored at run end");
  expect(html).toContain("Not reproduction verified");
  expect(html).toContain("recorded_terminal_partial");
  expect(html).not.toContain("Native reproduction and independent replay verified");
  expect(html).not.toContain("Full execution window observed");
  expect(render(page({ verification: undefined }))).toContain("Verification unavailable; no reproduction claim");
});

test("report-only request zeros cannot enter the decision chart or be turned into synthetic endpoint rows", () => {
  expect(executionDecisionChartRows([report])).toEqual([]);
  const rows = executionDecisionChartRows([decision, report]);
  expect(rows).toHaveLength(1);
  expect(rows[0].targetText).toBe(exact);
  expect(rows[0].filledText).toBe("483");
  expect(rows[0].bucketEndTimeNs).toBe(exact);
  const html = render(page({ items: [report] }));
  expect(html).toContain("No decision rows on this page");
  expect(html).not.toContain("Read sampled values");
});

test("empty pages, missing order IDs and active runs provide explicit next actions", () => {
  expect(render(page({ items: [] }))).toContain("This trace page is empty. Refresh to retry");
  expect(render(page({ items: [{ ...report, report: { ...report.report!, orderId: "0" } }] }))).toContain("Unavailable; use client ID / delivery time");
  for (const state of ["queued", "running", "paused"] as const) {
    const html = renderToStaticMarkup(<ExecutionTracePanel run={{ ...run, state }} />);
    expect(html).toContain("Resume a paused run or let the active run finish");
    expect(html).not.toContain("Refresh execution trace");
  }
});

test("lifecycle links retain baseline/candidate strings and exact order IDs", () => {
  const url = new URL(executionOrderLink("run_fixture", exact, "?baseline=a%26b&candidate=c%2Bd"), "http://fixture");
  expect(url.searchParams.get("baseline")).toBe("a&b");
  expect(url.searchParams.get("candidate")).toBe("c+d");
  expect(url.searchParams.get("order")).toBe(exact);
});

test("trace request fetches exactly one bounded native page with opaque cursor and propagates cancellation", async () => {
  let reads = 0;
  const client = createExsimApiClient("http://fixture", async (request) => {
    reads++;
    const url = new URL(request.url);
    expect(url.pathname).toBe("/v1/runs/run_trace/execution-trace");
    expect(url.searchParams.get("limit")).toBe("100");
    expect(url.searchParams.get("cursor")).toBe("opaque+cursor");
    return Response.json(page());
  });
  const result = await readExecutionTracePage(client, run.runId, "opaque+cursor", new AbortController().signal);
  expect(result.totalCount).toBe("1001");
  expect(reads).toBe(1);
  const controller = new AbortController();
  const aborting = createExsimApiClient("http://fixture", async (request) => new Promise<Response>((_resolve, reject) => {
    request.signal.addEventListener("abort", () => reject(new Error("selection changed")), { once: true });
    controller.abort();
  }));
  await expect(readExecutionTracePage(aborting, run.runId, undefined, controller.signal)).rejects.toThrow("selection changed");
});

test("native unavailable errors propagate rather than falling back to fabricated trace", async () => {
  const client = createExsimApiClient("http://fixture", async () => Response.json({ error: {
    code: "execution_evidence_unavailable", message: "No persisted trace", requestId: "test", retryable: false, fieldIssues: [],
  } }, { status: 409 }));
  await expect(readExecutionTracePage(client, run.runId, undefined, new AbortController().signal)).rejects.toThrow("No persisted trace");
});

test("source wires identity-clearing refresh, cursor history, API cancellation and shared page controls", () => {
  const source = readFileSync(new URL("../../src/components/research/execution-trace-panel.tsx", import.meta.url), "utf8");
  expect(source).toContain("useCursorHistory()");
  expect(source).toContain("${history.cursor ?? \"first\"}:${refresh}");
  expect(source).toContain("history.cursor, signal)");
  expect(source).toContain("key={`${run.runId}:${run.state}`}");
  expect(source).toContain("<ArtifactPageControls");
});
