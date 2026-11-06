"use client";

import Link from "next/link";
import { useParams, useRouter } from "next/navigation";
import { useRef, useState } from "react";
import { toast } from "sonner";
import { Checkbox } from "@/components/ui/checkbox";
import { readExperimentRuns, startMissingSeedRuns } from "@/lib/api/research-workflow";
import { GitCompareIcon } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { ApiFailureAlert } from "@/components/research/api-failure-alert";
import { PageHeading } from "@/components/research/page-heading";
import { isTerminalRunState, RunStateBadge } from "@/components/research/run-state-badge";
import { useApiResource } from "@/hooks/use-api-resource";
import { describeApiFailure, getBrowserApiClient, unwrapApiResult, type ApiSchemas } from "@/lib/api/exsim-api-client";
import { formatIntegerText, formatSimulationTimeNs, formatWallClockTime } from "@/lib/exact-decimal-format";
import { humanizeEnumValue } from "@/lib/research-labels";
import { cn } from "@/lib/utils";

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

  if (experimentResource.failure) return <ApiFailureAlert failure={experimentResource.failure} title="Could not load experiment" />;
  if (!experiment) return <p className="text-sm text-muted-foreground">Loading experiment...</p>;

  const recordedSeeds = new Set(runList?.items.map((run) => run.masterSeed));
  const missingSeeds = experiment.config.seedSet.masterSeeds.filter((seed) => !recordedSeeds.has(seed));
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
          <Button asChild size="sm"><Link href={`/experiments/new?from=${encodeURIComponent(experiment.experimentId)}`}>Create variant</Link></Button>
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
          </div>
        }
      />
      {experiment.description ? <p className="text-sm">{experiment.description}</p> : null}
      <div className="flex flex-col gap-5">
        <Card size="sm">
          <CardHeader>
            <CardTitle>Runs by seed</CardTitle>
            <CardDescription>Each master seed should have a run. Updates every second while any run is active.</CardDescription>
          </CardHeader>
          <CardContent>
            {runListResource.failure ? <ApiFailureAlert failure={runListResource.failure} onRetry={runListResource.reload} /> : null}
            {launchFailure ? <ApiFailureAlert failure={launchFailure} title="Seed launch interrupted" /> : null}
            {runList && missingSeeds.length > 0 ? (
              <div className="mb-4 flex flex-wrap items-center gap-3 rounded-md border bg-muted/40 p-3 text-sm" aria-live="polite">
                <p className="min-w-48 flex-1">{missingSeeds.length} of {experiment.config.seedSet.masterSeeds.length} seeds still need runs. Recorded runs are kept.</p>
                <label className="flex items-center gap-2 text-xs"><Checkbox checked={startPaused} disabled={isStarting} onCheckedChange={(checked) => setStartPaused(checked === true)} />Start paused</label>
                <Button size="sm" disabled={isStarting || runListResource.failure !== null} onClick={() => void startMissing()}>{isStarting ? "Starting..." : "Start missing seeds"}</Button>
              </div>
            ) : null}
            <div className="overflow-x-auto rounded-md border">
              <Table>
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
                      className="cursor-pointer focus-visible:bg-accent focus-visible:outline-none"
                      onClick={() => router.push(`/runs/${run.runId}`)}
                      onKeyDown={(keyboardEvent) => {
                        if (keyboardEvent.key === "Enter") router.push(`/runs/${run.runId}`);
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
          </CardContent>
        </Card>
        <Card size="sm">
          <CardHeader>
            <CardTitle>Assumptions</CardTitle>
            <CardDescription className="font-mono text-xs break-all">
              config {experiment.configDigest.algorithm} {experiment.configDigest.hexDigest}
            </CardDescription>
          </CardHeader>
          <CardContent>
            {assumptionsResource.failure ? <ApiFailureAlert failure={assumptionsResource.failure} /> : null}
            <div className="max-h-[36rem] overflow-auto rounded-md border">
              <Table>
                <TableBody>
                  {assumptionsResource.data?.assumptions.map((assumption) => (
                    <TableRow key={assumption.fieldPath} className={cn(assumption.differsFromBaseline && "bg-amber-500/10")}>
                      <TableCell className="text-muted-foreground">{humanizeEnumValue(assumption.category)}</TableCell>
                      <TableCell>{assumption.label}</TableCell>
                      <TableCell className="font-mono text-xs break-all">
                        {assumption.value}
                        {assumption.unit ? <span className="text-muted-foreground"> {assumption.unit}</span> : null}
                      </TableCell>
                    </TableRow>
                  ))}
                </TableBody>
              </Table>
            </div>
          </CardContent>
        </Card>
      </div>
    </>
  );
}
