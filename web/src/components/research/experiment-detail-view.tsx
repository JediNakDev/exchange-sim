"use client";

import Link from "next/link";
import { useParams, useRouter } from "next/navigation";
import { GitCompareIcon } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { ApiFailureAlert } from "@/components/research/api-failure-alert";
import { PageHeading } from "@/components/research/page-heading";
import { isTerminalRunState, RunStateBadge } from "@/components/research/run-state-badge";
import { useApiResource } from "@/hooks/use-api-resource";
import { getBrowserApiClient, unwrapApiResult } from "@/lib/api/exsim-api-client";
import { formatIntegerText, formatSimulationTimeNs, formatWallClockTime } from "@/lib/exact-decimal-format";
import { humanizeEnumValue } from "@/lib/research-labels";
import { cn } from "@/lib/utils";

const ACTIVE_RUN_POLL_INTERVAL_MS = 1000;
const IDLE_RUN_POLL_INTERVAL_MS = 10000;

export function ExperimentDetailView() {
  const { experimentId } = useParams<{ experimentId: string }>();
  const router = useRouter();
  const apiClient = getBrowserApiClient();
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
    async () => unwrapApiResult(await apiClient.GET("/v1/runs", { params: { query: { experimentId, limit: 1000 } } })),
    (latestRunPage) => (latestRunPage.items.some((run) => !isTerminalRunState(run.state)) ? ACTIVE_RUN_POLL_INTERVAL_MS : IDLE_RUN_POLL_INTERVAL_MS),
  );
  const runList = runListResource.data;

  if (experimentResource.failure) return <ApiFailureAlert failure={experimentResource.failure} title="Could not load experiment" />;
  if (!experiment) return <p className="text-sm text-muted-foreground">Loading experiment...</p>;

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
          experiment.parentExperimentId ? (
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
          )
        }
      />
      {experiment.description ? <p className="text-sm">{experiment.description}</p> : null}
      <div className="grid gap-4 xl:grid-cols-[minmax(0,3fr)_minmax(0,2fr)]">
        <Card size="sm">
          <CardHeader>
            <CardTitle>Runs</CardTitle>
            <CardDescription>One run per master seed. Updates every second while any run is active.</CardDescription>
          </CardHeader>
          <CardContent>
            {runListResource.failure ? <ApiFailureAlert failure={runListResource.failure} /> : null}
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
