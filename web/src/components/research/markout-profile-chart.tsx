"use client";
import { Bar, BarChart, CartesianGrid, XAxis, YAxis, ReferenceLine } from "recharts";
import { ChartContainer, ChartTooltip, ChartTooltipContent } from "@/components/ui/chart";
import { Card, CardHeader, CardTitle, CardDescription, CardContent } from "@/components/ui/card";
import { Collapsible, CollapsibleContent, CollapsibleTrigger } from "@/components/ui/collapsible";
import { Button } from "@/components/ui/button";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { SignedFigure } from "@/components/research/signed-figure";
import type { ApiSchemas } from "@/lib/api/exsim-api-client";
import { decimalTextToChartFloat } from "@/lib/chart-derived-floats";
import { formatDecimalForDisplay, formatDurationNs } from "@/lib/exact-decimal-format";
export function MarkoutProfileChart({ markouts }: { markouts: ApiSchemas["RunSummary"]["markouts"] }) {
  const rows = markouts.map((m) => ({ ...m, horizon: formatDurationNs(m.horizonNs), value: m.availableFillCount === "0" ? null : decimalTextToChartFloat(m.meanMarkoutPerShare) }));
  return <Card size="sm" className="min-w-0"><CardHeader><CardTitle>Post-fill quality by horizon (USD/share)</CardTitle><CardDescription>Eligible-share weighted signed markout against exchange mid, including initial spread edge. Bars represent configured horizons only. Unmeasured observations are absent, not zero.</CardDescription></CardHeader><CardContent>
    {!rows.some((row) => row.value !== null) ? <p role="status" className="mb-4 text-sm text-muted-foreground">No measured markouts at these horizons. Read censored and unavailable counts in the exact values below; there is no zero-performance observation.</p> : null}
    <ChartContainer config={{ value: { label: "Signed markout USD/share", color: "var(--chart-1)" } }} className="h-52 w-full aspect-auto"><BarChart data={rows} accessibilityLayer aria-label="Post-fill quality by horizon (USD/share)"><CartesianGrid vertical={false} stroke="var(--border)" /><XAxis dataKey="horizon" tickLine={false} axisLine={false} tick={{ fontSize: 10, fill: "var(--muted-foreground)" }} /><YAxis tickLine={false} axisLine={false} width={72} tick={{ fontSize: 10, fill: "var(--muted-foreground)" }} /><ReferenceLine y={0} stroke="var(--foreground)" /><ChartTooltip content={<ChartTooltipContent formatter={(_value, _name, item) => <span className="font-mono">{item.payload.meanMarkoutPerShare} USD/share, {item.payload.availableShares ?? "unknown"} eligible shares</span>} />} /><Bar dataKey="value" fill="var(--color-value)" isAnimationActive={false} /></BarChart></ChartContainer>
    <Collapsible className="mt-3"><CollapsibleTrigger asChild><Button variant="ghost" size="sm" className="h-auto max-w-full whitespace-normal">Read sampled values for post-fill quality</Button></CollapsibleTrigger><CollapsibleContent><Table aria-label="Post-fill quality exact values"><TableHeader><TableRow><TableHead>Horizon</TableHead><TableHead>USD/share</TableHead><TableHead>Eligible shares / fills</TableHead><TableHead>No mark / censored fills</TableHead></TableRow></TableHeader><TableBody>{markouts.map((m) => <TableRow key={m.horizonNs}><TableCell className="font-mono">{formatDurationNs(m.horizonNs)}</TableCell><TableCell className="font-mono">{m.availableFillCount === "0" ? "Unavailable" : <SignedFigure formattedValue={formatDecimalForDisplay(m.meanMarkoutPerShare, { signDisplay: "always" })} favourableDirection="higher" />}</TableCell><TableCell className="font-mono">{m.availableShares ?? "Unavailable"} / {m.availableFillCount}</TableCell><TableCell className="font-mono">{m.unavailableFillCount} / {m.censoredFillCount}</TableCell></TableRow>)}</TableBody></Table></CollapsibleContent></Collapsible>
  </CardContent></Card>;
}
