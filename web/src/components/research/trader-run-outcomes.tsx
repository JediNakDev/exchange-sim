import { KeyFigureList } from "@/components/research/key-figure-list";
import { SignedFigure } from "@/components/research/signed-figure";
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
    ["Ending position (shares)", formatDecimalForDisplay(summary.accounting.positionShares)],
    ["Max long / short (shares)", `${formatIntegerText(summary.risk.maxLongPositionShares)} / ${formatIntegerText(summary.risk.maxShortPositionShares)}`],
    ["Max drawdown (USD)", formatDecimalForDisplay(summary.risk.maxDrawdown)],
    ...(summary.markouts[0] ? [[`Measured shares at ${formatDurationNs(summary.markouts[0].horizonNs)}`, summary.markouts[0].availableShares ?? "Unavailable"] as [string, string]] : []),
  ];
  return <section className="flex min-w-0 flex-col gap-3" aria-label="Run outcomes">
    <div className="flex flex-wrap items-baseline justify-between gap-2">
      <h2 className="text-base font-medium">{execution ? "Completion and execution cost" : "P&L and inventory risk"}</h2>
      <p className="text-xs text-muted-foreground">{summary.availability} summary at event {summary.asOfEventPosition}</p>
    </div>
    <KeyFigureList figures={facts.map(([label, value]) => ({
      label,
      value: <span title={value} className="flex flex-wrap gap-x-1">{label === "Realized / unrealized P&L (USD)" ? <><SignedFigure formattedValue={formatDecimalForDisplay(summary.accounting.grossRealizedPnl, { signDisplay: "always" })} favourableDirection="higher" /> / <SignedFigure formattedValue={formatDecimalForDisplay(summary.accounting.unrealizedPnl, { signDisplay: "always" })} favourableDirection="higher" /></> : <SignedFigure formattedValue={value} favourableDirection={label.includes("position") || label.includes("long / short") || label.includes("shares") || label.includes("ratio") ? "neutral" : label.includes("drawdown") || execution ? "lower" : "higher"} />}</span>,
    }))} />
    <p className="mt-4 max-w-prose text-xs leading-relaxed text-muted-foreground">{execution
      ? "Positive signed costs are worse for buys and sells. Shortfall includes net fees; slippage excludes fees. Both cover filled quantity only. Residual opportunity cost is unavailable, so lower cost with lower completion is not an unconditional improvement."
      : "Net P&L includes inventory valuation and fees. Spread capture alone is not profit. Read signed markouts with eligible exposure alongside inventory and drawdown; markouts include the initial spread edge."}</p>
    {!execution ? <p className="max-w-prose text-xs text-muted-foreground">Measured coverage: {summary.markouts.slice(0, 1).map((m) => <span key={m.horizonNs} className="mr-3 inline-block">{formatDurationNs(m.horizonNs)}: {m.availableShares ?? "Unavailable"} eligible shares, {m.censoredFillCount} censored fills, {m.unavailableFillCount} no mark.</span>)} Means weight eligible shares; no-mark and censored fills are excluded. Other horizons are shown with the markout chart.</p> : null}
    {!execution && summary.accounting.markUnavailableReason ? <p className="mt-2 text-sm text-destructive">Terminal valuation unavailable: {humanizeEnumValue(summary.accounting.markUnavailableReason)}</p> : null}
  </section>;
}
