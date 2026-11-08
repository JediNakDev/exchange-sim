"use client";

import { ChevronRightIcon } from "lucide-react";
import { Collapsible, CollapsibleContent, CollapsibleTrigger } from "@/components/ui/collapsible";

import { useEffect, useRef, useState } from "react";
import { useRouter } from "next/navigation";
import { Button } from "@/components/ui/button";
import { ResearchSection } from "@/components/research/research-section";
import { ApiFailureAlert } from "@/components/research/api-failure-alert";
import { isTerminalRunState } from "@/components/research/run-state-badge";
import { createIdempotencyKey, describeApiFailure, getBrowserApiClient, unwrapApiResult, type ApiSchemas } from "@/lib/api/exsim-api-client";
import { downloadAudit } from "@/lib/audit-download";

export function RunAuditControls({ run }: { run: ApiSchemas["Run"] }) {
  const router = useRouter();
  const transfer = useRef<AbortController | null>(null);
  const retryKey = useRef(createIdempotencyKey("repeat-run"));
  const [progress, setProgress] = useState<string | null>(null);
  const [message, setMessage] = useState<string | null>(null);
  const [failure, setFailure] = useState<ApiSchemas["ApiError"] | null>(null);
  const [repeating, setRepeating] = useState(false);
  useEffect(() => () => transfer.current?.abort(), []);
  const terminal = isTerminalRunState(run.state);
  const exportAudit = async () => {
    if (transfer.current) return;
    const controller = new AbortController();
    transfer.current = controller;
    setFailure(null); setMessage(null); setProgress("Reading native inventory...");
    try {
      const audit = await downloadAudit(getBrowserApiClient(), run.runId, controller.signal, (file) => setProgress(`Downloading and verifying ${file}`));
      const url = URL.createObjectURL(new Blob([JSON.stringify(audit)], { type: "application/json" }));
      const anchor = document.createElement("a"); anchor.href = url; anchor.download = `${run.runId}-audit.json`; anchor.click();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
      setMessage(`Complete audit downloaded: ${audit.artifacts.length} present artifacts verified against inventory SHA-256. ${audit.inventory.reproductionSupported ? "Native reproduction is supported with the local command below." : "Recorded terminal evidence only; cancellation/failure timing is not reproduced by rerunning configuration."}`);
    } catch (error) {
      if (controller.signal.aborted) setMessage("Download cancelled. No complete package was saved.");
      else setFailure(describeApiFailure(error));
    } finally { transfer.current = null; setProgress(null); }
  };
  const repeat = async () => {
    if (repeating) return;
    setRepeating(true); setFailure(null);
    try {
      const next = unwrapApiResult(await getBrowserApiClient().POST("/v1/runs", { body: { experimentId: run.experimentId, masterSeed: run.masterSeed, startPaused: false }, params: { header: { "Idempotency-Key": retryKey.current } } }));
      router.push(`/runs/${next.runId}`);
    } catch (error) { setFailure(describeApiFailure(error)); setRepeating(false); }
  };
  return <ResearchSection title="Reproduce and export" description={<> Repeat this exact saved configuration and seed as a new attempt, or download complete native artifacts. A repeat creates new API identities; compare canonical event and ledger digests. </>}>
<div className="space-y-3">
      <div className="flex flex-wrap gap-2"><Button size="sm" variant="outline" disabled={!terminal || repeating || progress !== null} onClick={() => void repeat()}>{repeating ? "Starting repeat..." : "Repeat this seed"}</Button><Button size="sm" variant="outline" disabled={!terminal || progress !== null || repeating} onClick={() => void exportAudit()}>Download complete audit</Button>{progress ? <Button size="sm" variant="outline" onClick={() => transfer.current?.abort()}>Cancel download</Button> : null}</div>
      {!terminal ? <p className="text-xs text-muted-foreground">Finish or cancel the run before exporting a consistent terminal snapshot or repeating it.</p> : null}
      <p role="status" className="text-xs">{progress ?? message}</p>
      {failure ? <ApiFailureAlert failure={failure} title="Reproduction or export failed" /> : null}
      <Collapsible className="group/collapsible text-xs"><CollapsibleTrigger asChild><Button variant="ghost" size="sm" className="h-auto max-w-full whitespace-normal text-left"><ChevronRightIcon className="shrink-0 transition-transform group-data-[state=open]/collapsible:rotate-90" />Local integrity and reproduction commands</Button></CollapsibleTrigger><CollapsibleContent><p className="my-2 text-muted-foreground">From the project checkout, unpack the downloaded JSON into a new directory. Browser downloads are limited to 32 MiB of artifact bytes; the streaming local downloader handles larger packages. Use the executable identified by recorded provenance; current-build equivalence is not proof of historical authenticity.</p>
        <pre className="overflow-x-auto bg-muted p-3">{`python3 scripts/research_audit.py unpack ${run.runId}-audit.json ./audit-${run.runId}\npython3 scripts/research_audit.py verify ./audit-${run.runId}\nbuild/release/exsim_research verify ./audit-${run.runId}/runner`}</pre>
        <p className="my-2 text-muted-foreground">For streaming transfer, replace API_BASE with the isolated API origin you are using:</p><pre className="overflow-x-auto bg-muted p-3">{`python3 scripts/research_audit.py download API_BASE ${run.runId} ./audit-${run.runId}`}</pre>
        <p className="mt-2 text-muted-foreground">Only completed simulations support native config reproduction. Failed/cancelled packages preserve partial evidence and diagnostics; use integrity verification, not a claim that rerunning reproduces cancellation timing.</p>
      </CollapsibleContent></Collapsible>
    </div></ResearchSection>;
}
