"use client";

// Paired comparison of two experiments on their shared master seeds. Every
// difference and dispersion statistic comes from the service; this view only
// formats and arranges them.

import Link from "next/link";
import { usePathname, useRouter, useSearchParams } from "next/navigation";
import { useState } from "react";
import { CheckIcon, EqualIcon, GitCompareArrowsIcon, TriangleAlertIcon, XIcon } from "lucide-react";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs";
import { ExecutionTradeoffChart } from "@/components/research/execution-tradeoff-chart";
import { ComparisonLibrary } from "@/components/research/comparison-library";
import { ComparisonFinding } from "@/components/research/comparison-finding";
import { Button } from "@/components/ui/button";
import { ComparisonAuditEvidence } from "@/components/research/comparison-audit-evidence";
import { Alert, AlertDescription, AlertTitle } from "@/components/ui/alert";
import { Badge } from "@/components/ui/badge";
import { ResearchSection } from "@/components/research/research-section";
import { KeyFigureList } from "@/components/research/key-figure-list";
import { SignedFigure } from "@/components/research/signed-figure";
import { Empty, EmptyContent, EmptyDescription, EmptyHeader, EmptyMedia, EmptyTitle } from "@/components/ui/empty";
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
import { formatMetricValue, humanizeEnumValue, metricLabel, METRIC_UNIT_LABEL, SERIES_LABEL_BY_KIND } from "@/lib/research-labels";

const ALL_SEEDS = "mean";
const CHARTED_SERIES_KINDS: ApiSchemas["SeriesKind"][] = ["equity", "inventory", "drawdown"];

function ExperimentSelect({ label, value, experiments, onValueChange }: { label: string; value: string | null; experiments: ApiSchemas["Experiment"][]; onValueChange: (experimentId: string) => void }) {
  const selectId = `compare-${label.toLowerCase()}`;
  return (
    <Field className="min-w-0 w-full sm:w-auto sm:flex-1">
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
    <div className="min-w-0 border bg-card">
      <Table className="text-xs" aria-label="Metric differences">
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
                <TableCell className="text-right font-mono tabular-nums">
                  <SignedFigure formattedValue={formatMetricValue(dispersion.meanDifference, metricComparison.unit, { signDisplay: "always" })} favourableDirection={metricComparison.betterDirection === "neither" ? "neutral" : metricComparison.betterDirection} />
                </TableCell>
                <TableCell className="text-right font-mono tabular-nums">{formatMetricValue(dispersion.sampleStandardDeviation, metricComparison.unit)}</TableCell>
                <TableCell className="text-right font-mono tabular-nums">{formatMetricValue(dispersion.standardErrorOfMean, metricComparison.unit)}</TableCell>
                <TableCell className="text-right font-mono tabular-nums"><SignedFigure formattedValue={formatMetricValue(dispersion.minimumDifference, metricComparison.unit, { signDisplay: "always" })} favourableDirection={metricComparison.betterDirection === "neither" ? "neutral" : metricComparison.betterDirection} /></TableCell>
                <TableCell className="text-right font-mono tabular-nums"><SignedFigure formattedValue={formatMetricValue(dispersion.maximumDifference, metricComparison.unit, { signDisplay: "always" })} favourableDirection={metricComparison.betterDirection === "neither" ? "neutral" : metricComparison.betterDirection} /></TableCell>
                <TableCell className="text-right font-mono tabular-nums">
                  {metricComparison.betterDirection === "neither" ? "Not applicable" : <span className="inline-flex gap-1"><Badge variant="outline"><CheckIcon aria-hidden="true" />{dispersion.candidateBetterCount}</Badge><span> / </span><Badge variant="outline"><XIcon aria-hidden="true" />{dispersion.candidateWorseCount}</Badge><span> / </span><Badge variant="outline"><EqualIcon aria-hidden="true" />{dispersion.tiedCount}</Badge></span>}
                </TableCell>
                <TableCell className="font-mono tabular-nums">
                  <div className="flex flex-wrap gap-x-3">
                    {metricComparison.perSeed.map((pairedSeedMetric) => (
                      <span key={pairedSeedMetric.masterSeed} title={`baseline ${pairedSeedMetric.baselineValue ?? "n/a"}, candidate ${pairedSeedMetric.candidateValue ?? "n/a"}`}>
                        <Link className="text-muted-foreground hover:underline" href={`/runs/${pairedSeedMetric.baselineRunId}/inspect`} title={`Inspect baseline run ${pairedSeedMetric.baselineRunId}`}>
                          {pairedSeedMetric.masterSeed}:
                        </Link>{" "}
                        <Link className="hover:underline" href={`/runs/${pairedSeedMetric.candidateRunId}/inspect`} title={`Inspect candidate run ${pairedSeedMetric.candidateRunId}`}>
                          <SignedFigure formattedValue={formatMetricValue(pairedSeedMetric.difference, metricComparison.unit, { signDisplay: "always" })} favourableDirection={metricComparison.betterDirection === "neither" ? "neutral" : metricComparison.betterDirection} />
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
  const [findingDrafts, setFindingDrafts] = useState<Record<string, string>>({});
  const chartedSeed = searchParams.get("seed") ?? ALL_SEEDS;
  const setChartedSeed = (seed: string) => { const params = new URLSearchParams(searchParams); params.set("seed", seed); router.replace(`${pathname}?${params}`, { scroll: false }); };
  const experimentList = useApiResource("compare-experiments", () => readExperiments(apiClient), null);
  const experiments = experimentList.data?.items ?? [];
  const baselineExperimentId = searchParams.get("baseline") ?? null;
  const candidateExperimentId = searchParams.get("candidate") ?? (baselineExperimentId ? experiments.find((experiment) => experiment.parentExperimentId === baselineExperimentId)?.experimentId : null) ?? null;

  const runSelection = searchParams.get("runSelection");
  const activeTab = ["outcomes", "paths", "evidence", "finding"].includes(searchParams.get("tab") ?? "") ? searchParams.get("tab")! : "outcomes";
  const changeTab = (tab: string) => { const params = new URLSearchParams(searchParams); params.set("tab", tab); router.replace(`${pathname}?${params}`, { scroll: false }); };
  const comparisonResource = useApiResource(
    baselineExperimentId && candidateExperimentId ? `comparison:${baselineExperimentId}:${candidateExperimentId}:${runSelection ?? "latest"}` : null,
    async () => unwrapApiResult(await apiClient.GET("/v1/comparisons", { params: { query: { baselineExperimentId: baselineExperimentId!, candidateExperimentId: candidateExperimentId!, ...(runSelection ? { runSelection } : {}), maxPoints: 240 } } })),
    null,
  );
  const comparison = comparisonResource.data;

  const selectExperiment = (role: "baseline" | "candidate", experimentId: string) => {
    const nextSearchParams = new URLSearchParams(searchParams.toString());
    nextSearchParams.set(role, experimentId);
    nextSearchParams.delete("runSelection");
    if (role === "baseline" && !nextSearchParams.has("candidate") && candidateExperimentId) nextSearchParams.set("candidate", candidateExperimentId);
    if (role === "candidate" && !nextSearchParams.has("baseline") && baselineExperimentId) nextSearchParams.set("baseline", baselineExperimentId);
    nextSearchParams.delete("seed");
    router.replace(`${pathname}?${nextSearchParams.toString()}`);
  };

  if (!baselineExperimentId && !candidateExperimentId) return <>
    <PageHeading title="Compare" description="Revisit a study treatment, reopen pinned evidence or evaluate configurations across studies." />
    {experimentList.failure ? <ApiFailureAlert failure={experimentList.failure} onRetry={experimentList.reload} /> : experimentList.isLoading ? <p role="status">Loading study comparisons…</p> : <ComparisonLibrary experiments={experiments} />}
  </>;

  return (
    <>
      <PageHeading title="Compare experiments" description="Judge candidate outcomes against a baseline on matched seeds, then inspect the runs behind the difference." />
      {experimentList.failure ? <ApiFailureAlert failure={experimentList.failure} onRetry={experimentList.reload} /> : null}
      <div className="flex flex-wrap gap-5 border bg-card p-5">
        <ExperimentSelect label="Baseline" value={baselineExperimentId} experiments={experiments} onValueChange={(experimentId) => selectExperiment("baseline", experimentId)} />
        <ExperimentSelect label="Candidate" value={candidateExperimentId} experiments={experiments} onValueChange={(experimentId) => selectExperiment("candidate", experimentId)} />
      </div>
      {!baselineExperimentId || !candidateExperimentId ? (
        <Empty className="border bg-card">
          <EmptyHeader><EmptyMedia variant="icon"><GitCompareArrowsIcon /></EmptyMedia><EmptyTitle>Choose experiments to compare</EmptyTitle><EmptyDescription>Choose a baseline and candidate to evaluate their shared completed seeds.</EmptyDescription></EmptyHeader>
          <EmptyContent><Button asChild size="sm"><Link href={baselineExperimentId ? `/experiments/new?from=${encodeURIComponent(baselineExperimentId)}` : "/experiments"}>{baselineExperimentId ? "Create a candidate from this baseline" : "Browse experiments"}</Link></Button></EmptyContent>
        </Empty>
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
          <KeyFigureList aria-label="Comparison overview" figures={[
            { label: "Paired seeds", value: comparison.seedPairing.pairedSeeds.length },
            { label: "Baseline only", value: comparison.seedPairing.baselineOnlySeeds.length },
            { label: "Candidate only", value: comparison.seedPairing.candidateOnlySeeds.length },
            { label: "Assumption differences", value: comparison.assumptionDifferences.length },
          ]} />


          {comparison.seedPairing.pairedSeeds.length === 0 ? <Empty className="border bg-card"><EmptyHeader><EmptyMedia variant="icon"><GitCompareArrowsIcon /></EmptyMedia><EmptyTitle>No matched seeds to compare</EmptyTitle><EmptyDescription>Choose experiments with shared completed seeds to evaluate paired outcomes.</EmptyDescription></EmptyHeader><EmptyContent><Button asChild size="sm"><Link href={`/experiments/${comparison.candidate.experimentId}`}>Open candidate experiment</Link></Button></EmptyContent></Empty> : null}

          {comparison.runSelection ? <div className="flex flex-wrap items-center gap-3 border bg-card p-3">
            <Button size="sm" variant="outline" onClick={() => { const params = new URLSearchParams(searchParams); params.set("baseline", comparison.baseline.experimentId); params.set("candidate", comparison.candidate.experimentId); params.set("runSelection", comparison.runSelection!); router.replace(`${pathname}?${params}`); }}>Pin selected attempts</Button>
            <Button size="sm" variant="outline" onClick={() => { const receipt = { format: "exsim.comparison-receipt/1", query: { baselineExperimentId: comparison.baseline.experimentId, candidateExperimentId: comparison.candidate.experimentId, runSelection: comparison.runSelection }, comparison }; const url = URL.createObjectURL(new Blob([JSON.stringify(receipt, null, 2)], { type: "application/json" })); const link = document.createElement("a"); link.href = url; link.download = "comparison-receipt.json"; link.click(); setTimeout(() => URL.revokeObjectURL(url), 1000); }}>Download comparison receipt</Button>
            <p className="text-xs text-muted-foreground">{runSelection ? "Cohort pinned in this URL." : "This URL follows the latest eligible attempts until pinned."} Receipt records selected runs and artifact digests; later attempts remain visible in the ledger.</p>
          </div> : null}
          <Tabs value={activeTab} onValueChange={changeTab}>
            <TabsList className="h-auto max-w-full flex-wrap justify-start" aria-label="Comparison sections"><TabsTrigger value="outcomes">Trade-off and paired results</TabsTrigger><TabsTrigger value="paths">Paths over time</TabsTrigger><TabsTrigger value="evidence">Assumptions and eligibility</TabsTrigger><TabsTrigger value="finding">Record finding</TabsTrigger></TabsList>
            <TabsContent value="outcomes" className="space-y-6">
              <p className="max-w-prose text-sm text-muted-foreground">Candidate minus baseline on shared eligible seeds. Read absolute quantities, risk and observation coverage before preferring a treatment. {comparison.assumptionDifferences.length > 1 ? `${comparison.assumptionDifferences.length} changed fields: review assumptions to distinguish a single treatment from confounded effects.` : ""}</p>
          <ExecutionTradeoffChart comparison={comparison} />
          <PairedSeedAnalysis key={`${comparison.baseline.experimentId}:${comparison.candidate.experimentId}`} comparison={comparison} />
              <ComparisonAuditEvidence comparison={comparison} />
            </TabsContent>
            <TabsContent value="evidence" className="space-y-8">
          <div className="grid min-w-0 gap-8">
            <ResearchSection title={<>Paired seeds</>} description={<>Only reproduced, completed runs enter this paired comparison. Failed, incomplete and unpaired runs are excluded; this can bias the result.</>}>
<div className="flex flex-col gap-1 text-sm">
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
              </div>
            </ResearchSection>
            <ResearchSection title={<>Assumption differences ({comparison.assumptionDifferences.length})</>} description={<>Everything that differs between the two configurations. If more than the variable under test changed, the comparison mixes effects.</>}>
<div className="min-w-0 border bg-card">
                <Table className="text-xs" aria-label="Assumption differences">
                  <TableHeader>
                    <TableRow>
                      <TableHead>Category</TableHead>
                      <TableHead>Assumption</TableHead>
                      <TableHead className="text-right">Baseline</TableHead>
                      <TableHead className="text-right">Candidate</TableHead>
                    </TableRow>
                  </TableHeader>
                  <TableBody>
                    {comparison.assumptionDifferences.map((assumptionDifference) => (
                      <TableRow key={assumptionDifference.fieldPath} className="bg-warning/10">
                        <TableCell>{humanizeEnumValue(assumptionDifference.category)}</TableCell>
                        <TableCell>{assumptionDifference.label}</TableCell>
                        <TableCell className="text-right font-mono tabular-nums">
                          {assumptionDifference.baselineValue} {assumptionDifference.unit ?? ""}
                        </TableCell>
                        <TableCell className="text-right font-mono font-medium tabular-nums">
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
              </div>
            </ResearchSection>
          </div>
              <ComparisonAuditEvidence comparison={comparison} />
            </TabsContent>
            <TabsContent value="finding" forceMount className="data-[state=inactive]:hidden"><ComparisonFinding key={`${comparison.baseline.experimentId}:${comparison.candidate.experimentId}`} comparison={comparison} finding={findingDrafts[comparison.runSelection ?? "unbound"] ?? ""} onFindingChange={(finding) => setFindingDrafts((previous) => ({ ...previous, [comparison.runSelection ?? "unbound"]: finding }))} /></TabsContent>
            <TabsContent value="paths" className="space-y-6">


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
          <div className="grid min-w-0 gap-4 xl:grid-cols-3">
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
                    { valueKey: "baselineValue", textKey: "baselineValueText", label: "Baseline", color: "var(--chart-2)" },
                    { valueKey: "candidateValue", textKey: "candidateValueText", label: "Candidate", color: "var(--chart-1)" },
                  ]}
                />
              );
            })}
          </div>

          <ResearchSection title={<>Metric differences</>} description={<>
                Mean, sample standard deviation (n - 1) and standard error of the paired differences. With few seeds, read the per-seed values and the better / worse counts, not just the mean.
              </>}>
<div>
              <MetricComparisonTable metricComparisons={comparison.metricComparisons} />
            </div>
          </ResearchSection>
          </> : null}
          </TabsContent>
          </Tabs>
        </>
      ) : null}
    </>
  );
}
