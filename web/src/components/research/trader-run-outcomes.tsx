import type { ApiSchemas } from "@/lib/api/exsim-api-client";
import { formatDecimalForDisplay, formatIntegerText, formatDurationNs } from "@/lib/exact-decimal-format";
import { formatMetricValue, humanizeEnumValue, metricLabel } from "@/lib/research-labels";

export function TraderRunOutcomes({ summary }: { summary: ApiSchemas["RunSummary"] }) {
  const execution = summary.execution;
  const facts: [string, string][] = execution ? [
    ["Parent filled / requested (shares)", `${formatIntegerText(execution.filledQuantity)} / ${formatIntegerText(execution.parentQuantity)}`],
    ...(["completion_ratio", "residual_qty", "implementation_shortfall_total", "decision_slippage_total"] as const).map((key): [string, string] => {
      const metric = summary.metrics.find((item) => item.metricKey === key);
      return [`${metricLabel(key, null)} (${key === "completion_ratio" ? "ratio" : key === "residual_qty" ? "shares" : "USD"})`, metric?.value == null ? humanizeEnumValue(metric?.unavailableReason ?? "unavailable") : formatMetricValue(metric.value, metric.unit)];
    }),
  ] : [
    ["Net P&L (USD)", formatDecimalForDisplay(summary.accounting.netPnl, { signDisplay: "always" })],
    ["Realized / unrealized P&L (USD)", `${formatDecimalForDisplay(summary.accounting.grossRealizedPnl, { signDisplay: "always" })} / ${formatDecimalForDisplay(summary.accounting.unrealizedPnl, { signDisplay: "always" })}`],
    ["Ending position (shares)", formatDecimalForDisplay(summary.accounting.positionShares)],
    ["Max long / short (shares)", `${formatIntegerText(summary.risk.maxLongPositionShares)} / ${formatIntegerText(summary.risk.maxShortPositionShares)}`],
    ["Max drawdown (USD)", formatDecimalForDisplay(summary.risk.maxDrawdown)],
  ];
  return <section className="rounded-lg border bg-card p-5" aria-label="Run outcomes">
    <div className="mb-4 flex flex-wrap items-baseline justify-between gap-2">
      <h2 className="font-semibold">{execution ? "Completion and execution cost" : "P&L and inventory risk"}</h2>
      <p className="text-xs text-muted-foreground">{summary.availability} summary at event {summary.asOfEventPosition}</p>
    </div>
    <dl className="grid gap-4 sm:grid-cols-2 xl:grid-cols-5">{facts.map(([label, value]) => <div key={label}><dt className="text-xs text-muted-foreground">{label}</dt><dd className="mt-1 break-words font-mono text-base tabular-nums">{value}</dd></div>)}</dl>
    <p className="mt-4 max-w-4xl text-xs leading-relaxed text-muted-foreground">{execution
      ? "Positive signed costs are worse for buys and sells. Shortfall includes net fees; slippage excludes fees. Both cover filled quantity only. Residual opportunity cost is unavailable, so lower cost with lower completion is not an unconditional improvement."
      : "Net P&L includes inventory valuation and fees. Spread capture alone is not profit. Read signed markouts with eligible exposure alongside inventory and drawdown; markouts include the initial spread edge."}</p>
    {!execution ? <div className="mt-4 border-t pt-3">
      <h3 className="text-sm font-medium">Post-fill markouts</h3>
      <p className="mt-1 text-xs text-muted-foreground">Quantity-weighted USD/share against exchange mid: buy = future mid - fill price; sell = fill price - future mid. Positive is favorable and includes the initial spread edge. Per-share mean = sum(signed markout per share × eligible fill shares) / available shares. Eligible shares have an observed exchange mid at the horizon; no-mark and censored fills are excluded. Eligibility varies by horizon.</p>
      {summary.markouts.length === 0 ? <p className="mt-2 text-sm">No markout horizons reported.</p> : <dl className="mt-3 flex flex-wrap gap-x-8 gap-y-3">{summary.markouts.map((markout) => <div key={markout.horizonNs}>
        <dt className="text-xs text-muted-foreground">{formatDurationNs(markout.horizonNs)} ({markout.horizonNs} ns)</dt>
        <dd className="font-mono text-sm">{markout.availableFillCount === "0" ? "Unavailable" : formatDecimalForDisplay(markout.meanMarkoutPerShare, { signDisplay: "always" })}</dd>
        <dd className="text-xs text-muted-foreground">{markout.availableShares ?? "Unavailable"} eligible shares; {markout.availableFillCount} eligible fills; {markout.unavailableFillCount} no mark; {markout.censoredFillCount} censored</dd>
      </div>)}</dl>}
    </div> : null}
    {!execution && summary.accounting.markUnavailableReason ? <p className="mt-2 text-sm text-destructive">Terminal valuation unavailable: {humanizeEnumValue(summary.accounting.markUnavailableReason)}</p> : null}
  </section>;
}
