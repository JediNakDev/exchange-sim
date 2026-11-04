"use client";

// Thin wrapper over the shadcn chart for simulation-time series. Plot positions
// are approximate floats from chart-derived-floats.ts; tooltips always show the
// API's original decimal strings.

import { CartesianGrid, Line, LineChart, XAxis, YAxis } from "recharts";
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card";
import { ChartContainer, ChartLegend, ChartLegendContent, ChartTooltip, ChartTooltipContent, type ChartConfig } from "@/components/ui/chart";
import { formatDecimalForDisplay, formatSimulationTimeNs } from "@/lib/exact-decimal-format";

export interface SeriesLineDefinition {
  /** Row key holding the plot float. */
  valueKey: string;
  /** Row key holding the exact decimal string shown in the tooltip. */
  textKey: string;
  label: string;
  /** CSS color, normally var(--chart-N). */
  color: string;
}

export type SeriesChartRow = { simulationTimeSeconds: number; bucketEndTimeNs: string } & Record<string, number | string | null>;

export function SeriesLineChart({ title, description, rows, lines }: { title: string; description?: string; rows: SeriesChartRow[]; lines: SeriesLineDefinition[] }) {
  const chartConfig: ChartConfig = Object.fromEntries(lines.map((line) => [line.valueKey, { label: line.label, color: line.color }]));
  return (
    <Card size="sm">
      <CardHeader>
        <CardTitle>{title}</CardTitle>
        {description ? <CardDescription>{description}</CardDescription> : null}
      </CardHeader>
      <CardContent>
        <ChartContainer config={chartConfig} className="aspect-auto h-48 w-full">
          <LineChart data={rows} margin={{ left: 4, right: 8, top: 4, bottom: 0 }}>
            <CartesianGrid vertical={false} />
            <XAxis dataKey="simulationTimeSeconds" type="number" domain={["dataMin", "dataMax"]} tickLine={false} axisLine={false} tickFormatter={(tickSeconds: number) => `${tickSeconds}s`} />
            <YAxis tickLine={false} axisLine={false} width={64} domain={["auto", "auto"]} />
            <ChartTooltip
              content={
                <ChartTooltipContent
                  labelFormatter={(_label, payload) => {
                    const bucketEndTimeNs = payload?.[0]?.payload?.bucketEndTimeNs as string | undefined;
                    return bucketEndTimeNs ? `t = ${formatSimulationTimeNs(bucketEndTimeNs, 3)}` : "";
                  }}
                  formatter={(_value, _name, item) => {
                    const line = lines.find((candidate) => candidate.valueKey === item.dataKey);
                    const exactText = line ? (item.payload?.[line.textKey] as string | null | undefined) : undefined;
                    return (
                      <div className="flex w-full items-center justify-between gap-3">
                        <span className="flex items-center gap-1.5 text-muted-foreground">
                          <span className="h-0.5 w-3 rounded-full" style={{ background: line?.color }} />
                          {line?.label ?? String(item.dataKey)}
                        </span>
                        <span className="font-mono font-medium text-foreground tabular-nums">{exactText ? formatDecimalForDisplay(exactText) : "n/a"}</span>
                      </div>
                    );
                  }}
                />
              }
            />
            {lines.length > 1 ? <ChartLegend content={<ChartLegendContent />} /> : null}
            {lines.map((line) => (
              <Line key={line.valueKey} dataKey={line.valueKey} type="stepAfter" stroke={`var(--color-${line.valueKey})`} strokeWidth={2} dot={false} connectNulls={false} isAnimationActive={false} />
            ))}
          </LineChart>
        </ChartContainer>
      </CardContent>
    </Card>
  );
}
