"use client";

// Paired comparison of two experiments on their shared master seeds. Every
// difference and dispersion statistic comes from the service; this view only
// formats and arranges them.

import Link from "next/link";
import { usePathname, useRouter, useSearchParams } from "next/navigation";
import { useState } from "react";
import { TriangleAlertIcon } from "lucide-react";
import { Alert, AlertDescription, AlertTitle } from "@/components/ui/alert";
import { Badge } from "@/components/ui/badge";
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card";
import { Field, FieldLabel } from "@/components/ui/field";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui/select";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { ApiFailureAlert } from "@/components/research/api-failure-alert";
import { PairedSeedAnalysis } from "@/components/research/paired-seed-analysis";
import { PageHeading } from "@/components/research/page-heading";
import { SeriesLineChart, type SeriesChartRow } from "@/components/research/series-line-chart";
import { readExperiments } from "@/lib/api/research-workflow";
import { useApiResource } from "@/hooks/use-api-resource";
import { getBrowserApiClient, unwrapApiResult, type ApiSchemas } from "@/lib/api/exsim-api-client";
import { buildAlignedChartRows } from "@/lib/chart-derived-floats";
import { decimalTextSign } from "@/lib/exact-decimal-format";
import { formatMetricValue, humanizeEnumValue, metricLabel, METRIC_UNIT_LABEL, SERIES_LABEL_BY_KIND } from "@/lib/research-labels";
import { cn } from "@/lib/utils";

const ALL_SEEDS = "mean";
const CHARTED_SERIES_KINDS: ApiSchemas["SeriesKind"][] = ["equity", "inventory", "drawdown"];

function differenceToneClass(differenceText: string | null, betterDirection: ApiSchemas["MetricComparison"]["betterDirection"]): string {
  if (differenceText === null || betterDirection === "neither") return "";
  const differenceSign = decimalTextSign(differenceText);
  if (differenceSign === 0) return "";
  const candidateIsBetter = betterDirection === "higher" ? differenceSign > 0 : differenceSign < 0;
  return candidateIsBetter ? "text-green-700 dark:text-green-400" : "text-red-700 dark:text-red-400";
}

function ExperimentSelect({ label, value, experiments, onValueChange }: { label: string; value: string | null; experiments: ApiSchemas["Experiment"][]; onValueChange: (experimentId: string) => void }) {
  const selectId = `compare-${label.toLowerCase()}`;
  return (
    <Field className="min-w-64 flex-1">
      <FieldLabel htmlFor={selectId}>{label}</FieldLabel>
      <Select value={value ?? undefined} onValueChange={onValueChange}>
        <SelectTrigger id={selectId} className="w-full">
          <SelectValue placeholder="Choose an experiment" />
        </SelectTrigger>
        <SelectContent>
          {experiments.map((experiment) => (
            <SelectItem key={experiment.experimentId} value={experiment.experimentId}>
              {experiment.name} <span className="font-mono text-xs text-muted-foreground">{experiment.experimentId}</span>
            </SelectItem>
          ))}
        </SelectContent>
      </Select>
    </Field>
  );
}

function MetricComparisonTable({ metricComparisons }: { metricComparisons: ApiSchemas["MetricComparison"][] }) {
  return (
    <div className="overflow-x-auto rounded-md border">
      <Table className="text-xs">
        <TableHeader>
          <TableRow>
            <TableHead>Metric</TableHead>
            <TableHead>Unit</TableHead>
            <TableHead className="text-right">n</TableHead>
            <TableHead className="text-right">Mean diff</TableHead>
            <TableHead className="text-right">Std dev</TableHead>
            <TableHead className="text-right">Std error</TableHead>
            <TableHead className="text-right">Min diff</TableHead>
            <TableHead className="text-right">Max diff</TableHead>
            <TableHead className="text-right">Better / worse / tied</TableHead>
            <TableHead>Per-seed differences (candidate - baseline)</TableHead>
          </TableRow>
        </TableHeader>
        <TableBody>
          {metricComparisons.map((metricComparison) => {
            const dispersion = metricComparison.dispersion;
            return (
              <TableRow key={`${metricComparison.metricKey}:${metricComparison.horizonNs ?? ""}`}>
                <TableCell className="font-medium">
                  {metricLabel(metricComparison.metricKey, metricComparison.horizonNs)}
                  {metricComparison.exactness === "derived_float" ? (
                    <Badge variant="outline" className="ml-1">
                      derived
                    </Badge>
                  ) : null}
                </TableCell>
                <TableCell className="text-muted-foreground">
                  {METRIC_UNIT_LABEL[metricComparison.unit]}
                  {metricComparison.betterDirection !== "neither" ? `, ${metricComparison.betterDirection} is better` : ""}
                </TableCell>
                <TableCell className="text-right font-mono tabular-nums">{dispersion.sampleCount}</TableCell>
                <TableCell className={cn("text-right font-mono font-medium tabular-nums", differenceToneClass(dispersion.meanDifference, metricComparison.betterDirection))}>
                  {formatMetricValue(dispersion.meanDifference, metricComparison.unit, { signDisplay: "always" })}
                </TableCell>
                <TableCell className="text-right font-mono tabular-nums">{formatMetricValue(dispersion.sampleStandardDeviation, metricComparison.unit)}</TableCell>
                <TableCell className="text-right font-mono tabular-nums">{formatMetricValue(dispersion.standardErrorOfMean, metricComparison.unit)}</TableCell>
                <TableCell className="text-right font-mono tabular-nums">{formatMetricValue(dispersion.minimumDifference, metricComparison.unit, { signDisplay: "always" })}</TableCell>
                <TableCell className="text-right font-mono tabular-nums">{formatMetricValue(dispersion.maximumDifference, metricComparison.unit, { signDisplay: "always" })}</TableCell>
                <TableCell className="text-right font-mono tabular-nums">
                  {metricComparison.betterDirection === "neither" ? "Not applicable" : `${dispersion.candidateBetterCount} / ${dispersion.candidateWorseCount} / ${dispersion.tiedCount}`}
                </TableCell>
                <TableCell className="font-mono tabular-nums">
                  <div className="flex flex-wrap gap-x-3">
                    {metricComparison.perSeed.map((pairedSeedMetric) => (
                      <span key={pairedSeedMetric.masterSeed} title={`baseline ${pairedSeedMetric.baselineValue ?? "n/a"}, candidate ${pairedSeedMetric.candidateValue ?? "n/a"}`}>
                        <Link className="text-muted-foreground hover:underline" href={`/runs/${pairedSeedMetric.baselineRunId}/inspect`} title={`Inspect baseline run ${pairedSeedMetric.baselineRunId}`}>
                          {pairedSeedMetric.masterSeed}:
                        </Link>{" "}
                        <Link className={`hover:underline ${differenceToneClass(pairedSeedMetric.difference, metricComparison.betterDirection)}`} href={`/runs/${pairedSeedMetric.candidateRunId}/inspect`} title={`Inspect candidate run ${pairedSeedMetric.candidateRunId}`}>
                          {formatMetricValue(pairedSeedMetric.difference, metricComparison.unit, { signDisplay: "always" })}
                        </Link>
                      </span>
                    ))}
                  </div>
                </TableCell>
              </TableRow>
            );
          })}
        </TableBody>
      </Table>
    </div>
  );
}

export function ExperimentCompareView() {
  const router = useRouter();
  const pathname = usePathname();
  const searchParams = useSearchParams();
  const apiClient = getBrowserApiClient();
  const [chartedSeed, setChartedSeed] = useState(ALL_SEEDS);
  const experimentList = useApiResource("compare-experiments", () => readExperiments(apiClient), null);
  const experiments = experimentList.data?.items ?? [];
  const baselineExperimentId = searchParams.get("baseline") ?? experiments.find((experiment) => experiment.parentExperimentId === null)?.experimentId ?? null;
  const candidateExperimentId = searchParams.get("candidate") ?? experiments.find((experiment) => experiment.parentExperimentId === baselineExperimentId)?.experimentId ?? null;

  const comparisonResource = useApiResource(
    baselineExperimentId && candidateExperimentId ? `comparison:${baselineExperimentId}:${candidateExperimentId}` : null,
    async () => unwrapApiResult(await apiClient.GET("/v1/comparisons", { params: { query: { baselineExperimentId: baselineExperimentId!, candidateExperimentId: candidateExperimentId!, maxPoints: 240 } } })),
    null,
  );
  const comparison = comparisonResource.data;

  const selectExperiment = (role: "baseline" | "candidate", experimentId: string) => {
    const nextSearchParams = new URLSearchParams(searchParams.toString());
    nextSearchParams.set(role, experimentId);
    if (role === "baseline" && !nextSearchParams.has("candidate") && candidateExperimentId) nextSearchParams.set("candidate", candidateExperimentId);
    if (role === "candidate" && !nextSearchParams.has("baseline") && baselineExperimentId) nextSearchParams.set("baseline", baselineExperimentId);
    setChartedSeed(ALL_SEEDS);
    router.replace(`${pathname}?${nextSearchParams.toString()}`);
  };

  return (
    <>
      <PageHeading title="Compare experiments" description="Judge candidate outcomes against a baseline on matched seeds, then inspect the runs behind the difference." />
      {experimentList.failure ? <ApiFailureAlert failure={experimentList.failure} onRetry={experimentList.reload} /> : null}
      <div className="comparison-selectors flex flex-wrap gap-5 rounded-lg border bg-card p-5">
        <ExperimentSelect label="Baseline" value={baselineExperimentId} experiments={experiments} onValueChange={(experimentId) => selectExperiment("baseline", experimentId)} />
        <ExperimentSelect label="Candidate" value={candidateExperimentId} experiments={experiments} onValueChange={(experimentId) => selectExperiment("candidate", experimentId)} />
      </div>
      {!baselineExperimentId || !candidateExperimentId ? (
        <p className="rounded-md border p-4 text-sm text-muted-foreground">
          Choose a baseline and candidate to evaluate their shared completed seeds.
          {baselineExperimentId ? <> <Link className="underline text-foreground" href={`/experiments/new?from=${encodeURIComponent(baselineExperimentId)}`}>Create a candidate from this baseline</Link>.</> : null}
        </p>
      ) : null}
      {comparisonResource.failure ? <ApiFailureAlert failure={comparisonResource.failure} title="Comparison unavailable or stale. Refresh before interpreting results." onRetry={comparisonResource.reload} /> : null}
      {comparisonResource.isLoading ? <p className="text-sm text-muted-foreground">Computing comparison...</p> : null}
      {comparison ? (
        <>
          {!comparison.comparable ? (
            <Alert variant="destructive">
              <TriangleAlertIcon />
              <AlertTitle>These experiments are not comparable as a paired test</AlertTitle>
              <AlertDescription>
                <ul className="list-disc pl-4">
                  {comparison.incomparabilityReasons.map((reason) => (
                    <li key={reason}>{reason}</li>
                  ))}
                </ul>
              </AlertDescription>
            </Alert>
          ) : null}
          <div className="grid gap-4 xl:grid-cols-[minmax(0,1fr)_minmax(0,2fr)]">
            <Card size="sm">
              <CardHeader>
                <CardTitle>Paired seeds</CardTitle>
                <CardDescription>Only reproduced, completed runs enter this paired comparison. Failed, incomplete and unpaired runs are excluded; this can bias the result.</CardDescription>
              </CardHeader>
              <CardContent className="flex flex-col gap-1 text-sm">
                <p>
                  Paired ({comparison.seedPairing.pairedSeeds.length}): <span className="font-mono">{comparison.seedPairing.pairedSeeds.join(", ") || "none"}</span>
                </p>
                <p>
                  Baseline only: <span className="font-mono">{comparison.seedPairing.baselineOnlySeeds.join(", ") || "none"}</span>
                </p>
                <p>
                  Candidate only: <span className="font-mono">{comparison.seedPairing.candidateOnlySeeds.join(", ") || "none"}</span>
                </p>
                <p className="text-xs text-muted-foreground">
                  <Link className="underline" href={`/experiments/${comparison.baseline.experimentId}`}>
                    {comparison.baseline.name}
                  </Link>{" "}
                  vs{" "}
                  <Link className="underline" href={`/experiments/${comparison.candidate.experimentId}`}>
                    {comparison.candidate.name}
                  </Link>
                </p>
              </CardContent>
            </Card>
            <Card size="sm">
              <CardHeader>
                <CardTitle>Assumption differences ({comparison.assumptionDifferences.length})</CardTitle>
                <CardDescription>Everything that differs between the two configurations. If more than the variable under test changed, the comparison mixes effects.</CardDescription>
              </CardHeader>
              <CardContent>
                <Table className="text-xs">
                  <TableHeader>
                    <TableRow>
                      <TableHead>Category</TableHead>
                      <TableHead>Assumption</TableHead>
                      <TableHead>Baseline</TableHead>
                      <TableHead>Candidate</TableHead>
                    </TableRow>
                  </TableHeader>
                  <TableBody>
                    {comparison.assumptionDifferences.map((assumptionDifference) => (
                      <TableRow key={assumptionDifference.fieldPath} className="bg-amber-500/10">
                        <TableCell>{humanizeEnumValue(assumptionDifference.category)}</TableCell>
                        <TableCell>{assumptionDifference.label}</TableCell>
                        <TableCell className="font-mono">
                          {assumptionDifference.baselineValue} {assumptionDifference.unit ?? ""}
                        </TableCell>
                        <TableCell className="font-mono font-medium">
                          {assumptionDifference.candidateValue} {assumptionDifference.unit ?? ""}
                        </TableCell>
                      </TableRow>
                    ))}
                    {comparison.assumptionDifferences.length === 0 ? (
                      <TableRow>
                        <TableCell colSpan={4} className="text-muted-foreground">
                          Identical configurations.
                        </TableCell>
                      </TableRow>
                    ) : null}
                  </TableBody>
                </Table>
              </CardContent>
            </Card>
          </div>

          <PairedSeedAnalysis key={`${comparison.baseline.experimentId}:${comparison.candidate.experimentId}`} comparison={comparison} />

          {comparison.comparable ? <>
          <div className="flex flex-wrap items-end gap-3">
            <Field className="w-64">
              <FieldLabel htmlFor="charted-seed">Charted runs</FieldLabel>
              <Select value={chartedSeed} onValueChange={setChartedSeed}>
                <SelectTrigger id="charted-seed" className="w-full">
                  <SelectValue />
                </SelectTrigger>
                <SelectContent>
                  <SelectItem value={ALL_SEEDS}>Mean across paired seeds (derived)</SelectItem>
                  {comparison.seedPairing.pairedSeeds.map((masterSeed) => (
                    <SelectItem key={masterSeed} value={masterSeed}>
                      Seed {masterSeed} (recorded values)
                    </SelectItem>
                  ))}
                </SelectContent>
              </Select>
            </Field>
            <p className="pb-2 text-xs text-muted-foreground">All charts share one simulated-time axis. Gaps mean a run had ended or not yet started.</p>
          </div>
          <div className="grid gap-4 lg:grid-cols-3">
            {CHARTED_SERIES_KINDS.map((seriesKind) => {
              const alignedSeries = comparison.alignedSeries.find((candidateSeries) => candidateSeries.seriesKind === seriesKind);
              if (!alignedSeries) return null;
              return (
                <SeriesLineChart
                  key={seriesKind}
                  title={`${SERIES_LABEL_BY_KIND[seriesKind]} (${METRIC_UNIT_LABEL[alignedSeries.unit]})`}
                  description={chartedSeed === ALL_SEEDS ? "Mean across paired seeds" : `Seed ${chartedSeed}`}
                  rows={buildAlignedChartRows(alignedSeries, chartedSeed === ALL_SEEDS ? null : chartedSeed) as SeriesChartRow[]}
                  lines={[
                    { valueKey: "baselineValue", textKey: "baselineValueText", label: "Baseline", color: "var(--chart-1)" },
                    { valueKey: "candidateValue", textKey: "candidateValueText", label: "Candidate", color: "var(--chart-2)" },
                  ]}
                />
              );
            })}
          </div>

          <Card size="sm">
            <CardHeader>
              <CardTitle>Metric differences</CardTitle>
              <CardDescription>
                Mean, sample standard deviation (n - 1) and standard error of the paired differences. With few seeds, read the per-seed values and the better / worse counts, not just the mean.
              </CardDescription>
            </CardHeader>
            <CardContent>
              <MetricComparisonTable metricComparisons={comparison.metricComparisons} />
            </CardContent>
          </Card>
          </> : null}
        </>
      ) : null}
    </>
  );
}
