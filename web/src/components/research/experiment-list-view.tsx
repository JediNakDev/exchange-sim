"use client";

import Link from "next/link";
import { useState } from "react";
import { useRouter } from "next/navigation";
import { ArrowLeftRightIcon, ChevronRightIcon, FlaskConicalIcon, GitCompareArrowsIcon, PlusIcon, SearchIcon, SearchXIcon, TimerIcon } from "lucide-react";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui/select";
import { Checkbox } from "@/components/ui/checkbox";
import { Empty, EmptyContent, EmptyDescription, EmptyHeader, EmptyMedia, EmptyTitle } from "@/components/ui/empty";
import { InputGroup, InputGroupAddon, InputGroupInput } from "@/components/ui/input-group";
import { Button } from "@/components/ui/button";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { ApiFailureAlert } from "@/components/research/api-failure-alert";
import { KeyFigureList } from "@/components/research/key-figure-list";
import { PageHeading } from "@/components/research/page-heading";
import { ResearchSection } from "@/components/research/research-section";
import { RunStateBadge } from "@/components/research/run-state-badge";
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

const RESEARCH_TASK_STARTERS = [
  {
    title: "Execute a parent order",
    description: "Compare TWAP with immediate execution. Judge cost alongside completion and the shares left unexecuted.",
    href: "/experiments/new?intent=execution",
    actionLabel: "Research execution",
    icon: TimerIcon,
  },
  {
    title: "Evaluate a market maker",
    description: "Test quote width and inventory skew. Follow net P&L, inventory risk and post-fill price movement across seeds.",
    href: "/experiments/new?intent=market-making",
    actionLabel: "Research market making",
    icon: ArrowLeftRightIcon,
  },
];

function RunStateCountBadges({ runStateCounts }: { runStateCounts: ApiSchemas["RunStateCounts"] }) {
  const nonZeroStates = (Object.entries(runStateCounts) as [ApiSchemas["RunState"], number][]).filter(([, runCount]) => runCount > 0);
  if (nonZeroStates.length === 0) return <span className="text-muted-foreground">no runs</span>;
  return (
    <div className="flex flex-wrap gap-1">
      {nonZeroStates.map(([runState, runCount]) => (
        <RunStateBadge key={runState} runState={runState}>
          {runCount} {runState}
        </RunStateBadge>
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

  const activityFigureText = (count: number) => (experimentList.data ? String(count) : experimentList.isLoading ? "..." : "n/a");
  const hasActiveFilter = search !== "" || task !== "all";

  return (
    <>
      <PageHeading
        title="Experiments"
        description="Test a strategy change against the same market randomness. Keep the assumptions, follow the fills."
        actions={
          <Button asChild>
            <Link href="/experiments/new">
              <PlusIcon data-icon="inline-start" /> New experiment
            </Link>
          </Button>
        }
      />
      {experimentList.failure ? <ApiFailureAlert failure={experimentList.failure} onRetry={experimentList.reload} /> : null}
      <KeyFigureList
        aria-label="Research activity"
        figures={[
          { label: "Experiments", value: activityFigureText(experiments.length) },
          { label: "Completed runs", value: activityFigureText(completedRuns) },
          { label: "In progress or paused", value: activityFigureText(activeRuns) },
        ]}
      />
      <section aria-label="Start a research task" className="grid gap-px border bg-border md:grid-cols-2">
        {RESEARCH_TASK_STARTERS.map(({ title, description, href, actionLabel, icon: StarterIcon }) => (
          <div key={href} className="flex flex-col items-start gap-3 bg-background p-4">
            <div className="flex items-center gap-2">
              <StarterIcon className="size-4 text-primary" aria-hidden="true" />
              <h2 className="text-sm font-medium">{title}</h2>
            </div>
            <p className="max-w-prose text-xs/relaxed text-muted-foreground">{description}</p>
            <Button asChild size="sm" variant="outline"><Link href={href}>{actionLabel}</Link></Button>
          </div>
        ))}
      </section>
      <ResearchSection
        title="Your experiments"
        description="One configuration, a common set of seeds. Select a baseline, then a candidate, to compare them."
        actions={
          <InputGroup className="w-64 max-w-full">
            <InputGroupAddon><SearchIcon aria-hidden="true" /></InputGroupAddon>
            <InputGroupInput aria-label="Search experiments" placeholder="Find an experiment" value={search} onChange={(event) => setSearch(event.target.value)} />
          </InputGroup>
        }
      >
        <div className="flex flex-col border bg-card">
          <div className="flex flex-wrap items-center gap-3 border-b bg-muted/50 px-3 py-2">
            <div className="flex items-center gap-2 text-xs">
              <label htmlFor="library-task" className="text-muted-foreground">Research task</label>
              <Select value={task} onValueChange={setTask}>
                <SelectTrigger id="library-task" size="sm" className="bg-background"><SelectValue /></SelectTrigger>
                <SelectContent><SelectItem value="all">All experiments</SelectItem><SelectItem value="execution">Execution</SelectItem><SelectItem value="market-making">Market making</SelectItem></SelectContent>
              </Select>
            </div>
            <p className="min-w-0 flex-1 text-xs text-muted-foreground" aria-live="polite">{selected.length === 0 ? "Select a baseline, then a candidate to compare." : `Baseline: ${experiments.find((item) => item.experimentId === selected[0])?.name}${selected[1] ? `; candidate: ${experiments.find((item) => item.experimentId === selected[1])?.name}` : "; select a candidate."}`}</p>
            {selected.length > 0 ? <Button size="sm" variant="ghost" onClick={() => setSelectedIds([])}>Clear selection</Button> : null}
            {selected.length === 2 ? (
              <Button size="sm" asChild>
                <Link href={`/compare?baseline=${encodeURIComponent(selected[0])}&candidate=${encodeURIComponent(selected[1])}`}><GitCompareArrowsIcon data-icon="inline-start" />Compare selected</Link>
              </Button>
            ) : null}
          </div>
          <Table aria-label="Experiments">
            <TableHeader>
              <TableRow className="hover:bg-transparent">
                <TableHead className="w-10"><span className="sr-only">Select for comparison</span></TableHead>
                <TableHead>Experiment</TableHead>
                <TableHead>Strategy</TableHead>
                <TableHead className="text-right">Seeds</TableHead>
                <TableHead className="text-right">Feed / order delay</TableHead>
                <TableHead>Runs</TableHead>
                <TableHead>Created</TableHead>
                <TableHead className="w-8"><span className="sr-only">Open experiment</span></TableHead>
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
              {visibleExperiments.map((experiment) => {
                const isSelected = selected.includes(experiment.experimentId);
                return (
                  <TableRow
                    key={experiment.experimentId}
                    tabIndex={0}
                    data-state={isSelected ? "selected" : undefined}
                    className="group cursor-pointer focus-visible:bg-muted focus-visible:outline-2 focus-visible:-outline-offset-2 focus-visible:outline-ring"
                    onClick={() => router.push(`/experiments/${experiment.experimentId}`)}
                    onKeyDown={(keyboardEvent) => {
                      if (keyboardEvent.target === keyboardEvent.currentTarget && keyboardEvent.key === "Enter") router.push(`/experiments/${experiment.experimentId}`);
                    }}
                  >
                    <TableCell onClick={(event) => event.stopPropagation()}>
                      <Checkbox aria-label={`Select ${experiment.name} for comparison`} checked={isSelected} disabled={!isSelected && selected.length === 2}
                        onCheckedChange={(checked) => setSelectedIds(checked === true ? [...selected, experiment.experimentId] : selected.filter((id) => id !== experiment.experimentId))} />
                    </TableCell>
                    <TableCell className="whitespace-normal">
                      <Link href={`/experiments/${experiment.experimentId}`} tabIndex={-1} className="block max-w-72 min-w-40 text-sm font-medium">
                        {experiment.name}
                      </Link>
                      <span className="mt-0.5 block font-mono text-[11px] text-muted-foreground">{experiment.experimentId}</span>
                    </TableCell>
                    <TableCell className="max-w-64 min-w-44 whitespace-normal">
                      {describeStrategy(experiment.config.strategy)}
                      <span className="mt-0.5 block text-muted-foreground">{humanizeEnumValue(experiment.marketWorld)}</span>
                    </TableCell>
                    <TableCell className="text-right font-mono tabular-nums">{experiment.config.seedSet.masterSeeds.length}</TableCell>
                    <TableCell className="text-right font-mono tabular-nums">
                      {formatDurationNs(experiment.config.latency.feedLatencyNs)} / {formatDurationNs(experiment.config.latency.orderEntryLatencyNs)}
                    </TableCell>
                    <TableCell>
                      <RunStateCountBadges runStateCounts={experiment.runStateCounts} />
                    </TableCell>
                    <TableCell className="font-mono text-muted-foreground tabular-nums">{formatWallClockTime(experiment.createdAt)}</TableCell>
                    <TableCell><ChevronRightIcon className="size-4 text-muted-foreground group-hover:text-foreground" aria-hidden="true" /></TableCell>
                  </TableRow>
                );
              })}
              {experimentList.data && visibleExperiments.length === 0 ? (
                <TableRow className="hover:bg-transparent">
                  <TableCell colSpan={8} className="whitespace-normal">
                    <Empty className="py-10">
                      <EmptyHeader>
                        <EmptyMedia variant="icon">{hasActiveFilter ? <SearchXIcon /> : <FlaskConicalIcon />}</EmptyMedia>
                        <EmptyTitle>{hasActiveFilter ? "No matching experiments" : "Start with a question about your strategy"}</EmptyTitle>
                        <EmptyDescription>{hasActiveFilter ? "Try a different name, strategy or research task." : "Choose a baseline, set your assumptions, and run the same market scenario across a set of seeds."}</EmptyDescription>
                      </EmptyHeader>
                      {!search ? <EmptyContent><Button asChild size="sm"><Link href="/experiments/new">Create an experiment</Link></Button></EmptyContent> : null}
                    </Empty>
                  </TableCell>
                </TableRow>
              ) : null}
            </TableBody>
          </Table>
        </div>
      </ResearchSection>
    </>
  );
}
