"use client";

import Link from "next/link";
import { useSearchParams } from "next/navigation";
import { ApiFailureAlert } from "@/components/research/api-failure-alert";
import { useApiResource } from "@/hooks/use-api-resource";
import { getBrowserApiClient, unwrapApiResult } from "@/lib/api/exsim-api-client";
import { humanizeEnumValue } from "@/lib/research-labels";

export function useComparisonContext() {
  const params = useSearchParams();
  const baseline = params.get("baseline");
  const candidate = params.get("candidate");
  return baseline && candidate && baseline !== candidate
    ? `?${new URLSearchParams({ baseline, candidate, ...(params.get("runSelection") ? { runSelection: params.get("runSelection")! } : {}) }).toString()}` : "";
}

export function RunResearchContext({ experimentId }: { experimentId: string }) {
  const params = useSearchParams();
  const comparisonQuery = useComparisonContext();
  const experiment = useApiResource(`run-context:${experimentId}`, async () => unwrapApiResult(await getBrowserApiClient().GET("/v1/experiments/{experimentId}", { params: { path: { experimentId } } })), null);
  const role = comparisonQuery ? params.get("baseline") === experimentId ? "Baseline" : params.get("candidate") === experimentId ? "Candidate" : null : null;
  return <section aria-label="Research context" className="rounded-md border bg-card px-4 py-3 text-sm">
    <div className="flex flex-wrap items-baseline gap-x-3 gap-y-1">
      {role ? <strong>{role}</strong> : null}
      <Link className="min-w-0 max-w-full break-words font-medium underline" href={`/experiments/${encodeURIComponent(experimentId)}`}>{experiment.data?.name ?? experimentId}</Link>
      {experiment.data ? <span className="text-xs text-muted-foreground">{humanizeEnumValue(experiment.data.config.strategy.strategyKind)}; config <span className="font-mono">{experiment.data.configDigest.hexDigest.slice(0, 12)}</span></span> : null}
      {role ? <Link className="underline" href={`/compare${comparisonQuery}`}>Return to baseline / candidate comparison</Link> : null}
    </div>
    {experiment.failure ? <ApiFailureAlert failure={experiment.failure} title="Could not load experiment context" onRetry={experiment.reload} /> : null}
  </section>;
}
