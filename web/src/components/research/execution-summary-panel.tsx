import { DefinitionList } from "@/components/research/definition-list";
import { SignedFigure } from "@/components/research/signed-figure";
import type { ReactNode } from "react";
import { Badge } from "@/components/ui/badge";
import { ResearchSection } from "@/components/research/research-section";
import type { ApiSchemas } from "@/lib/api/exsim-api-client";
import { formatDecimalForDisplay, formatIntegerText, formatSimulationTimeNs } from "@/lib/exact-decimal-format";
import { formatMetricValue, humanizeEnumValue, metricLabel } from "@/lib/research-labels";

const EXECUTION_METRICS: ApiSchemas["MetricKey"][] = ["completion_ratio", "residual_qty", "decision_slippage_total", "decision_slippage_per_share", "implementation_shortfall_total", "implementation_shortfall_per_share", "participation_rate"];
const WINDOW_LABEL: Record<ApiSchemas["ExecutionSummary"]["observationStatus"], string> = {
  ongoing: "Execution window still open",
  window_observed: "Execution window observed",
  censored_run_end: "Execution window cut short",
};

export function ExecutionSummaryPanel({ summary }: { summary: ApiSchemas["RunSummary"] }) {
  const execution = summary.execution;
  if (!execution) return null;
  const facts: [string, ReactNode][] = [
    ["Parent instruction", `${execution.parentSide} ${formatIntegerText(execution.parentQuantity)} shares`],
    ["Filled parent shares", formatIntegerText(execution.filledQuantity)],
    ["Fill notional (USD)", formatDecimalForDisplay(execution.fillNotional)],
    ["Net fees paid (USD)", <span key="net-fees" title={formatDecimalForDisplay(execution.netFees)}><SignedFigure formattedValue={formatDecimalForDisplay(execution.netFees)} favourableDirection="lower" /></span>],
    ["Exchange mid at decision", formatDecimalForDisplay(execution.benchmarkMid)],
    ["Delivered mid at decision", formatDecimalForDisplay(execution.visibleMidAtDecision)],
    ["Parent decision time", execution.decisionTimeNs === null ? "Not declared yet" : formatSimulationTimeNs(execution.decisionTimeNs, 6)],
    ["First parent fill", execution.firstFillTimeNs === null ? "No fill observed" : formatSimulationTimeNs(execution.firstFillTimeNs, 6)],
    ["Last parent fill", execution.lastFillTimeNs === null ? "No fill observed" : formatSimulationTimeNs(execution.lastFillTimeNs, 6)],
    ["Participation market volume (shares)", formatIntegerText(execution.participationMarketQuantity)],
  ];
  return (
    <ResearchSection className="xl:col-span-2" title="Parent-order execution" description={<> <Badge variant="outline">{WINDOW_LABEL[execution.observationStatus]}</Badge>
          {!execution.parentDeclared ? "Parent metrics are unavailable before the scheduled decision." : "A completed simulation can still leave unexecuted parent shares."} </>}>
<div className="grid gap-5 lg:grid-cols-2">
        <DefinitionList className="grid-cols-[minmax(0,1fr)_minmax(0,1fr)]" rows={facts} />
        <div>
          <dl className="grid grid-cols-[minmax(0,1fr)_minmax(0,1fr)] gap-x-4 gap-y-2 text-sm" aria-label="Parent execution metrics">
            {summary.metrics.filter((metric) => EXECUTION_METRICS.includes(metric.metricKey)).map((metric) => (
              <div key={metric.metricKey} className="contents">
                <dt className="text-muted-foreground">{metricLabel(metric.metricKey, null)}{metric.exactness === "derived_float" ? <span className="ml-1 text-xs">(derived)</span> : null}</dt>
                <dd title={metric.value === null ? undefined : formatMetricValue(metric.value, metric.unit)} className="break-words font-mono text-xs tabular-nums">{metric.value === null ? humanizeEnumValue(metric.unavailableReason ?? "unavailable") : <SignedFigure formattedValue={formatMetricValue(metric.value, metric.unit)} favourableDirection={metric.metricKey.includes("shortfall") || metric.metricKey.includes("slippage") ? "lower" : "neutral"} />}</dd>
              </div>
            ))}
          </dl>
          <p className="mt-4 text-xs leading-relaxed text-muted-foreground">Costs use the exchange-truth mid at the parent decision, which may differ from the delivered mid. Positive signed costs mean worse execution for either side. Decision slippage excludes fees; implementation shortfall includes net fees. Lower filled-quantity costs do not resolve completion risk. Costs cover filled quantity only; residual opportunity cost is unavailable. Total costs are in USD; per-share costs are in USD/share. Participation uses all market trades after declaration through the last parent fill, including own trades.</p>
        </div>
      </div>
    </ResearchSection>
  );
}
