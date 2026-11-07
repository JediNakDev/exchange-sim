"use client";

import Link from "next/link";
import { useState } from "react";
import { useRouter } from "next/navigation";
import { PlusIcon, SearchIcon, ArrowUpRightIcon } from "lucide-react";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui/select";
import { Checkbox } from "@/components/ui/checkbox";
import { Badge } from "@/components/ui/badge";
import { Input } from "@/components/ui/input";
import { Button } from "@/components/ui/button";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { ApiFailureAlert } from "@/components/research/api-failure-alert";
import { PageHeading } from "@/components/research/page-heading";
import { readExperiments } from "@/lib/api/research-workflow";
import { useApiResource } from "@/hooks/use-api-resource";
import { getBrowserApiClient, type ApiSchemas } from "@/lib/api/exsim-api-client";
import { formatDurationNs, formatWallClockTime } from "@/lib/exact-decimal-format";
import { humanizeEnumValue } from "@/lib/research-labels";

const EXPERIMENT_LIST_POLL_INTERVAL_MS = 3000;

export function describeStrategy(strategyConfig: ApiSchemas["StrategyConfig"]): string {
  switch (strategyConfig.strategyKind) {
    case "fixed_spread_market_maker":
      return `fixed spread, ${strategyConfig.halfSpreadTicks} ticks each side x ${strategyConfig.quoteQuantity}`;
    case "inventory_aware_market_maker":
      return `inventory-aware, ${strategyConfig.halfSpreadTicks} ticks each side x ${strategyConfig.quoteQuantity}, skew ${strategyConfig.inventorySkewTicksPerUnit} per ${strategyConfig.inventorySkewUnitShares}`;
    case "twap_execution":
      return `TWAP ${strategyConfig.parentSide}, ${strategyConfig.parentQuantity} shares / ${formatDurationNs(strategyConfig.sliceIntervalNs)}`;
    case "immediate_sweep_execution":
      return `immediate sweep ${strategyConfig.parentSide}, ${strategyConfig.parentQuantity} shares`;
  }
}

function RunStateCountBadges({ runStateCounts }: { runStateCounts: ApiSchemas["RunStateCounts"] }) {
  const nonZeroStates = (Object.entries(runStateCounts) as [ApiSchemas["RunState"], number][]).filter(([, runCount]) => runCount > 0);
  if (nonZeroStates.length === 0) return <span className="text-muted-foreground">no runs</span>;
  return (
    <div className="flex flex-wrap gap-1">
      {nonZeroStates.map(([runState, runCount]) => (
        <Badge key={runState} className="run-state" data-state={runState} variant={runState === "failed" ? "destructive" : runState === "running" ? "default" : "secondary"}>
          {runCount} {runState}
        </Badge>
      ))}
    </div>
  );
}

export function ExperimentListView() {
  const router = useRouter();
  const [search, setSearch] = useState("");
  const [task, setTask] = useState("all");
  const [selectedIds, setSelectedIds] = useState<string[]>([]);
  const experimentList = useApiResource(
    "experiments",
    () => readExperiments(getBrowserApiClient()),
    EXPERIMENT_LIST_POLL_INTERVAL_MS,
  );

  const experiments = experimentList.data?.items ?? [];
  const visibleExperiments = experiments.filter((experiment) => {
    const execution = experiment.config.strategy.strategyKind.endsWith("_execution");
    return (task === "all" || (task === "execution" ? execution : !execution))
      && `${experiment.name} ${experiment.experimentId} ${describeStrategy(experiment.config.strategy)}`.toLowerCase().includes(search.toLowerCase());
  });
  const selected = selectedIds.filter((id) => experiments.some((experiment) => experiment.experimentId === id));
  const completedRuns = experiments.reduce((total, experiment) => total + experiment.runStateCounts.completed, 0);
  const activeRuns = experiments.reduce((total, experiment) => total + experiment.runStateCounts.running + experiment.runStateCounts.queued + experiment.runStateCounts.paused, 0);

  return (
    <>
      <PageHeading
        title="Experiments"
        description="Test a strategy change against the same market randomness. Keep the assumptions, follow the fills."
        actions={
          <Button asChild size="sm">
            <Link href="/experiments/new">
              <PlusIcon /> New experiment
            </Link>
          </Button>
        }
      />
      {experimentList.failure ? <ApiFailureAlert failure={experimentList.failure} onRetry={experimentList.reload} /> : null}
      <section aria-label="Start a research task" className="grid gap-4 md:grid-cols-2">
        <div className="rounded-lg border bg-card p-5">
          <h2 className="text-lg font-medium">Execute a parent order</h2>
          <p className="mt-2 text-sm text-muted-foreground">Compare TWAP with immediate execution. Judge cost alongside completion and the shares left unexecuted.</p>
          <Button asChild className="mt-4" variant="outline"><Link href="/experiments/new?intent=execution">Research execution</Link></Button>
        </div>
        <div className="rounded-lg border bg-card p-5">
          <h2 className="text-lg font-medium">Evaluate a market maker</h2>
          <p className="mt-2 text-sm text-muted-foreground">Test quote width and inventory skew. Follow net P&amp;L, inventory risk and post-fill price movement across seeds.</p>
          <Button asChild className="mt-4" variant="outline"><Link href="/experiments/new?intent=market-making">Research market making</Link></Button>
        </div>
      </section>
      <p className="text-xs text-muted-foreground">Interactive synthetic market, single venue. Controlled experiments are not calibrated predictions of live trading performance.</p>
      <dl className="study-summary" aria-label="Research activity">
        <div><dt>Experiments</dt><dd>{experimentList.data ? experiments.length : experimentList.isLoading ? "..." : "n/a"}</dd></div>
        <div><dt>Completed runs</dt><dd>{experimentList.data ? completedRuns : experimentList.isLoading ? "..." : "n/a"}</dd></div>
        <div><dt>In progress or paused</dt><dd>{experimentList.data ? activeRuns : experimentList.isLoading ? "..." : "n/a"}</dd></div>
      </dl>
      <section className="study-library" aria-labelledby="study-library-title">
        <div className="study-library-heading">
          <div><h2 id="study-library-title">Your experiments</h2><p>One configuration, a common set of seeds.</p></div>
          <div className="relative w-56 max-w-[50%]">
            <SearchIcon className="absolute top-2.5 left-3 text-muted-foreground" size={15} aria-hidden="true" />
            <Input aria-label="Search experiments" placeholder="Find an experiment" value={search} onChange={(event) => setSearch(event.target.value)} className="h-9 pl-9 text-xs" />
          </div>
        </div>
        <div className="flex flex-wrap items-center gap-3 border-t px-5 py-3">
          <div className="flex items-center gap-2 text-xs"><label htmlFor="library-task">Research task</label>
            <Select value={task} onValueChange={setTask}>
              <SelectTrigger id="library-task"><SelectValue /></SelectTrigger>
              <SelectContent><SelectItem value="all">All experiments</SelectItem><SelectItem value="execution">Execution</SelectItem><SelectItem value="market-making">Market making</SelectItem></SelectContent>
            </Select>
          </div>
          <p className="min-w-0 flex-1 text-xs text-muted-foreground" aria-live="polite">{selected.length === 0 ? "Select a baseline, then a candidate to compare." : `Baseline: ${experiments.find((item) => item.experimentId === selected[0])?.name}${selected[1] ? `; candidate: ${experiments.find((item) => item.experimentId === selected[1])?.name}` : "; select a candidate."}`}</p>
          {selected.length > 0 ? <Button size="sm" variant="ghost" onClick={() => setSelectedIds([])}>Clear selection</Button> : null}
          {selected.length === 2 ? <Button size="sm" asChild><Link href={`/compare?baseline=${encodeURIComponent(selected[0])}&candidate=${encodeURIComponent(selected[1])}`}>Compare selected</Link></Button> : null}
        </div>
        <Table>
          <TableHeader>
            <TableRow>
              <TableHead><span className="sr-only">Select for comparison</span></TableHead>
              <TableHead>Experiment</TableHead>
              <TableHead>Strategy</TableHead>
              <TableHead className="text-right">Seeds</TableHead>
              <TableHead className="text-right">Feed / order delay</TableHead>
              <TableHead>Runs</TableHead>
              <TableHead>Created</TableHead>
              <TableHead><span className="sr-only">Open experiment</span></TableHead>
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
            {visibleExperiments.map((experiment) => (
              <TableRow
                key={experiment.experimentId}
                tabIndex={0}
                className="cursor-pointer focus-visible:bg-accent focus-visible:outline-none"
                onClick={() => router.push(`/experiments/${experiment.experimentId}`)}
                onKeyDown={(keyboardEvent) => {
                  if (keyboardEvent.target === keyboardEvent.currentTarget && keyboardEvent.key === "Enter") router.push(`/experiments/${experiment.experimentId}`);
                }}
              >
                <TableCell onClick={(event) => event.stopPropagation()}>
                  <Checkbox aria-label={`Select ${experiment.name} for comparison`} checked={selected.includes(experiment.experimentId)} disabled={!selected.includes(experiment.experimentId) && selected.length === 2}
                    onCheckedChange={(checked) => setSelectedIds(checked === true ? [...selected, experiment.experimentId] : selected.filter((id) => id !== experiment.experimentId))} />
                </TableCell>
                <TableCell>
                  <Link href={`/experiments/${experiment.experimentId}`} tabIndex={-1} className="study-name">
                    {experiment.name}
                  </Link>
                  <span className="study-id font-mono">{experiment.experimentId}</span>
                </TableCell>
                <TableCell className="study-strategy">{describeStrategy(experiment.config.strategy)}<span className="study-world">{humanizeEnumValue(experiment.marketWorld)}</span></TableCell>
                <TableCell className="text-right tabular-nums"><span className="study-seed">{experiment.config.seedSet.masterSeeds.length}</span></TableCell>
                <TableCell className="text-right font-mono text-xs tabular-nums">
                  {formatDurationNs(experiment.config.latency.feedLatencyNs)} / {formatDurationNs(experiment.config.latency.orderEntryLatencyNs)}
                </TableCell>
                <TableCell>
                  <RunStateCountBadges runStateCounts={experiment.runStateCounts} />
                </TableCell>
                <TableCell className="text-xs text-muted-foreground">{formatWallClockTime(experiment.createdAt)}</TableCell>
                <TableCell><ArrowUpRightIcon size={16} className="text-muted-foreground" aria-hidden="true" /></TableCell>
              </TableRow>
            ))}
            {experimentList.data && visibleExperiments.length === 0 ? (
              <TableRow>
                <TableCell colSpan={8}>
                  <div className="study-empty"><strong>{search || task !== "all" ? "No matching experiments" : "Start with a question about your strategy"}</strong>
                    <p>{search || task !== "all" ? "Try a different name, strategy or research task." : "Choose a baseline, set your assumptions, and run the same market scenario across a set of seeds."}</p>
                    {!search ? <Button asChild size="sm"><Link href="/experiments/new">Create an experiment</Link></Button> : null}
                  </div>
                </TableCell>
              </TableRow>
            ) : null}
          </TableBody>
        </Table>
      </section>
    </>
  );
}
