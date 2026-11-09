"use client";
import Link from "next/link";
import { useOptimistic, useTransition } from "react";
import { useSearchParams, useRouter } from "next/navigation";
import { SearchIcon } from "lucide-react";
import { useApiResource } from "@/hooks/use-api-resource";
import { getBrowserApiClient, unwrapApiResult } from "@/lib/api/exsim-api-client";
import { readAllPages, readExperiments } from "@/lib/api/research-workflow";
import { researchRoot, STRATEGY_NAMES } from "@/lib/research-workspace";
import { formatSimulationTimeNs } from "@/lib/exact-decimal-format";
import { PageHeading } from "@/components/research/page-heading";
import { ResearchSection } from "@/components/research/research-section";
import { ApiFailureAlert } from "@/components/research/api-failure-alert";
import { RunStateBadge } from "@/components/research/run-state-badge";
import { Button } from "@/components/ui/button";
import { InputGroup, InputGroupAddon, InputGroupInput } from "@/components/ui/input-group";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { Empty, EmptyHeader, EmptyTitle, EmptyDescription } from "@/components/ui/empty";

export function RunListView() {
  const search = useSearchParams(), router = useRouter();
  const [query, setQuery] = useOptimistic(search.get("q") ?? "");
  const [, startTransition] = useTransition();
  const status = search.get("status") ?? "all", study = search.get("study");
  const client = getBrowserApiClient();
  const resource = useApiResource("all-research-runs", async () => {
    const [runs, experiments] = await Promise.all([readAllPages(async (cursor) => unwrapApiResult(await client.GET("/v1/runs", { params: { query: { limit: 200, cursor } } })), (r) => r.runId), readExperiments(client)]);
    return { runs: runs.items, experiments: experiments.items };
  }, 5000);
  const data = resource.data;
  const experiments = new Map(data?.experiments.map((e) => [e.experimentId, e]));
  const visible = data?.runs.filter((run) => {
    const experiment = experiments.get(run.experimentId);
    return (!study || run.experimentId === study || (experiment && researchRoot(experiment, data.experiments) === study)) && (status === "all" || (status === "active" ? ["running", "queued", "paused"].includes(run.state) : status === "interrupted" ? ["failed", "cancelled"].includes(run.state) : run.state === status)) && `${experiment?.name} ${run.runId} ${run.masterSeed}`.toLowerCase().includes(query.toLowerCase());
  }) ?? [];
  function filter(key: string, value: string) { const params = new URLSearchParams(search); if (query) params.set("q", query); if (value) params.set(key, value); else params.delete(key); startTransition(() => { if (key === "q") setQuery(value); router.replace(`/runs?${params}`, { scroll: false }); }); }
  return <>
    <PageHeading title="Runs" description="Follow active work, recover interrupted attempts and open the exact seed behind a result." actions={<Button asChild variant="outline"><Link href="/research">Return to research</Link></Button>} />
    {resource.failure ? <ApiFailureAlert failure={resource.failure} title="Run list unavailable or stale" onRetry={resource.reload} /> : null}
    <div className="flex flex-wrap items-center justify-between gap-3"><nav aria-label="Run filters" className="flex flex-wrap gap-1">{[["all", "All runs"], ["active", "Active and paused"], ["interrupted", "Failed and cancelled"], ["completed", "Completed"]].map(([value, label]) => <Button key={value} size="sm" variant={status === value ? "secondary" : "ghost"} aria-pressed={status === value} onClick={() => filter("status", value)}>{label}</Button>)}</nav><InputGroup className="w-full sm:w-72"><InputGroupAddon><SearchIcon /></InputGroupAddon><InputGroupInput aria-label="Search runs" value={query} placeholder="Study, exact seed or run ID" onChange={(e) => filter("q", e.target.value)} /></InputGroup></div>
    {study ? <p className="text-xs text-muted-foreground">Filtered to one study. <Button variant="link" size="sm" onClick={() => filter("study", "")}>Show all studies</Button></p> : null}
    <ResearchSection title={status === "active" ? "Work in progress" : status === "interrupted" ? "Work to investigate" : "Seed run attempts"} description="Every attempt remains visible. Completed simulation does not imply full parent completion or comparison eligibility.">
      {resource.isLoading ? <p role="status">Loading runs…</p> : !data ? <p className="text-sm text-muted-foreground">Run list unavailable. Retry before interpreting this as no recorded work.</p> : visible.length ? <div className="min-w-0 border bg-card"><Table aria-label="Research run queue"><TableHeader><TableRow><TableHead>Study / configuration</TableHead><TableHead>Seed</TableHead><TableHead>State</TableHead><TableHead>Simulated time</TableHead><TableHead>Outcome / next action</TableHead></TableRow></TableHeader><TableBody>{visible.map((run) => { const experiment = experiments.get(run.experimentId); return <TableRow key={run.runId}>
        <TableCell className="max-w-80 whitespace-normal"><Link href={`/experiments/${run.experimentId}`} className="font-medium underline">{experiment?.name ?? "Experiment unavailable"}</Link><p className="mt-1 text-xs text-muted-foreground">{experiment ? STRATEGY_NAMES[experiment.config.strategy.strategyKind] : run.experimentId}</p><Link className="mt-1 block break-all font-mono text-xs text-muted-foreground underline" href={`/runs/${run.runId}`}>{run.runId}</Link></TableCell>
        <TableCell className="font-mono">{run.masterSeed}</TableCell><TableCell><RunStateBadge runState={run.state} /></TableCell><TableCell className="font-mono text-xs">{formatSimulationTimeNs(run.progress.simulationTimeNs, 3)} / {formatSimulationTimeNs(run.progress.endSimulationTimeNs, 0)}</TableCell><TableCell className="max-w-80 whitespace-normal"><p className="mb-2 text-xs text-muted-foreground">{run.state === "failed" || run.state === "cancelled" ? run.terminalDiagnostics?.message : run.state === "paused" ? "Paused at an event boundary. Step, resume or inspect." : run.state === "completed" ? "Review outcome and observation coverage." : "Simulation work is in progress."}</p><Button asChild size="sm" variant="outline"><Link href={`/runs/${run.runId}`}>{run.state === "completed" ? "Analyze result" : run.state === "paused" ? "Open controls" : "Review run"}</Link></Button></TableCell>
      </TableRow>; })}</TableBody></Table></div> : <Empty className="border"><EmptyHeader><EmptyTitle>No runs in this view</EmptyTitle><EmptyDescription>Change the filters, or start a study from Research. Failed and cancelled work is retained when it exists.</EmptyDescription></EmptyHeader></Empty>}
    </ResearchSection>
  </>;
}
