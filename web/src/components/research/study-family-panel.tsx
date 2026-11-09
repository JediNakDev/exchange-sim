"use client";
import Link from "next/link";
import { useApiResource } from "@/hooks/use-api-resource";
import { getBrowserApiClient, type ApiSchemas } from "@/lib/api/exsim-api-client";
import { readExperiments } from "@/lib/api/research-workflow";
import { researchRoot, configurationChanges, STRATEGY_NAMES } from "@/lib/research-workspace";
import { Button } from "@/components/ui/button";
import { Collapsible, CollapsibleContent, CollapsibleTrigger } from "@/components/ui/collapsible";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { ResearchSection } from "@/components/research/research-section";
import { ApiFailureAlert } from "@/components/research/api-failure-alert";

export function StudyFamilyPanel({ experiment }: { experiment: ApiSchemas["Experiment"] }) {
  const resource = useApiResource("study-family", () => readExperiments(getBrowserApiClient()), null);
  if (!resource.data) return <ResearchSection title="Study workspace">{resource.failure ? <ApiFailureAlert failure={resource.failure} title="Study lineage unavailable" onRetry={resource.reload} /> : <p role="status">Loading baseline and variants…</p>}</ResearchSection>;
  const experiments = resource.data.items;
  const root = researchRoot(experiment, experiments);
  const baseline = experiments.find((e) => e.experimentId === root) ?? experiment;
  const family = experiments.filter((e) => researchRoot(e, experiments) === root);
  return <ResearchSection title="Study workspace" description={baseline.description || "Keep the question, baseline and treatments together. Each configuration retains its exact assumptions."} actions={<Button asChild variant={family.length === 1 ? "default" : "outline"} size="sm"><Link href={`/experiments/new?from=${experiment.experimentId}`}>Test another change</Link></Button>}>
    {resource.failure ? <ApiFailureAlert failure={resource.failure} title="Study lineage unavailable" onRetry={resource.reload} /> : null}
    <div className="flex flex-wrap items-center gap-2 border-b pb-3 text-xs"><Link className="underline" href="/research">All research</Link><span>/</span><Link className="font-medium underline" href={`/experiments/${baseline.experimentId}`}>{baseline.name}</Link><span className="text-muted-foreground">{root === experiment.experimentId ? "Baseline configuration" : `Viewing variant: ${experiment.name}`}</span></div>
    <div className="mt-3 min-w-0 border bg-card"><Table aria-label="Study configurations"><TableHeader><TableRow><TableHead>Role / configuration</TableHead><TableHead>Changed assumptions</TableHead><TableHead>Research action</TableHead></TableRow></TableHeader><TableBody>{family.map((item) => { const changes = configurationChanges(baseline.config, item.config); return <TableRow key={item.experimentId} data-state={item.experimentId === experiment.experimentId ? "selected" : undefined}>
      <TableCell className="max-w-72 whitespace-normal"><Link className="font-medium underline" href={`/experiments/${item.experimentId}`}>{item.name}</Link><p className="mt-1 text-xs text-muted-foreground">{item.experimentId === root ? "Baseline" : "Variant"}, {STRATEGY_NAMES[item.config.strategy.strategyKind]}</p></TableCell>
      <TableCell className="max-w-96 whitespace-normal">{changes.length ? <Collapsible><CollapsibleTrigger asChild><Button variant="ghost" size="sm" className="h-auto max-w-full whitespace-normal text-left">{changes.length} changed fields: {changes.slice(0, 2).map((c) => c.path).join(", ")}{changes.length > 2 ? ", …" : ""}</Button></CollapsibleTrigger><CollapsibleContent><ul className="space-y-2 py-3">{changes.map((c) => <li key={c.path} className="break-words text-xs"><span>{c.path}</span>: <span className="font-mono">{c.before}</span> to <span className="font-mono">{c.after}</span></li>)}</ul></CollapsibleContent></Collapsible> : <span className="text-xs text-muted-foreground">{item.experimentId === root ? "Reference assumptions" : "Same configuration, repeatability test"}</span>}</TableCell>
      <TableCell>{item.experimentId !== root ? <Button asChild size="sm" variant={item.experimentId === experiment.experimentId || item.experimentId === family.find((e) => e.experimentId !== root)?.experimentId ? "default" : "outline"}><Link href={`/compare?baseline=${root}&candidate=${item.experimentId}`}>Compare with baseline</Link></Button> : <span className="text-xs text-muted-foreground">Reference configuration</span>}</TableCell>
    </TableRow>; })}</TableBody></Table></div>
  </ResearchSection>;
}
