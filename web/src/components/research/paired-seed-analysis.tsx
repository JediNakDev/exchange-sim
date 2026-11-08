"use client";

import Link from "next/link";
import { useId, useState } from "react";
import { Bar, BarChart, CartesianGrid, ReferenceLine, XAxis, YAxis } from "recharts";
import { CheckIcon, EqualIcon, XIcon } from "lucide-react";
import { Badge } from "@/components/ui/badge";
import { SignedFigure } from "@/components/research/signed-figure";
import { Field, FieldLabel } from "@/components/ui/field";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui/select";
import { ChartContainer, ChartTooltip, ChartTooltipContent } from "@/components/ui/chart";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import type { ApiSchemas } from "@/lib/api/exsim-api-client";
import { decimalTextToChartFloat } from "@/lib/chart-derived-floats";
import { formatMetricValue, metricLabel, METRIC_UNIT_LABEL } from "@/lib/research-labels";
import { interpretPairedDifference, priorityComparisonMetrics } from "@/lib/trader-analytics";

const metricId = (metric: ApiSchemas["MetricComparison"]) => `${metric.metricKey}:${metric.horizonNs ?? ""}`;

export function PairedSeedAnalysis({ comparison }: { comparison: ApiSchemas["ExperimentComparison"] }) {
  const chartDescriptionId = useId();
  const [selectedId, setSelectedId] = useState<string | null>(null);
  const priorities = priorityComparisonMetrics(comparison.metricComparisons);
  const metric = comparison.metricComparisons.find((item) => metricId(item) === selectedId) ?? priorities.find((item) => item.dispersion.sampleCount > 0) ?? priorities[0] ?? comparison.metricComparisons[0];
  if (!comparison.comparable || !metric) return null;
  const rows = metric.perSeed.map((pair) => ({ ...pair, differencePosition: decimalTextToChartFloat(pair.difference) }));
  return (
    <section className="flex min-w-0 flex-col gap-3" aria-labelledby={`${chartDescriptionId}-heading`}>
      <div><h2 id={`${chartDescriptionId}-heading`} className="text-base font-medium">Candidate impact across matched seeds</h2>
      <p className="mt-1 max-w-prose text-xs/relaxed text-muted-foreground">Candidate minus baseline. Costs and completion must be judged together. Synthetic seed results do not establish real-market profitability.</p></div>
      <div className="space-y-5">
        {comparison.metricComparisons.some((item) => item.metricKey === "completion_ratio" && item.dispersion.sampleCount > 0) ? <section className="border bg-muted/30 p-3 text-sm" aria-label="Execution comparison definitions">
          <h3 className="font-medium">Read costs with completion</h3>
          <p className="mt-1">Costs use the exchange mid at parent declaration, not each child’s arrival mid. Positive signed costs are worse for buys and sells. Decision slippage excludes fees; executed cost including fees adds net fees.</p>
          <p className="mt-2">Costs cover filled shares only, in USD or USD/share as labeled. Unexecuted opportunity cost is unavailable. A lower cost on fewer filled shares is not an unconditional improvement.</p>
          <p className="mt-2">Participation divides parent fills by all market-traded shares from declaration through the last parent fill, including own trades. Different last-fill times mean different volume horizons.</p>
        </section> : null}
        <div className="grid gap-4 md:grid-cols-2 xl:grid-cols-3">
          {priorities.map((item) => <div key={metricId(item)} className="min-w-0 border bg-card p-4 text-xs">
            <p className="text-muted-foreground">{metricLabel(item.metricKey, item.horizonNs)}</p>
            <p className="font-mono text-lg tabular-nums"><SignedFigure formattedValue={formatMetricValue(item.dispersion.meanDifference, item.unit, { signDisplay: "always" })} favourableDirection={item.betterDirection === "neither" ? "neutral" : item.betterDirection} /> <span className="text-xs">{METRIC_UNIT_LABEL[item.unit]}</span></p>
            <p>{interpretPairedDifference(item)}</p>
            <p className="mt-2 text-xs text-muted-foreground">n = {item.dispersion.sampleCount}; {item.betterDirection === "neither" ? "preference counts not applicable" : <span className="inline-flex flex-wrap gap-1">better / worse / tied: <Badge variant="outline"><CheckIcon aria-hidden="true" />{item.dispersion.candidateBetterCount}</Badge> / <Badge variant="outline"><XIcon aria-hidden="true" />{item.dispersion.candidateWorseCount}</Badge> / <Badge variant="outline"><EqualIcon aria-hidden="true" />{item.dispersion.tiedCount}</Badge></span>}</p>
          </div>)}
        </div>
        <Field className="max-w-md">
          <FieldLabel htmlFor="paired-metric">Paired difference to inspect</FieldLabel>
          <Select value={metricId(metric)} onValueChange={setSelectedId}>
            <SelectTrigger id="paired-metric" className="w-full"><SelectValue /></SelectTrigger>
            <SelectContent>{comparison.metricComparisons.map((item) => <SelectItem key={metricId(item)} value={metricId(item)}>{metricLabel(item.metricKey, item.horizonNs)}</SelectItem>)}</SelectContent>
          </Select>
        </Field>
        <p id={chartDescriptionId} className="text-sm">{interpretPairedDifference(metric)}. {metric.betterDirection === "neither" ? "No preferred sign." : `${metric.betterDirection === "lower" ? "Negative" : "Positive"} differences favor the candidate.`} Available pairs: {metric.dispersion.sampleCount} of {comparison.seedPairing.pairedSeeds.length}. {metric.dispersion.sampleCount < 2 ? "Dispersion and standard error require at least two observations." : "Read individual differences alongside the mean; standard error is not a confidence interval."}</p>
        {metric.dispersion.sampleCount > 0 ? <ChartContainer config={{ differencePosition: { label: "Candidate minus baseline", color: "var(--chart-1)" } }} className="h-56 w-full aspect-auto">
          <BarChart aria-label={`Paired difference: ${metricLabel(metric.metricKey, metric.horizonNs)} (${METRIC_UNIT_LABEL[metric.unit]})`} aria-describedby={chartDescriptionId} data={rows} accessibilityLayer margin={{ left: 12, right: 12, bottom: 16 }}>
            <CartesianGrid vertical={false} />
            <XAxis dataKey="masterSeed" tickLine={false} label={{ value: "Master seed", position: "bottom", offset: 0 }} />
            <YAxis width={80} tickLine={false} label={{ value: METRIC_UNIT_LABEL[metric.unit], angle: -90, position: "insideLeft" }} />
            <ReferenceLine y={0} stroke="var(--foreground)" />
            <ChartTooltip content={<ChartTooltipContent labelFormatter={(_label, payload) => `Seed ${payload?.[0]?.payload?.masterSeed ?? ""}`} formatter={(_value, _name, item) => <span className="font-mono">{formatMetricValue(item.payload.difference, metric.unit, { signDisplay: "always" })} {METRIC_UNIT_LABEL[metric.unit]}</span>} />} />
            <Bar dataKey="differencePosition" fill="var(--color-differencePosition)" isAnimationActive={false} />
          </BarChart>
        </ChartContainer> : <p className="border p-4 text-sm text-muted-foreground">No available paired values for this metric. Missing observations are excluded, never plotted as zero.</p>}
        <p className="text-xs text-muted-foreground">Plot positions are approximate. The table retains service values ({metric.exactness === "derived_float" ? "derived" : "exact"}); missing values are unavailable. Zero means tied, not missing. Open either run to inspect the outcome.</p>
        <div className="max-h-72 overflow-auto border bg-card">
          <Table className="text-xs" aria-label="Paired differences by seed"><TableHeader><TableRow><TableHead>Seed</TableHead><TableHead className="text-right">Baseline</TableHead><TableHead className="text-right">Candidate</TableHead><TableHead className="text-right">Difference ({METRIC_UNIT_LABEL[metric.unit]})</TableHead></TableRow></TableHeader>
            <TableBody>{metric.perSeed.map((pair) => <TableRow key={pair.masterSeed}>
              <TableCell className="text-right font-mono tabular-nums">{pair.masterSeed}</TableCell>
              <TableCell className="text-right font-mono tabular-nums"><Link className="underline font-mono" href={`/runs/${pair.baselineRunId}?${new URLSearchParams({ baseline: comparison.baseline.experimentId, candidate: comparison.candidate.experimentId, ...(comparison.runSelection ? { runSelection: comparison.runSelection } : {}) }).toString()}`}>{pair.baselineValue === null ? "Unavailable" : formatMetricValue(pair.baselineValue, metric.unit)}</Link></TableCell>
              <TableCell className="text-right font-mono tabular-nums"><Link className="underline font-mono" href={`/runs/${pair.candidateRunId}?${new URLSearchParams({ baseline: comparison.baseline.experimentId, candidate: comparison.candidate.experimentId, ...(comparison.runSelection ? { runSelection: comparison.runSelection } : {}) }).toString()}`}>{pair.candidateValue === null ? "Unavailable" : formatMetricValue(pair.candidateValue, metric.unit)}</Link></TableCell>
              <TableCell className="text-right font-mono tabular-nums">{pair.difference === null ? "Unavailable" : <SignedFigure formattedValue={formatMetricValue(pair.difference, metric.unit, { signDisplay: "always" })} favourableDirection={metric.betterDirection === "neither" ? "neutral" : metric.betterDirection} />}</TableCell>
            </TableRow>)}</TableBody>
          </Table>
        </div>
      </div>
    </section>
  );
}
