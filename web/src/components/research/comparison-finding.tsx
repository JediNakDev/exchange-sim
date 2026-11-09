"use client";
import { useState } from "react";
import Link from "next/link";
import type { ApiSchemas } from "@/lib/api/exsim-api-client";
import { Field, FieldLabel, FieldDescription } from "@/components/ui/field";
import { Textarea } from "@/components/ui/textarea";
import { Button } from "@/components/ui/button";
import { ResearchSection } from "@/components/research/research-section";
export function ComparisonFinding({ comparison, finding, onFindingChange }: { comparison: ApiSchemas["ExperimentComparison"]; finding: string; onFindingChange: (text: string) => void }) {
  const [saved, setSaved] = useState(false);
  const params = new URLSearchParams({ baseline: comparison.baseline.experimentId, candidate: comparison.candidate.experimentId, ...(comparison.runSelection ? { runSelection: comparison.runSelection } : {}) });
  function download() {
    const note = { format: "exsim.research-finding/1", finding, writtenAt: new Date().toISOString(), query: Object.fromEntries(params), baseline: comparison.baseline, candidate: comparison.candidate, selectionPolicy: comparison.selectionPolicy, runSelection: comparison.runSelection, assumptionDifferences: comparison.assumptionDifferences, seedPairing: comparison.seedPairing, attemptLedger: comparison.attemptLedger, metrics: comparison.metricComparisons };
    const url = URL.createObjectURL(new Blob([JSON.stringify(note, null, 2)], { type: "application/json" }));
    const anchor = document.createElement("a"); anchor.href = url; anchor.download = "exsim-research-finding.json"; anchor.click(); setTimeout(() => URL.revokeObjectURL(url), 1000); setSaved(true);
  }
  return <ResearchSection title="What did you learn?" description="Record the trade-off and next test with the exact selected cohort. A conclusion is your interpretation, not automatic strategy approval.">
    <Field className="max-w-2xl"><FieldLabel htmlFor="research-finding">Finding and next hypothesis</FieldLabel><Textarea id="research-finding" rows={5} value={finding} onChange={(e) => { onFindingChange(e.target.value); setSaved(false); }} placeholder="What improved, what became worse, and which assumption would you test next?" /><FieldDescription>Exported as a local JSON file with selected attempts, configuration differences and metric evidence. Text is not saved on the service; export before leaving this page.</FieldDescription></Field>
    <div className="mt-4 flex flex-wrap gap-2"><Button size="sm" disabled={!finding.trim() || !comparison.runSelection} onClick={download}>Export finding and evidence</Button><Button asChild size="sm" variant="outline"><Link href={`/compare?${params}`}>Open pinned comparison</Link></Button><Button asChild size="sm" variant="outline"><Link href={`/experiments/new?from=${comparison.candidate.experimentId}`}>Test another change</Link></Button></div>
    {saved ? <p role="status" className="mt-3 text-xs text-muted-foreground">Download requested. Keep the exported file; this note is not stored in your study.</p> : null}
  </ResearchSection>;
}
