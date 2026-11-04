"use client";

import Link from "next/link";
import { useRouter } from "next/navigation";
import { PlusIcon } from "lucide-react";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { ApiFailureAlert } from "@/components/research/api-failure-alert";
import { PageHeading } from "@/components/research/page-heading";
import { useApiResource } from "@/hooks/use-api-resource";
import { getBrowserApiClient, unwrapApiResult, type ApiSchemas } from "@/lib/api/exsim-api-client";
import { formatDecimalForDisplay, formatWallClockTime } from "@/lib/exact-decimal-format";
import { humanizeEnumValue } from "@/lib/research-labels";

const EXPERIMENT_LIST_POLL_INTERVAL_MS = 3000;

export function describeStrategy(strategyConfig: ApiSchemas["StrategyConfig"]): string {
  switch (strategyConfig.strategyKind) {
    case "fixed_spread_market_maker":
      return `fixed spread, ${strategyConfig.quoteWidthTicks} ticks x ${strategyConfig.quoteQuantity}`;
    default:
      return humanizeEnumValue(strategyConfig.strategyKind);
  }
}

function RunStateCountBadges({ runStateCounts }: { runStateCounts: ApiSchemas["RunStateCounts"] }) {
  const nonZeroStates = (Object.entries(runStateCounts) as [ApiSchemas["RunState"], number][]).filter(([, runCount]) => runCount > 0);
  if (nonZeroStates.length === 0) return <span className="text-muted-foreground">no runs</span>;
  return (
    <div className="flex flex-wrap gap-1">
      {nonZeroStates.map(([runState, runCount]) => (
        <Badge key={runState} variant={runState === "failed" ? "destructive" : runState === "running" ? "default" : "secondary"}>
          {runCount} {runState}
        </Badge>
      ))}
    </div>
  );
}

export function ExperimentListView() {
  const router = useRouter();
  const experimentList = useApiResource(
    "experiments",
    async () => unwrapApiResult(await getBrowserApiClient().GET("/v1/experiments", { params: { query: { limit: 200 } } })),
    EXPERIMENT_LIST_POLL_INTERVAL_MS,
  );

  return (
    <>
      <PageHeading
        title="Experiments"
        description="Each experiment fixes one configuration; its runs differ only by master seed."
        actions={
          <Button asChild size="sm">
            <Link href="/experiments/new">
              <PlusIcon /> New experiment
            </Link>
          </Button>
        }
      />
      {experimentList.failure ? <ApiFailureAlert failure={experimentList.failure} /> : null}
      <div className="overflow-x-auto rounded-md border">
        <Table>
          <TableHeader>
            <TableRow>
              <TableHead>Name</TableHead>
              <TableHead>Experiment id</TableHead>
              <TableHead>Strategy</TableHead>
              <TableHead>Market world</TableHead>
              <TableHead className="text-right">Seeds</TableHead>
              <TableHead className="text-right">Latency feed / order</TableHead>
              <TableHead>Runs</TableHead>
              <TableHead>Created</TableHead>
            </TableRow>
          </TableHeader>
          <TableBody>
            {experimentList.isLoading ? (
              <TableRow>
                <TableCell colSpan={8} className="text-muted-foreground">
                  Loading experiments...
                </TableCell>
              </TableRow>
            ) : null}
            {experimentList.data?.items.map((experiment) => (
              <TableRow
                key={experiment.experimentId}
                tabIndex={0}
                className="cursor-pointer focus-visible:bg-accent focus-visible:outline-none"
                onClick={() => router.push(`/experiments/${experiment.experimentId}`)}
                onKeyDown={(keyboardEvent) => {
                  if (keyboardEvent.key === "Enter") router.push(`/experiments/${experiment.experimentId}`);
                }}
              >
                <TableCell className="font-medium">
                  <Link href={`/experiments/${experiment.experimentId}`} tabIndex={-1}>
                    {experiment.name}
                  </Link>
                </TableCell>
                <TableCell className="font-mono text-xs">{experiment.experimentId}</TableCell>
                <TableCell>{describeStrategy(experiment.config.strategy)}</TableCell>
                <TableCell>{humanizeEnumValue(experiment.marketWorld)}</TableCell>
                <TableCell className="text-right tabular-nums">{experiment.config.seedSet.masterSeeds.length}</TableCell>
                <TableCell className="text-right font-mono text-xs tabular-nums">
                  {formatDecimalForDisplay(experiment.config.latency.feedLatencyNs)} / {formatDecimalForDisplay(experiment.config.latency.orderEntryLatencyNs)} ns
                </TableCell>
                <TableCell>
                  <RunStateCountBadges runStateCounts={experiment.runStateCounts} />
                </TableCell>
                <TableCell className="font-mono text-xs">{formatWallClockTime(experiment.createdAt)}</TableCell>
              </TableRow>
            ))}
            {experimentList.data && experimentList.data.items.length === 0 ? (
              <TableRow>
                <TableCell colSpan={8} className="text-muted-foreground">
                  No experiments yet.
                </TableCell>
              </TableRow>
            ) : null}
          </TableBody>
        </Table>
      </div>
    </>
  );
}
