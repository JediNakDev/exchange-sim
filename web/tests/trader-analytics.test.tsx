import { expect, test } from "bun:test";
import { renderToStaticMarkup } from "react-dom/server";
import { MockResearchService } from "../mock-server/mock-research-service";
import type { ApiSchemas } from "../src/lib/api/exsim-api-client";
import { interpretPairedDifference, priorityComparisonMetrics } from "../src/lib/trader-analytics";
import { TraderRunOutcomes } from "../src/components/research/trader-run-outcomes";
import { PairedSeedAnalysis } from "../src/components/research/paired-seed-analysis";

async function fixtures() {
  const service = new MockResearchService();
  const summary: ApiSchemas["RunSummary"] = await (await service.handleRequest(new Request("http://fixture.exsim/v1/runs/run_fsw2seed303/summary"))).json();
  const comparison: ApiSchemas["ExperimentComparison"] = await (await service.handleRequest(new Request("http://fixture.exsim/v1/comparisons?baselineExperimentId=exp_fixedspreadw2&candidateExperimentId=exp_fixedspreadw4"))).json();
  return { summary, comparison };
}

test("paired interpretation uses service direction and exact sign below float resolution", async () => {
  const { comparison } = await fixtures();
  const metric = structuredClone(comparison.metricComparisons[0]);
  metric.dispersion.sampleCount = 2;
  metric.dispersion.meanDifference = "-0.0000000000000000000000000000000000000000001";
  metric.betterDirection = "lower";
  expect(interpretPairedDifference(metric)).toContain("improves");
  metric.betterDirection = "higher";
  expect(interpretPairedDifference(metric)).toContain("worsens");
  metric.betterDirection = "neither";
  expect(interpretPairedDifference(metric)).toContain("no preferred direction");
  metric.dispersion.meanDifference = "0.00000";
  expect(interpretPairedDifference(metric)).toBe("No mean difference");
  metric.dispersion.sampleCount = 0;
  expect(interpretPairedDifference(metric)).toContain("No paired observations");
});

test("market-making priorities do not promote unavailable execution metrics", async () => {
  const { comparison } = await fixtures();
  expect(priorityComparisonMetrics(comparison.metricComparisons)[0].metricKey).toBe("net_pnl");
  const completion = { ...comparison.metricComparisons[0], metricKey: "completion_ratio" as const };
  completion.dispersion = { ...completion.dispersion, sampleCount: 1 };
  expect(priorityComparisonMetrics([...comparison.metricComparisons, completion])[0].metricKey).toBe("completion_ratio");
});

test("paired chart evidence retains exact large values, missing pairs and run links", async () => {
  const { comparison } = await fixtures();
  const metric = comparison.metricComparisons.find((item) => item.metricKey === "net_pnl")!;
  metric.perSeed[0] = { ...metric.perSeed[0], masterSeed: "9007199254740993", baselineValue: "9007199254740993.00005", candidateValue: null, difference: null };
  const html = renderToStaticMarkup(<PairedSeedAnalysis comparison={comparison} />);
  for (const text of ["9007199254740993", "9,007,199,254,740,993.00005", "Unavailable", "missing values are unavailable", `/runs/${metric.perSeed[0].baselineRunId}`, "Available pairs", "better / worse / tied"]) expect(html).toContain(text);
});

test("non-comparable worlds produce no paired result surface", async () => {
  const { comparison } = await fixtures();
  comparison.comparable = false;
  expect(renderToStaticMarkup(<PairedSeedAnalysis comparison={comparison} />)).toBe("");
});

test("no paired observations are unavailable, not zero or a winner", async () => {
  const { comparison } = await fixtures();
  for (const metric of comparison.metricComparisons) {
    metric.dispersion = { ...metric.dispersion, sampleCount: 0, meanDifference: null };
    metric.perSeed = metric.perSeed.map((pair) => ({ ...pair, difference: null }));
  }
  const html = renderToStaticMarkup(<PairedSeedAnalysis comparison={comparison} />);
  expect(html).toContain("No available paired values");
  expect(html).toContain("at least two observations");
  expect(html).not.toContain("Candidate improves");
});

test("market-making outcomes expose unavailable valuation and censored markouts", async () => {
  const { summary } = await fixtures();
  delete summary.execution;
  summary.accounting.netPnl = null;
  summary.accounting.markUnavailableReason = "no_mark";
  summary.markouts[0] = { ...summary.markouts[0], availableFillCount: "0", censoredFillCount: "17", meanMarkoutPerShare: null };
  const html = renderToStaticMarkup(<TraderRunOutcomes summary={summary} />);
  for (const text of ["P&amp;L and inventory risk", "n/a", "Terminal valuation unavailable", "Unavailable", "17", "censored", "Spread capture alone is not profit"]) expect(html).toContain(text);
});

test("execution outcomes lead with residual and signed costs rather than P&L", async () => {
  const { summary } = await fixtures();
  summary.execution = { parentSide: "buy", parentQuantity: "9007199254740993", filledQuantity: "1", parentDeclared: true, observationStatus: "censored_run_end", benchmark: "reference_exchange_mid_at_decision", benchmarkMid: "100", visibleMidAtDecision: null, decisionTimeNs: "0", firstFillTimeNs: "1", lastFillTimeNs: "1", fillNotional: "100", netFees: "0", participationMarketQuantity: null, unexecutedOpportunityCost: null };
  summary.metrics = [{ metricKey: "implementation_shortfall_total", horizonNs: null, value: "-0.00005", unit: "currency", exactness: "exact_fixed_point", unavailableReason: null }];
  const html = renderToStaticMarkup(<TraderRunOutcomes summary={summary} />);
  for (const text of ["Completion and execution cost", "9,007,199,254,740,993", "-0.00005", "USD", "shares", "unavailable", "Residual opportunity cost"]) expect(html).toContain(text);
  expect(html).not.toContain("Net P&amp;L (USD)");
});

test("neutral metric preference is not presented as equality", async () => {
  const { comparison } = await fixtures();
  const metric = comparison.metricComparisons.find((item) => item.metricKey === "ending_position")!;
  metric.betterDirection = "neither";
  metric.dispersion = { ...metric.dispersion, sampleCount: 3, tiedCount: 3 };
  comparison.metricComparisons = [metric];
  const html = renderToStaticMarkup(<PairedSeedAnalysis comparison={comparison} />);
  expect(html).toContain("preference counts not applicable");
  expect(html).not.toContain("better / worse / tied:");
});

test("execution comparison carries benchmark, quantity scope and volume horizon definitions", async () => {
  const { comparison } = await fixtures();
  comparison.metricComparisons.push({ ...comparison.metricComparisons[0], metricKey: "completion_ratio", unit: "ratio" });
  const html = renderToStaticMarkup(<PairedSeedAnalysis comparison={comparison} />);
  for (const text of ["Execution comparison definitions", "exchange mid at parent declaration", "Positive signed costs", "filled shares only", "opportunity cost is unavailable", "last parent fill", "including own trades"]) expect(html).toContain(text);
});
