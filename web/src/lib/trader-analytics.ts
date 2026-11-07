import type { ApiSchemas } from "@/lib/api/exsim-api-client";
import { decimalTextSign } from "@/lib/exact-decimal-format";

// Direction is supplied by the service. Never infer a winner from magnitude.
export function interpretPairedDifference(metric: ApiSchemas["MetricComparison"]): string {
  const { sampleCount, meanDifference } = metric.dispersion;
  if (sampleCount === 0 || meanDifference === null) return "No paired observations available";
  const sign = decimalTextSign(meanDifference);
  if (sign === 0) return "No mean difference";
  if (metric.betterDirection === "neither") return "Directional change; no preferred direction";
  const better = metric.betterDirection === "higher" ? sign > 0 : sign < 0;
  return better ? "Candidate improves the paired mean" : "Candidate worsens the paired mean";
}

export function priorityComparisonMetrics(metrics: ApiSchemas["MetricComparison"][]) {
  const execution = metrics.some((metric) => metric.metricKey === "completion_ratio" && metric.dispersion.sampleCount > 0);
  const keys: ApiSchemas["MetricKey"][] = execution
    ? ["completion_ratio", "residual_qty", "implementation_shortfall_total", "decision_slippage_total"]
    : ["net_pnl", "max_drawdown", "ending_position", "markout_total"];
  return keys.flatMap((key) => {
    const matches = metrics.filter((metric) => metric.metricKey === key);
    return key === "markout_total" ? matches.slice(0, 1) : matches;
  });
}
