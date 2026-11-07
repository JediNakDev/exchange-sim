// Human labels and unit-aware display for contract enums. Formatting stays exact:
// values are decimal strings and are only rounded for display, half-to-even.

import type { ApiSchemas } from "@/lib/api/exsim-api-client";
import { formatDecimalForDisplay, formatDurationNs, type DecimalDisplayOptions } from "@/lib/exact-decimal-format";

export const METRIC_LABEL_BY_KEY: Record<ApiSchemas["MetricKey"], string> = {
  completion_ratio: "Parent completion",
  residual_qty: "Parent residual quantity",
  decision_slippage_total: "Decision slippage, executed quantity",
  decision_slippage_per_share: "Decision slippage per filled share",
  implementation_shortfall_total: "Executed cost including fees",
  implementation_shortfall_per_share: "Executed cost per filled share",
  participation_rate: "Participation through last parent fill",
  net_pnl: "Net P&L",
  equity: "Ending equity",
  gross_realized_pnl: "Gross realized P&L",
  unrealized_pnl: "Unrealized P&L",
  net_fees: "Net fees paid",
  ending_position: "Ending position",
  max_long_position: "Max long position",
  max_short_position: "Max short position",
  max_drawdown: "Max drawdown",
  fill_count: "Fills",
  filled_shares: "Filled shares",
  maker_fill_count: "Maker fills",
  taker_fill_count: "Taker fills",
  fill_rate: "Fill rate (shares)",
  submitted_order_count: "Orders submitted",
  canceled_order_count: "Orders canceled",
  cancel_race_lost_count: "Cancel races lost",
  spread_capture_total: "Spread capture",
  markout_total: "Markout total",
  markout_per_share: "Markout per eligible share",
  new_order_rejected_count: "New-order rejections",
  cancel_request_count: "Cancel requests",
  cancel_rejected_count: "Cancel rejections",
  replace_request_count: "Replace requests",
  replace_rejected_count: "Replace rejections",
  markout_eligible_shares: "Markout eligible shares",
  markout_eligible_fills: "Markout eligible fills",
  markout_no_mark_fills: "Markout fills without a mark",
  markout_censored_fills: "Markout censored fills",

  quote_exposure_time: "Quote exposure time",
};

export const METRIC_UNIT_LABEL: Record<ApiSchemas["MetricUnit"], string> = {
  currency: "USD",
  currency_per_share: "USD/share",
  shares: "shares",
  count: "count",
  ratio: "ratio",
  nanoseconds: "time",
};

export function metricLabel(metricKey: ApiSchemas["MetricKey"], horizonNs: string | null): string {
  return horizonNs === null ? METRIC_LABEL_BY_KEY[metricKey] : `${METRIC_LABEL_BY_KEY[metricKey]} @ ${formatDurationNs(horizonNs)}`;
}

/** Formats a metric value in its unit; durations get a time unit, everything else keeps its digits. */
export function formatMetricValue(valueText: string | null, metricUnit: ApiSchemas["MetricUnit"], options: DecimalDisplayOptions = {}): string {
  if (valueText === null) return "n/a";
  if (metricUnit === "nanoseconds") {
    const durationText = formatDurationNs(valueText.replace(/^[-+]/, ""));
    return valueText.startsWith("-") ? `-${durationText}` : options.signDisplay === "always" && valueText !== "0" ? `+${durationText}` : durationText;
  }
  return formatDecimalForDisplay(valueText, options);
}

export const SERIES_LABEL_BY_KIND: Record<ApiSchemas["SeriesKind"], string> = {
  equity: "Equity",
  inventory: "Inventory",
  exchange_mid: "Exchange mid",
  quoted_spread: "Quoted spread",
  drawdown: "Drawdown",
};

export function humanizeEnumValue(enumValue: string): string {
  return enumValue.replaceAll("_", " ");
}
