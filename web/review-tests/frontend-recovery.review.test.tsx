/**
 * Report-only reproducers for FRONTEND_RECOVERY_REVIEW_2026-11-07.md.
 * Run alone: EXSIM_RUN_FRONTEND_REVIEW_FAILURES=1 bun test ./review-tests/frontend-recovery.review.test.tsx
 * Expected on 4595fa0: five failures covering four findings. Default: skipped known-defect assertions.
 * These invoke actual component bodies and callbacks with isolated hook slots,
 * Next navigation and API-resource fixtures. They do not render a DOM or prove
 * browser behavior. Run alone because module mocks replace React hooks.
 */
import { beforeEach, expect, mock, test } from "bun:test";
import * as React from "react";
import { BASELINE_CONFIGURATIONS } from "../mock-server/baseline-catalog";
import { MockResearchService } from "../mock-server/mock-research-service";
import type { ApiSchemas } from "../src/lib/api/exsim-api-client";

const realReact = { ...React };
let slots: unknown[] = [];
let slotIndex = 0;
let query = "";
let runId = "run_fsw2seed303";
const failures = { code: "service_unavailable", message: "injected offline", requestId: "review", retryable: true, fieldIssues: [] };
const resources = new Map<string, { data: unknown; failure: unknown }>();
mock.module("react", () => ({
  ...realReact,
  useState: (initial: unknown) => {
    const index = slotIndex++;
    if (!(index in slots)) slots[index] = initial;
    return [slots[index], (next: unknown) => { slots[index] = typeof next === "function" ? next(slots[index]) : next; }];
  },
  useMemo: (calculate: () => unknown) => calculate(),
  useRef: (initial: unknown) => {
    const index = slotIndex++;
    if (!(index in slots)) slots[index] = { current: initial };
    return slots[index];
  },
}));
mock.module("next/navigation", () => ({
  useRouter: () => ({ push: () => {}, replace: () => {} }),
  useSearchParams: () => new URLSearchParams(query),
  useParams: () => ({ runId }),
}));
mock.module("../src/hooks/use-api-resource", () => ({
  useApiResource: (key: string | null) => ({
    data: key === null ? null : resources.get(key)?.data ?? null,
    failure: key === null ? null : resources.get(key)?.failure ?? null,
    isLoading: false,
    reload: () => {},
  }),
}));
const { ExperimentCreateView } = await import("../src/components/research/experiment-create-view");
const { RunDetailView } = await import("../src/components/research/run-detail-view");
const { RunInspectView } = await import("../src/components/research/run-inspect-view");
const { ApiFailureAlert } = await import("../src/components/research/api-failure-alert");

type Element = React.ReactElement<Record<string, unknown>>;
function elements(tree: React.ReactNode): Element[] {
  if (Array.isArray(tree)) return tree.flatMap(elements);
  if (!React.isValidElement(tree)) return [];
  const element = tree as Element;
  return [element, ...elements(element.props.children as React.ReactNode)];
}
function render(view: () => React.ReactNode) {
  slotIndex = 0;
  return elements(view());
}
function field(tree: Element[], label: string) {
  const result = tree.find((element) => element.props.label === label);
  if (!result) throw new Error(`Missing field ${label}`);
  return result;
}
async function fixture<T>(path: string): Promise<T> {
  return (await new MockResearchService().handleRequest(new Request(`http://fixture.exsim${path}`))).json();
}
const defect = process.env.EXSIM_RUN_FRONTEND_REVIEW_FAILURES === "1" ? test : test.skip;
beforeEach(() => {
  slots = [];
  slotIndex = 0;
  query = "";
  runId = "run_fsw2seed303";
  resources.clear();
  resources.set("baselines", { data: { items: BASELINE_CONFIGURATIONS }, failure: null });
});

defect("FR-01: clearing a draft interval should leave an editable form", () => {
  const first = render(ExperimentCreateView);
  (field(first, "Requote interval").props.onValueChange as (value: string) => void)("");
  expect(() => render(ExperimentCreateView)).not.toThrow();
});

defect("FR-02: changing the variant source must not retain the old edited intent", async () => {
  const parentA = await fixture<ApiSchemas["Experiment"]>("/v1/experiments/exp_fixedspreadw2");
  const parentB = await fixture<ApiSchemas["Experiment"]>("/v1/experiments/exp_fixedspreadw4");
  resources.set(`variant:${parentA.experimentId}`, { data: parentA, failure: null });
  resources.set(`variant:${parentB.experimentId}`, { data: parentB, failure: null });
  query = `from=${parentA.experimentId}`;
  const first = render(ExperimentCreateView);
  (field(first, "Name").props.onValueChange as (value: string) => void)("Edited variant A");
  query = `from=${parentB.experimentId}`;
  const second = render(ExperimentCreateView);
  expect(field(second, "Name").props.value).toBe(`${parentB.name} variant`);
});

defect("FR-03: a failed run refresh with retained data must be visible in summary", async () => {
  const run = await fixture<ApiSchemas["Run"]>(`/v1/runs/${runId}`);
  resources.set(`run:${runId}`, { data: run, failure: failures });
  const tree = render(RunDetailView);
  expect(tree.filter((element) => element.type === ApiFailureAlert)).not.toHaveLength(0);
});

defect("FR-03: a failed run refresh with retained data must be visible in inspector", async () => {
  const run = await fixture<ApiSchemas["Run"]>(`/v1/runs/${runId}`);
  resources.set(`inspect-run:${runId}`, { data: run, failure: failures });
  const wrapper = RunInspectView() as Element;
  const tree = render(() => (wrapper.type as (props: Record<string, unknown>) => React.ReactNode)(wrapper.props));
  expect(tree.filter((element) => element.type === ApiFailureAlert)).not.toHaveLength(0);
});

defect("FR-04: a selected lifecycle read failure must have an in-place retry", async () => {
  const run = await fixture<ApiSchemas["Run"]>(`/v1/runs/${runId}`);
  resources.set(`inspect-run:${runId}`, { data: run, failure: null });
  const wrapper = RunInspectView() as Element;
  const inspect = () => (wrapper.type as (props: Record<string, unknown>) => React.ReactNode)(wrapper.props);
  render(inspect);
  slots[2] = "1"; // The existing focusedOrderId state slot, equivalent to selecting order 1.
  resources.set(`lifecycle:${runId}:1:${run.progress.processedCanonicalEventCount}`, { data: null, failure: failures });
  const panel = render(inspect).find((element) => element.props.lifecycleFailure === failures)!;
  const alert = (panel.type as (props: Record<string, unknown>) => Element)(panel.props);
  expect(alert.type).toBe(ApiFailureAlert);
  expect(alert.props.onRetry).toBeFunction();
});
