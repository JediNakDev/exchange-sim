"use client";

import Link from "next/link";
import { useParams, useRouter } from "next/navigation";
import { useRef, useState } from "react";
import { toast } from "sonner";
import { Checkbox } from "@/components/ui/checkbox";
import { readExperimentRuns, startMissingSeedRuns } from "@/lib/api/research-workflow";
import { GitCompareIcon } from "lucide-react";
import { Button } from "@/components/ui/button";
import { KeyFigureList } from "@/components/research/key-figure-list";
import { ResearchSection } from "@/components/research/research-section";
import { Field, FieldLabel } from "@/components/ui/field";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { ApiFailureAlert } from "@/components/research/api-failure-alert";
import { PageHeading } from "@/components/research/page-heading";
import { isTerminalRunState, RunStateBadge } from "@/components/research/run-state-badge";
import { useApiResource } from "@/hooks/use-api-resource";
import { describeApiFailure, getBrowserApiClient, unwrapApiResult, type ApiSchemas } from "@/lib/api/exsim-api-client";
import { formatDurationNs, formatIntegerText, formatSimulationTimeNs, formatWallClockTime } from "@/lib/exact-decimal-format";
import { humanizeEnumValue } from "@/lib/research-labels";

const ACTIVE_RUN_POLL_INTERVAL_MS = 1000;
const IDLE_RUN_POLL_INTERVAL_MS = 10000;

export function ExperimentDetailView() {
  const { experimentId } = useParams<{ experimentId: string }>();
  const router = useRouter();
  const apiClient = getBrowserApiClient();
  const [startPaused, setStartPaused] = useState(false);
  const [isStarting, setIsStarting] = useState(false);
  const starting = useRef(false);
  const [launchFailure, setLaunchFailure] = useState<ApiSchemas["ApiError"] | null>(null);
  const experimentResource = useApiResource(`experiment:${experimentId}`, async () => unwrapApiResult(await apiClient.GET("/v1/experiments/{experimentId}", { params: { path: { experimentId } } })), null);
  const experiment = experimentResource.data;
  // The service flattens the stored configuration the same way it does before creation.
  const assumptionsResource = useApiResource(
    experiment ? `assumptions:${experiment.experimentId}` : null,
    async () =>
      unwrapApiResult(
        await apiClient.POST("/v1/experiment-previews", {
          body: { name: experiment!.name, derivedFromBaselineId: experiment!.derivedFromBaselineId, config: experiment!.config },
        }),
      ),
    null,
  );
  const runListResource = useApiResource(
    `runs-of:${experimentId}`,
    () => readExperimentRuns(apiClient, experimentId),
    (latestRunPage) => (latestRunPage.items.some((run) => !isTerminalRunState(run.state)) ? ACTIVE_RUN_POLL_INTERVAL_MS : IDLE_RUN_POLL_INTERVAL_MS),
  );
  const runList = runListResource.data;

  if (experimentResource.failure) return <ApiFailureAlert failure={experimentResource.failure} title="Could not load experiment" onRetry={experimentResource.reload} />;
  if (!experiment) return <p className="text-sm text-muted-foreground">Loading experiment...</p>;

  const recordedSeeds = new Set(runList?.items.map((run) => run.masterSeed));
  const missingSeeds = experiment.config.seedSet.masterSeeds.filter((seed) => !recordedSeeds.has(seed));
  const completedRuns = runList?.items.filter((run) => run.state === "completed") ?? [];
  const failedRuns = runList?.items.filter((run) => run.state === "failed" || run.state === "cancelled") ?? [];
  const activeRuns = runList?.items.filter((run) => !isTerminalRunState(run.state)) ?? [];
  const completedSeeds = new Set(completedRuns.map((run) => run.masterSeed));
  // Starting missing seeds is the header's green action; once every seed has a run, analysis becomes the next step.
  const analysisIsTheNextStep = runList !== null && missingSeeds.length === 0;
  const startMissing = async () => {
    if (starting.current) return;
    starting.current = true;
    setIsStarting(true);
    setLaunchFailure(null);
    try {
      const count = await startMissingSeedRuns(apiClient, experiment, startPaused);
      toast.success(count === 0 ? "All seeds already have runs" : `Started ${count} seed ${count === 1 ? "run" : "runs"}`);
    } catch (caught) {
      setLaunchFailure(describeApiFailure(caught));
    } finally {
      starting.current = false;
      setIsStarting(false);
      runListResource.reload();
    }
  };

  return (
    <>
      <PageHeading
        title={experiment.name}
        description={
          <span className="font-mono text-xs">
            {experiment.experimentId} from baseline {experiment.derivedFromBaselineId}
            {experiment.parentExperimentId ? (
              <>
                {" "}
                with parent <Link className="underline" href={`/experiments/${experiment.parentExperimentId}`}>{experiment.parentExperimentId}</Link>
              </>
            ) : null}
          </span>
        }
        actions={
          <div className="flex flex-wrap gap-2">
          <Button asChild size="sm" variant="outline"><Link href={`/experiments/new?from=${encodeURIComponent(experiment.experimentId)}`}>Create variant</Link></Button>
          {experiment.parentExperimentId ? (
            <Button asChild size="sm" variant="outline">
              <Link href={`/compare?baseline=${experiment.parentExperimentId}&candidate=${experiment.experimentId}`}>
                <GitCompareIcon /> Compare with parent
              </Link>
            </Button>
          ) : (
            <Button asChild size="sm" variant="outline">
              <Link href={`/compare?baseline=${experiment.experimentId}`}>
                <GitCompareIcon /> Compare
              </Link>
            </Button>
          )}
          {runList && missingSeeds.length > 0 ? <Button size="sm" disabled={isStarting || runListResource.failure !== null} onClick={() => void startMissing()}>{isStarting ? "Starting..." : "Start missing seeds"}</Button> : null}
          </div>
        }
      />
      {experiment.description ? <p className="max-w-prose text-sm">{experiment.description}</p> : null}
      <ResearchSection title="Research progress" aria-label="Experiment overview">
        <p className="mt-1 text-sm text-muted-foreground">{humanizeEnumValue(experiment.config.strategy.strategyKind)} on {experiment.config.instrument.symbol}; {formatDurationNs(experiment.config.bounds.endSimulationTimeNs)} per seed. Change one assumption in a variant, then compare completed runs on common seeds.</p>
        <KeyFigureList figures={[
          { label: "Seeds with completed runs", value: runList ? `${completedSeeds.size} / ${experiment.config.seedSet.masterSeeds.length}` : "Unavailable" },
          { label: "Active or paused runs", value: runList ? activeRuns.length : "Unavailable" },
          { label: "Failed or cancelled runs", value: runList ? failedRuns.length : "Unavailable" },
          { label: "Seeds without a run", value: runList ? missingSeeds.length : "Unavailable" },
        ]} />
        {runListResource.failure ? <p className="mb-3 text-sm text-destructive">Progress is stale. Refresh the run list before drawing conclusions.</p> : null}
        <div className="flex flex-wrap gap-2">
          {completedRuns[0] ? <Button asChild size="sm" variant={analysisIsTheNextStep ? "default" : "outline"}><Link href={`/runs/${completedRuns[0].runId}`}>Analyze a completed run</Link></Button> : null}
          {activeRuns[0] ? <Button asChild size="sm" variant="outline"><Link href={`/runs/${activeRuns[0].runId}`}>Monitor an active run</Link></Button> : null}
          {failedRuns[0] ? <Button asChild size="sm" variant="outline"><Link href={`/runs/${failedRuns[0].runId}`}>Review interrupted run</Link></Button> : null}
        </div>
        <p className="mt-3 text-xs text-muted-foreground">{experiment.config.seedSet.masterSeeds.length < 2 ? "One seed is a demonstration. Use multiple paired seeds to assess variability." : "Seed variability describes this synthetic scenario. It does not establish live-market performance."} Failed and cancelled runs are not successful observations.</p>
      </ResearchSection>
      <div className="flex min-w-0 flex-col gap-8">
        <ResearchSection title="Runs by seed" description="Each master seed should have a run. Updates every second while any run is active.">
            {runListResource.failure ? <ApiFailureAlert failure={runListResource.failure} onRetry={runListResource.reload} /> : null}
            {launchFailure ? <ApiFailureAlert failure={launchFailure} title="Seed launch interrupted" /> : null}
            {runList && missingSeeds.length > 0 ? (
              <div className="mb-4 flex flex-wrap items-center gap-3 border bg-muted/40 p-3 text-sm" aria-live="polite">
                <p className="min-w-48 flex-1">{missingSeeds.length} of {experiment.config.seedSet.masterSeeds.length} seeds still need runs. Recorded runs are kept.</p>
                <Field orientation="horizontal" className="w-auto"><Checkbox id="start-paused" checked={startPaused} disabled={isStarting} onCheckedChange={(checked) => setStartPaused(checked === true)} /><FieldLabel htmlFor="start-paused">Start paused</FieldLabel></Field>
              </div>
            ) : null}
            <div className="min-w-0 border bg-card">
              <Table aria-label="Runs by seed">
                <TableHeader>
                  <TableRow>
                    <TableHead>Run</TableHead>
                    <TableHead className="text-right">Seed</TableHead>
                    <TableHead>State</TableHead>
                    <TableHead className="text-right">Events</TableHead>
                    <TableHead className="text-right">Simulated time</TableHead>
                    <TableHead>Created</TableHead>
                    <TableHead>Inspect</TableHead>
                  </TableRow>
                </TableHeader>
                <TableBody>
                  {runList?.items.map((run) => (
                    <TableRow
                      key={run.runId}
                      tabIndex={0}
                      className="cursor-pointer focus-visible:bg-accent focus-visible:outline-2 focus-visible:-outline-offset-2 focus-visible:outline-ring"
                      onClick={() => router.push(`/runs/${run.runId}`)}
                      onKeyDown={(keyboardEvent) => {
                        if (keyboardEvent.target === keyboardEvent.currentTarget && keyboardEvent.key === "Enter") router.push(`/runs/${run.runId}`);
                      }}
                    >
                      <TableCell className="font-mono text-xs">
                        <Link href={`/runs/${run.runId}`} tabIndex={-1}>
                          {run.runId}
                        </Link>
                      </TableCell>
                      <TableCell className="text-right font-mono tabular-nums">{run.masterSeed}</TableCell>
                      <TableCell>
                        <RunStateBadge runState={run.state} />
                      </TableCell>
                      <TableCell className="text-right font-mono tabular-nums">{formatIntegerText(run.progress.processedCanonicalEventCount)}</TableCell>
                      <TableCell className="text-right font-mono text-xs tabular-nums">
                        {formatSimulationTimeNs(run.progress.simulationTimeNs, 3)} / {formatSimulationTimeNs(run.progress.endSimulationTimeNs, 0)}
                      </TableCell>
                      <TableCell className="font-mono text-xs">{formatWallClockTime(run.createdAt)}</TableCell>
                      <TableCell>
                        <Link className="text-xs underline" href={`/runs/${run.runId}/inspect`} onClick={(clickEvent) => clickEvent.stopPropagation()}>
                          events
                        </Link>
                      </TableCell>
                    </TableRow>
                  ))}
                  {runList && runList.items.length === 0 ? (
                    <TableRow>
                      <TableCell colSpan={7} className="text-muted-foreground">
                        No runs yet.
                      </TableCell>
                    </TableRow>
                  ) : null}
                </TableBody>
              </Table>
            </div>
        </ResearchSection>
        <ResearchSection title="Assumptions" description={<span className="font-mono break-all">config {experiment.configDigest.algorithm} {experiment.configDigest.hexDigest}</span>}>
            {assumptionsResource.failure ? <ApiFailureAlert failure={assumptionsResource.failure} onRetry={assumptionsResource.reload} /> : null}
            <div className="min-w-0 border bg-card [&>[data-slot=table-container]]:max-h-[36rem] [&>[data-slot=table-container]]:overflow-auto">
              <Table aria-label="Assumptions">
                <TableHeader className="sticky top-0 z-1 bg-muted"><TableRow><TableHead>Category</TableHead><TableHead>Assumption</TableHead><TableHead>Value</TableHead></TableRow></TableHeader>
                <TableBody>
                  {assumptionsResource.data?.assumptions.map((assumption) => (
                    <TableRow key={assumption.fieldPath} data-differs-from-baseline={assumption.differsFromBaseline ? "true" : undefined} className="data-[differs-from-baseline=true]:bg-warning/10">
                      <TableCell className="text-muted-foreground">{humanizeEnumValue(assumption.category)}</TableCell>
                      <TableCell className="whitespace-normal">{assumption.label}</TableCell>
                      <TableCell className="font-mono whitespace-normal break-all tabular-nums">
                        {assumption.value}
                        {assumption.unit ? <span className="text-muted-foreground"> {assumption.unit}</span> : null}
                      </TableCell>
                    </TableRow>
                  ))}
                </TableBody>
              </Table>
            </div>
        </ResearchSection>
      </div>
    </>
  );
}
