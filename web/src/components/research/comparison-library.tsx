"use client";
import Link from "next/link";
import { useState } from "react";
import { useRouter } from "next/navigation";
import type { ApiSchemas } from "@/lib/api/exsim-api-client";
import { researchStudies, configurationChanges } from "@/lib/research-workspace";
import { ResearchSection } from "@/components/research/research-section";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Field, FieldLabel, FieldDescription, FieldError } from "@/components/ui/field";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { Empty, EmptyHeader, EmptyTitle, EmptyDescription } from "@/components/ui/empty";

export function ComparisonLibrary({ experiments }: { experiments: ApiSchemas["Experiment"][] }) {
  const router = useRouter();
  const [failure, setFailure] = useState<string | null>(null);
  const studies = researchStudies(experiments);
  const pairs = studies.flatMap(({ baseline, configurations }) => configurations.filter((c) => c.experimentId !== baseline.experimentId).map((candidate) => ({ baseline, candidate })));
  async function openReceipt(file: File | undefined) {
    if (!file) return;
    setFailure(null);
    try {
      if (file.size > 8 * 1024 * 1024) throw new Error("Receipt exceeds 8 MiB. Open a smaller comparison receipt, not a complete audit archive.");
      const value = JSON.parse(await file.text());
      if (value === null || typeof value !== "object" || !["exsim.comparison-receipt/1", "exsim.research-finding/1"].includes(value.format) || value.query === null || typeof value.query !== "object") throw new Error("Choose an exsim comparison receipt or research finding JSON file.");
      const baseline = value.query.baselineExperimentId ?? value.query.baseline, candidate = value.query.candidateExperimentId ?? value.query.candidate, runSelection = value.query.runSelection;
      if (![baseline, candidate, runSelection].every((field) => typeof field === "string" && field.length > 0)) throw new Error("This receipt is missing its baseline, candidate or pinned run selection.");
      router.push(`/compare?${new URLSearchParams({ baseline, candidate, runSelection })}`);
    } catch (error) { setFailure(error instanceof Error ? error.message : "Could not read this receipt."); }
  }
  return <>
    <ResearchSection title="Continue a study comparison" description="Start with an existing baseline and its variants. Native eligibility and assumption differences are checked when you open the pair.">
      {pairs.length ? <div className="border bg-card"><Table aria-label="Study comparisons"><TableHeader><TableRow><TableHead>Baseline / question</TableHead><TableHead>Candidate</TableHead><TableHead>Changed fields</TableHead><TableHead>Action</TableHead></TableRow></TableHeader><TableBody>{pairs.map(({ baseline, candidate }) => <TableRow key={candidate.experimentId}><TableCell className="max-w-72 whitespace-normal"><Link className="font-medium underline" href={`/experiments/${baseline.experimentId}`}>{baseline.name}</Link><p className="mt-1 text-xs text-muted-foreground">{baseline.description}</p></TableCell><TableCell className="max-w-64 whitespace-normal">{candidate.name}</TableCell><TableCell className="font-mono">{configurationChanges(baseline.config, candidate.config).length}</TableCell><TableCell><Button asChild variant="outline" size="sm"><Link href={`/compare?baseline=${baseline.experimentId}&candidate=${candidate.experimentId}`}>Compare this pair</Link></Button></TableCell></TableRow>)}</TableBody></Table></div> : <Empty className="border"><EmptyHeader><EmptyTitle>No study has a variant yet</EmptyTitle><EmptyDescription>Create a baseline, change one assumption in a variant, then evaluate their common seeds.</EmptyDescription></EmptyHeader><Button asChild size="sm"><Link href="/research">Choose a research question</Link></Button></Empty>}
    </ResearchSection>
    <ResearchSection title="Reopen pinned evidence" description="Use a downloaded comparison receipt or research finding. The service loads those exact selected attempts; unavailable evidence will be reported rather than substituted."><Field className="max-w-xl"><FieldLabel htmlFor="comparison-receipt-file">Open comparison receipt</FieldLabel><Input id="comparison-receipt-file" type="file" accept="application/json,.json" aria-describedby="receipt-help receipt-error" onChange={(event) => void openReceipt(event.target.files?.[0])} /><FieldDescription id="receipt-help">Local JSON, up to 8 MiB. No file is uploaded or stored on the service.</FieldDescription>{failure ? <FieldError id="receipt-error">{failure}</FieldError> : null}</Field></ResearchSection>
    {experiments.length > 1 ? <ResearchSection title="Compare across studies" description="Cross-study comparisons may mix assumptions. Inspect compatibility, changed fields and paired coverage before interpreting a difference."><Button asChild variant="outline" size="sm"><Link href={`/compare?baseline=${experiments[0].experimentId}`}>Choose baseline and candidate</Link></Button></ResearchSection> : null}
  </>;
}
