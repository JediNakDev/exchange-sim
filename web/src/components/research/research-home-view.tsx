"use client";

import Link from "next/link";
import { useState } from "react";
import { useSearchParams } from "next/navigation";
import { ArrowLeftRightIcon, SearchIcon, TimerIcon, PlusIcon } from "lucide-react";
import { Button } from "@/components/ui/button";
import { InputGroup, InputGroupAddon, InputGroupInput } from "@/components/ui/input-group";
import { Empty, EmptyHeader, EmptyTitle, EmptyDescription } from "@/components/ui/empty";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { PageHeading } from "@/components/research/page-heading";
import { ResearchSection } from "@/components/research/research-section";
import { ApiFailureAlert } from "@/components/research/api-failure-alert";
import { useApiResource } from "@/hooks/use-api-resource";
import { getBrowserApiClient } from "@/lib/api/exsim-api-client";
import { readExperiments } from "@/lib/api/research-workflow";
import { researchStudies, STRATEGY_NAMES, configurationChanges } from "@/lib/research-workspace";
import { formatWallClockTime } from "@/lib/exact-decimal-format";

export function ResearchHomeView() {
  const strategyFilter = useSearchParams().get("strategy");
  const [search, setSearch] = useState("");
  const resource = useApiResource("research-studies", () => readExperiments(getBrowserApiClient()), 10000);
  const studies = researchStudies(resource.data?.items ?? []);
  const attention = studies.filter(({ configurations }) => configurations.some((item) => item.runStateCounts.failed + item.runStateCounts.paused + item.runStateCounts.running + item.runStateCounts.queued > 0));
  const visible = studies.filter(({ configurations }) => !strategyFilter || configurations.some((item) => item.config.strategy.strategyKind === strategyFilter)).filter(({ baseline, configurations }) => `${baseline.name} ${baseline.description} ${configurations.map((item) => STRATEGY_NAMES[item.config.strategy.strategyKind]).join(" ")}`.toLowerCase().includes(search.toLowerCase()));
  return <>
    <PageHeading title="Research" description="Test a hypothesis. Compare the trade-off. Explain the outcome." actions={<Button asChild><Link href="/experiments/new"><PlusIcon data-icon="inline-start" />New study</Link></Button>} />
    <section aria-label="Start a study" className="grid gap-px border bg-border md:grid-cols-2">
      <div className="flex min-w-0 flex-col items-start gap-3 bg-card p-5"><TimerIcon className="size-5 text-primary" /><h2 className="text-base font-medium">Execute a parent order</h2><p className="max-w-prose text-sm text-muted-foreground">Does a different schedule reduce cost without leaving more shares unfilled?</p><Button asChild variant="outline" size="sm"><Link href="/experiments/new?intent=execution">Research execution</Link></Button></div>
      <div className="flex min-w-0 flex-col items-start gap-3 bg-card p-5"><ArrowLeftRightIcon className="size-5 text-primary" /><h2 className="text-base font-medium">Evaluate a market maker</h2><p className="max-w-prose text-sm text-muted-foreground">Does a quote change improve net returns after inventory risk, fees and adverse selection?</p><Button asChild variant="outline" size="sm"><Link href="/experiments/new?intent=market-making">Research market making</Link></Button></div>
    </section>
    {resource.failure ? <ApiFailureAlert failure={resource.failure} onRetry={resource.reload} /> : null}
    {attention.length ? <ResearchSection title="Needs attention" description="Active work and interrupted runs. Completion counts are attempts, not valid statistical samples."><div className="divide-y border">{attention.slice(0, 4).map(({ baseline, configurations }) => { const paused = configurations.reduce((n, x) => n + x.runStateCounts.paused, 0); const failed = configurations.reduce((n, x) => n + x.runStateCounts.failed, 0); return <div className="flex flex-wrap items-center justify-between gap-3 p-4" key={baseline.experimentId}><div><Link className="text-sm font-medium underline" href={`/experiments/${baseline.experimentId}`}>{baseline.name}</Link><p className="mt-1 text-xs text-muted-foreground">{paused ? `${paused} paused attempts. ` : ""}{failed ? `${failed} failed attempts. ` : ""}Review runs before interpreting results.</p></div><Button asChild size="sm" variant="outline"><Link href={`/runs?study=${baseline.experimentId}`}>Review runs</Link></Button></div>; })}</div></ResearchSection> : null}
    {strategyFilter ? <p className="text-xs text-muted-foreground">Filtered by strategy. <Link className="underline" href="/research">Show all research</Link></p> : null}
    <ResearchSection title="Your studies" description="Each study keeps its baseline, variants and seed runs together. Comparisons use actual eligible paired attempts." actions={<InputGroup className="w-full sm:w-72"><InputGroupAddon><SearchIcon /></InputGroupAddon><InputGroupInput aria-label="Search studies" placeholder="Search question or strategy" value={search} onChange={(e) => setSearch(e.target.value)} /></InputGroup>}>
      {resource.isLoading ? <p role="status" className="text-sm text-muted-foreground">Loading research…</p> : !resource.data ? <p className="text-sm text-muted-foreground">Study list unavailable. Retry the request before interpreting this as an empty library.</p> : visible.length ? <div className="min-w-0 border bg-card"><Table aria-label="Research studies"><TableHeader><TableRow><TableHead>Study / question</TableHead><TableHead>Baseline</TableHead><TableHead>Variants / changes</TableHead><TableHead>Run attempts</TableHead><TableHead>Next action</TableHead></TableRow></TableHeader><TableBody>{visible.map(({ baseline, configurations, updatedAt }) => { const candidates = configurations.filter((x) => x.experimentId !== baseline.experimentId); const completed = configurations.reduce((n, x) => n + x.runStateCounts.completed, 0); return <TableRow key={baseline.experimentId}>
        <TableCell className="max-w-80 whitespace-normal"><Link className="font-medium underline" href={`/experiments/${baseline.experimentId}`}>{baseline.name}</Link><p className="mt-1 text-xs text-muted-foreground">{baseline.description || "Open the study to review assumptions and choose a treatment."}</p><p className="mt-1 text-xs text-muted-foreground">{formatWallClockTime(updatedAt)}</p></TableCell>
        <TableCell className="whitespace-normal">{STRATEGY_NAMES[baseline.config.strategy.strategyKind]}<p className="mt-1 text-xs text-muted-foreground">{baseline.config.instrument.symbol}, {baseline.config.seedSet.masterSeeds.length} planned seeds</p></TableCell>
        <TableCell className="max-w-64 whitespace-normal">{candidates.length ? candidates.slice(0, 2).map((candidate) => <p key={candidate.experimentId} className="mb-1 text-xs">{candidate.name}: {configurationChanges(baseline.config, candidate.config).length} changed fields</p>) : <span className="text-muted-foreground">Baseline only</span>}</TableCell>
        <TableCell className="font-mono tabular-nums">{completed} completed</TableCell>
        <TableCell><Button asChild variant="outline" size="sm"><Link href={candidates[0] ? `/compare?baseline=${baseline.experimentId}&candidate=${candidates[0].experimentId}` : `/experiments/new?from=${baseline.experimentId}`}>{candidates[0] ? "Compare variants" : "Create a variant"}</Link></Button></TableCell>
      </TableRow>; })}</TableBody></Table></div> : <Empty className="border"><EmptyHeader><EmptyTitle>{search ? "No matching studies" : "Start with a trading question"}</EmptyTitle><EmptyDescription>{search ? "Change your search to find another strategy or question." : "Choose execution or market making above. Your baseline and every variant will stay together here."}</EmptyDescription></EmptyHeader></Empty>}
    </ResearchSection>
  </>;
}
