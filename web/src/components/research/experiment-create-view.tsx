"use client";

// Create an experiment from a baseline. Every input stays an exact string; the
// research service validates and flattens the configuration (preview) before the
// person can start runs, so the assumptions shown are the service's own reading.

import { useMemo, useState } from "react";
import { useRouter } from "next/navigation";
import { toast } from "sonner";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card";
import { Checkbox } from "@/components/ui/checkbox";
import { Field, FieldDescription, FieldError, FieldGroup, FieldLabel, FieldLegend, FieldSet } from "@/components/ui/field";
import { Input } from "@/components/ui/input";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui/select";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { Textarea } from "@/components/ui/textarea";
import { ApiFailureAlert } from "@/components/research/api-failure-alert";
import { PageHeading } from "@/components/research/page-heading";
import { useApiResource } from "@/hooks/use-api-resource";
import { createIdempotencyKey, describeApiFailure, getBrowserApiClient, unwrapApiResult, type ApiSchemas } from "@/lib/api/exsim-api-client";
import { formatDecimalForDisplay, formatDurationNs } from "@/lib/exact-decimal-format";
import { humanizeEnumValue } from "@/lib/research-labels";
import { cn } from "@/lib/utils";

type ExperimentConfig = ApiSchemas["ExperimentConfig"];
type FixedSpreadConfig = ApiSchemas["FixedSpreadMarketMakerConfig"];

const NO_PARENT_EXPERIMENT = "none";

function splitListText(listText: string): string[] {
  return listText.split(/[\s,]+/).filter((entry) => entry.length > 0);
}

interface ExperimentDraft {
  baselineId: string;
  name: string;
  description: string;
  parentExperimentId: string;
  config: ExperimentConfig;
  masterSeedsText: string;
  markoutHorizonsText: string;
}

function draftFromBaseline(baseline: ApiSchemas["BaselineConfiguration"]): ExperimentDraft {
  return {
    baselineId: baseline.baselineId,
    name: `${baseline.name} variant`,
    description: "",
    parentExperimentId: NO_PARENT_EXPERIMENT,
    config: structuredClone(baseline.config),
    masterSeedsText: baseline.config.seedSet.masterSeeds.join(", "),
    markoutHorizonsText: baseline.config.markoutHorizonsNs.join(", "),
  };
}

function buildCreateRequest(draft: ExperimentDraft): ApiSchemas["ExperimentCreateRequest"] {
  return {
    name: draft.name,
    ...(draft.description.trim() === "" ? {} : { description: draft.description }),
    derivedFromBaselineId: draft.baselineId,
    ...(draft.parentExperimentId === NO_PARENT_EXPERIMENT ? {} : { parentExperimentId: draft.parentExperimentId }),
    config: {
      ...draft.config,
      seedSet: { masterSeeds: splitListText(draft.masterSeedsText) },
      markoutHorizonsNs: splitListText(draft.markoutHorizonsText),
    },
  };
}

interface PreviewOutcome {
  requestText: string;
  preview: ApiSchemas["ExperimentPreview"];
  experimentIdempotencyKey: string;
}

function TextField({
  label,
  fieldPath,
  value,
  onValueChange,
  unit,
  description,
  issuesByFieldPath,
  inputMode = "decimal",
}: {
  label: string;
  fieldPath: string;
  value: string;
  onValueChange: (nextValue: string) => void;
  unit?: string;
  description?: string;
  issuesByFieldPath: Map<string, string[]>;
  inputMode?: "decimal" | "numeric" | "text";
}) {
  const inputId = `field-${fieldPath.replaceAll("/", "-")}`;
  const fieldIssues = issuesByFieldPath.get(fieldPath) ?? [];
  return (
    <Field data-invalid={fieldIssues.length > 0 || undefined}>
      <FieldLabel htmlFor={inputId}>
        {label}
        {unit ? <span className="font-normal text-muted-foreground">({unit})</span> : null}
      </FieldLabel>
      <Input
        id={inputId}
        value={value}
        inputMode={inputMode}
        aria-invalid={fieldIssues.length > 0 || undefined}
        className="font-mono"
        onChange={(changeEvent) => onValueChange(changeEvent.target.value)}
      />
      {description ? <FieldDescription>{description}</FieldDescription> : null}
      {fieldIssues.length > 0 ? <FieldError errors={fieldIssues.map((issue) => ({ message: issue }))} /> : null}
    </Field>
  );
}

function AssumptionsTable({ assumptions }: { assumptions: ApiSchemas["AssumptionLine"][] }) {
  return (
    <div className="max-h-[32rem] overflow-auto rounded-md border">
      <Table>
        <TableHeader>
          <TableRow>
            <TableHead>Category</TableHead>
            <TableHead>Assumption</TableHead>
            <TableHead>Value</TableHead>
          </TableRow>
        </TableHeader>
        <TableBody>
          {assumptions.map((assumption) => (
            <TableRow key={assumption.fieldPath} className={cn(assumption.differsFromBaseline && "bg-amber-500/10")}>
              <TableCell className="text-muted-foreground">{humanizeEnumValue(assumption.category)}</TableCell>
              <TableCell>
                {assumption.label}
                {assumption.differsFromBaseline ? (
                  <Badge variant="outline" className="ml-2">
                    changed
                  </Badge>
                ) : null}
              </TableCell>
              <TableCell className="font-mono text-xs break-all">
                {assumption.value}
                {assumption.unit ? <span className="text-muted-foreground"> {assumption.unit}</span> : null}
              </TableCell>
            </TableRow>
          ))}
        </TableBody>
      </Table>
    </div>
  );
}

export function ExperimentCreateView() {
  const router = useRouter();
  const apiClient = getBrowserApiClient();
  const baselineList = useApiResource("baselines", async () => unwrapApiResult(await apiClient.GET("/v1/baselines")), null);
  const experimentList = useApiResource("experiments-for-parent", async () => unwrapApiResult(await apiClient.GET("/v1/experiments", { params: { query: { limit: 200 } } })), null);
  const [editedDraft, setEditedDraft] = useState<ExperimentDraft | null>(null);
  const [previewOutcome, setPreviewOutcome] = useState<PreviewOutcome | null>(null);
  const [requestFailure, setRequestFailure] = useState<ApiSchemas["ApiError"] | null>(null);
  const [isSubmitting, setIsSubmitting] = useState(false);

  const firstBaseline = baselineList.data?.items[0] ?? null;
  const draft = editedDraft ?? (firstBaseline ? draftFromBaseline(firstBaseline) : null);
  const createRequest = useMemo(() => (draft ? buildCreateRequest(draft) : null), [draft]);
  const createRequestText = createRequest ? JSON.stringify(createRequest) : null;
  const isPreviewCurrent = previewOutcome !== null && previewOutcome.requestText === createRequestText;
  const issuesByFieldPath = useMemo(() => {
    const groupedIssues = new Map<string, string[]>();
    for (const fieldIssue of [...(isPreviewCurrent ? previewOutcome.preview.fieldIssues : []), ...(requestFailure?.fieldIssues ?? [])]) {
      groupedIssues.set(fieldIssue.fieldPath, [...(groupedIssues.get(fieldIssue.fieldPath) ?? []), fieldIssue.issue]);
    }
    return groupedIssues;
  }, [isPreviewCurrent, previewOutcome, requestFailure]);

  if (baselineList.failure) return <ApiFailureAlert failure={baselineList.failure} title="Could not load baselines" />;
  if (!draft || !createRequest || !baselineList.data) return <p className="text-sm text-muted-foreground">Loading baselines...</p>;

  const updateDraft = (applyChange: (nextDraft: ExperimentDraft) => void) => {
    const nextDraft = structuredClone(draft);
    applyChange(nextDraft);
    setEditedDraft(nextDraft);
    setRequestFailure(null);
  };
  const strategyConfig = draft.config.strategy.strategyKind === "fixed_spread_market_maker" ? (draft.config.strategy as FixedSpreadConfig) : null;
  const updateFixedSpread = (applyChange: (nextStrategy: FixedSpreadConfig) => void) => updateDraft((nextDraft) => applyChange(nextDraft.config.strategy as FixedSpreadConfig));

  const requestPreview = async () => {
    setRequestFailure(null);
    try {
      const preview = unwrapApiResult(await apiClient.POST("/v1/experiment-previews", { body: createRequest }));
      setPreviewOutcome({ requestText: JSON.stringify(createRequest), preview, experimentIdempotencyKey: createIdempotencyKey("experiment") });
    } catch (caught) {
      setPreviewOutcome(null);
      setRequestFailure(describeApiFailure(caught));
    }
  };

  const createExperimentAndStartRuns = async () => {
    if (!isPreviewCurrent) return;
    setIsSubmitting(true);
    setRequestFailure(null);
    try {
      const experiment = unwrapApiResult(
        await apiClient.POST("/v1/experiments", { body: createRequest, params: { header: { "Idempotency-Key": previewOutcome.experimentIdempotencyKey } } }),
      );
      for (const masterSeed of experiment.config.seedSet.masterSeeds) {
        unwrapApiResult(
          await apiClient.POST("/v1/runs", {
            body: { experimentId: experiment.experimentId, masterSeed, startPaused: false },
            params: { header: { "Idempotency-Key": `${previewOutcome.experimentIdempotencyKey}-seed-${masterSeed}`.slice(0, 128) } },
          }),
        );
      }
      toast.success(`Created ${experiment.name} and queued ${experiment.config.seedSet.masterSeeds.length} runs`);
      router.push(`/experiments/${experiment.experimentId}`);
    } catch (caught) {
      setRequestFailure(describeApiFailure(caught));
      setIsSubmitting(false);
    }
  };

  const latency = draft.config.latency;
  const parentCandidates = experimentList.data?.items ?? [];

  return (
    <>
      <PageHeading title="New experiment" description="Start from a baseline, change what you are testing, review every assumption, then queue one run per master seed." />
      <div className="grid gap-4 xl:grid-cols-[minmax(0,1fr)_minmax(0,1fr)]">
        <form
          className="flex flex-col gap-4"
          onSubmit={(submitEvent) => {
            submitEvent.preventDefault();
            void requestPreview();
          }}
        >
          <FieldSet>
            <FieldLegend>Identity</FieldLegend>
            <FieldGroup className="grid gap-3 md:grid-cols-2">
              <Field>
                <FieldLabel htmlFor="baseline-select">Baseline</FieldLabel>
                <Select
                  value={draft.baselineId}
                  onValueChange={(baselineId) => {
                    const baseline = baselineList.data?.items.find((candidate) => candidate.baselineId === baselineId);
                    if (baseline) setEditedDraft(draftFromBaseline(baseline));
                  }}
                >
                  <SelectTrigger id="baseline-select" className="w-full">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    {baselineList.data.items.map((baseline) => (
                      <SelectItem key={baseline.baselineId} value={baseline.baselineId}>
                        {baseline.name}
                      </SelectItem>
                    ))}
                  </SelectContent>
                </Select>
                <FieldDescription>{baselineList.data.items.find((baseline) => baseline.baselineId === draft.baselineId)?.description}</FieldDescription>
              </Field>
              <Field>
                <FieldLabel htmlFor="parent-select">Parent experiment</FieldLabel>
                <Select value={draft.parentExperimentId} onValueChange={(parentExperimentId) => updateDraft((nextDraft) => void (nextDraft.parentExperimentId = parentExperimentId))}>
                  <SelectTrigger id="parent-select" className="w-full">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value={NO_PARENT_EXPERIMENT}>None</SelectItem>
                    {parentCandidates.map((experiment) => (
                      <SelectItem key={experiment.experimentId} value={experiment.experimentId}>
                        {experiment.name}
                      </SelectItem>
                    ))}
                  </SelectContent>
                </Select>
                <FieldDescription>Lineage only; compare against any experiment later.</FieldDescription>
              </Field>
              <TextField label="Name" fieldPath="/name" inputMode="text" value={draft.name} onValueChange={(name) => updateDraft((nextDraft) => void (nextDraft.name = name))} issuesByFieldPath={issuesByFieldPath} />
              <Field>
                <FieldLabel htmlFor="experiment-description">Hypothesis or notes</FieldLabel>
                <Textarea id="experiment-description" rows={2} value={draft.description} onChange={(changeEvent) => updateDraft((nextDraft) => void (nextDraft.description = changeEvent.target.value))} />
              </Field>
            </FieldGroup>
          </FieldSet>

          <FieldSet>
            <FieldLegend>Scenario</FieldLegend>
            <FieldDescription>
              <code className="font-mono">{draft.config.scenario.scenarioId}</code>: {draft.config.scenario.description}
            </FieldDescription>
            {draft.config.scenario.backgroundAgents.map((agent, agentIndex) => (
              <FieldGroup key={agent.agentKind} className="grid gap-3 md:grid-cols-3">
                <p className="text-sm font-medium md:col-span-3">{humanizeEnumValue(agent.agentKind)}</p>
                {agent.parameters.map((parameter, parameterIndex) => (
                  <TextField
                    key={parameter.name}
                    label={parameter.name}
                    unit={parameter.unit}
                    description={parameter.description}
                    fieldPath={`/config/scenario/backgroundAgents/${agentIndex}/parameters/${parameterIndex}/value`}
                    value={parameter.value}
                    onValueChange={(parameterValue) => updateDraft((nextDraft) => void (nextDraft.config.scenario.backgroundAgents[agentIndex].parameters[parameterIndex].value = parameterValue))}
                    issuesByFieldPath={issuesByFieldPath}
                  />
                ))}
              </FieldGroup>
            ))}
          </FieldSet>

          {strategyConfig ? (
            <FieldSet>
              <FieldLegend>Strategy: fixed-spread market maker</FieldLegend>
              <FieldGroup className="grid gap-3 md:grid-cols-3">
                <TextField label="Quote width" unit="ticks" fieldPath="/config/strategy/quoteWidthTicks" inputMode="numeric" value={strategyConfig.quoteWidthTicks} onValueChange={(quoteWidthTicks) => updateFixedSpread((nextStrategy) => void (nextStrategy.quoteWidthTicks = quoteWidthTicks))} issuesByFieldPath={issuesByFieldPath} />
                <TextField label="Quote quantity" unit="shares" fieldPath="/config/strategy/quoteQuantity" inputMode="numeric" value={strategyConfig.quoteQuantity} onValueChange={(quoteQuantity) => updateFixedSpread((nextStrategy) => void (nextStrategy.quoteQuantity = quoteQuantity))} issuesByFieldPath={issuesByFieldPath} />
                <TextField label="Minimum requote interval" unit="ns" fieldPath="/config/strategy/minimumRequoteIntervalNs" inputMode="numeric" value={strategyConfig.minimumRequoteIntervalNs} onValueChange={(intervalNs) => updateFixedSpread((nextStrategy) => void (nextStrategy.minimumRequoteIntervalNs = intervalNs))} issuesByFieldPath={issuesByFieldPath} />
                <Field orientation="horizontal" className="md:col-span-3">
                  <Checkbox id="requote-on-mid-change" checked={strategyConfig.requoteOnVisibleMidChange} onCheckedChange={(checkedState) => updateFixedSpread((nextStrategy) => void (nextStrategy.requoteOnVisibleMidChange = checkedState === true))} />
                  <FieldLabel htmlFor="requote-on-mid-change">Requote when the strategy-visible mid changes</FieldLabel>
                </Field>
              </FieldGroup>
            </FieldSet>
          ) : null}

          <FieldSet>
            <FieldLegend>Seeds</FieldLegend>
            <TextField
              label="Master seeds"
              description="One run per seed. Separate with commas or spaces. Use the same seeds as the experiment you will compare against."
              fieldPath="/config/seedSet/masterSeeds"
              inputMode="text"
              value={draft.masterSeedsText}
              onValueChange={(masterSeedsText) => updateDraft((nextDraft) => void (nextDraft.masterSeedsText = masterSeedsText))}
              issuesByFieldPath={issuesByFieldPath}
            />
          </FieldSet>

          <FieldSet>
            <FieldLegend>Latency</FieldLegend>
            <FieldDescription>Each path is configured separately. Feed: exchange to strategy market data. Order entry: strategy to exchange. Acknowledgment: exchange to strategy reports. Cancel: strategy cancel to exchange.</FieldDescription>
            <FieldGroup className="grid gap-3 md:grid-cols-3">
              <Field>
                <FieldLabel htmlFor="latency-model">Model</FieldLabel>
                <Select value={latency.latencyModel} onValueChange={(latencyModel) => updateDraft((nextDraft) => void (nextDraft.config.latency.latencyModel = latencyModel as ApiSchemas["LatencyModelKind"]))}>
                  <SelectTrigger id="latency-model" className="w-full">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value="constant">constant</SelectItem>
                    <SelectItem value="uniform_jitter">uniform jitter</SelectItem>
                  </SelectContent>
                </Select>
              </Field>
              <TextField label="Feed" unit="ns" fieldPath="/config/latency/feedLatencyNs" inputMode="numeric" value={latency.feedLatencyNs} description={formatDurationNs(latency.feedLatencyNs)} onValueChange={(latencyNs) => updateDraft((nextDraft) => void (nextDraft.config.latency.feedLatencyNs = latencyNs))} issuesByFieldPath={issuesByFieldPath} />
              <TextField label="Order entry" unit="ns" fieldPath="/config/latency/orderEntryLatencyNs" inputMode="numeric" value={latency.orderEntryLatencyNs} description={formatDurationNs(latency.orderEntryLatencyNs)} onValueChange={(latencyNs) => updateDraft((nextDraft) => void (nextDraft.config.latency.orderEntryLatencyNs = latencyNs))} issuesByFieldPath={issuesByFieldPath} />
              <TextField label="Acknowledgment" unit="ns" fieldPath="/config/latency/acknowledgmentLatencyNs" inputMode="numeric" value={latency.acknowledgmentLatencyNs} description={formatDurationNs(latency.acknowledgmentLatencyNs)} onValueChange={(latencyNs) => updateDraft((nextDraft) => void (nextDraft.config.latency.acknowledgmentLatencyNs = latencyNs))} issuesByFieldPath={issuesByFieldPath} />
              <TextField label="Cancel" unit="ns" fieldPath="/config/latency/cancelLatencyNs" inputMode="numeric" value={latency.cancelLatencyNs} description={formatDurationNs(latency.cancelLatencyNs)} onValueChange={(latencyNs) => updateDraft((nextDraft) => void (nextDraft.config.latency.cancelLatencyNs = latencyNs))} issuesByFieldPath={issuesByFieldPath} />
              <TextField label="Jitter" unit="ns" fieldPath="/config/latency/jitterNs" inputMode="numeric" value={latency.jitterNs} onValueChange={(latencyNs) => updateDraft((nextDraft) => void (nextDraft.config.latency.jitterNs = latencyNs))} issuesByFieldPath={issuesByFieldPath} />
            </FieldGroup>
          </FieldSet>

          <FieldSet>
            <FieldLegend>Fees and risk</FieldLegend>
            <FieldGroup className="grid gap-3 md:grid-cols-3">
              <TextField label="Fee schedule id" fieldPath="/config/fees/feeScheduleId" inputMode="text" value={draft.config.fees.feeScheduleId} onValueChange={(feeScheduleId) => updateDraft((nextDraft) => void (nextDraft.config.fees.feeScheduleId = feeScheduleId))} issuesByFieldPath={issuesByFieldPath} />
              <TextField label="Maker fee" unit="USD/share, negative is a rebate" fieldPath="/config/fees/makerFeePerShare" value={draft.config.fees.makerFeePerShare} onValueChange={(feeText) => updateDraft((nextDraft) => void (nextDraft.config.fees.makerFeePerShare = feeText))} issuesByFieldPath={issuesByFieldPath} />
              <TextField label="Taker fee" unit="USD/share" fieldPath="/config/fees/takerFeePerShare" value={draft.config.fees.takerFeePerShare} onValueChange={(feeText) => updateDraft((nextDraft) => void (nextDraft.config.fees.takerFeePerShare = feeText))} issuesByFieldPath={issuesByFieldPath} />
              <TextField label="Max absolute position" unit="shares" fieldPath="/config/risk/maxAbsolutePositionShares" inputMode="numeric" value={draft.config.risk.maxAbsolutePositionShares} onValueChange={(limitText) => updateDraft((nextDraft) => void (nextDraft.config.risk.maxAbsolutePositionShares = limitText))} issuesByFieldPath={issuesByFieldPath} />
              <TextField label="Max open orders" unit="count" fieldPath="/config/risk/maxOpenOrders" inputMode="numeric" value={draft.config.risk.maxOpenOrders} onValueChange={(limitText) => updateDraft((nextDraft) => void (nextDraft.config.risk.maxOpenOrders = limitText))} issuesByFieldPath={issuesByFieldPath} />
              <TextField label="Max order quantity" unit="shares" fieldPath="/config/risk/maxOrderQuantity" inputMode="numeric" value={draft.config.risk.maxOrderQuantity} onValueChange={(limitText) => updateDraft((nextDraft) => void (nextDraft.config.risk.maxOrderQuantity = limitText))} issuesByFieldPath={issuesByFieldPath} />
            </FieldGroup>
          </FieldSet>

          <FieldSet>
            <FieldLegend>Accounting and bounds</FieldLegend>
            <FieldGroup className="grid gap-3 md:grid-cols-3">
              <TextField label="Initial cash" unit="USD" fieldPath="/config/initialCash" value={draft.config.initialCash} onValueChange={(cashText) => updateDraft((nextDraft) => void (nextDraft.config.initialCash = cashText))} issuesByFieldPath={issuesByFieldPath} />
              <TextField label="Markout horizons" unit="ns, comma separated" fieldPath="/config/markoutHorizonsNs" inputMode="text" value={draft.markoutHorizonsText} onValueChange={(horizonsText) => updateDraft((nextDraft) => void (nextDraft.markoutHorizonsText = horizonsText))} issuesByFieldPath={issuesByFieldPath} />
              <TextField label="Simulation end" unit="ns" fieldPath="/config/bounds/endSimulationTimeNs" inputMode="numeric" value={draft.config.bounds.endSimulationTimeNs} description={formatDurationNs(draft.config.bounds.endSimulationTimeNs.match(/^\d+$/) ? draft.config.bounds.endSimulationTimeNs : "0")} onValueChange={(endTimeText) => updateDraft((nextDraft) => void (nextDraft.config.bounds.endSimulationTimeNs = endTimeText))} issuesByFieldPath={issuesByFieldPath} />
            </FieldGroup>
          </FieldSet>

          <div className="flex flex-wrap gap-2">
            <Button type="submit" variant={isPreviewCurrent ? "outline" : "default"}>
              Review assumptions
            </Button>
            <Button type="button" disabled={!isPreviewCurrent || !previewOutcome?.preview.valid || isSubmitting} onClick={() => void createExperimentAndStartRuns()}>
              {isSubmitting ? "Starting..." : `Create and start ${createRequest.config.seedSet.masterSeeds.length} runs`}
            </Button>
          </div>
        </form>

        <div className="flex flex-col gap-4 xl:sticky xl:top-14 xl:self-start">
          {requestFailure ? <ApiFailureAlert failure={requestFailure} /> : null}
          <Card size="sm">
            <CardHeader>
              <CardTitle>Assumptions summary</CardTitle>
              <CardDescription>
                {isPreviewCurrent
                  ? "As read by the research service. Highlighted rows differ from the baseline."
                  : previewOutcome
                    ? "The form changed since the last review. Review again before starting."
                    : "Review assumptions to see the full configuration the runs will use."}
              </CardDescription>
            </CardHeader>
            {isPreviewCurrent ? (
              <CardContent className="flex flex-col gap-3">
                <div className="flex flex-wrap gap-2 text-sm">
                  <Badge variant={previewOutcome.preview.valid ? "secondary" : "destructive"}>{previewOutcome.preview.valid ? "valid" : "not executable"}</Badge>
                  <span>{previewOutcome.preview.estimatedWork.runCount} runs</span>
                  <span>x {formatDurationNs(previewOutcome.preview.estimatedWork.simulatedDurationPerRunNs)} simulated</span>
                  <span>~{formatDecimalForDisplay(previewOutcome.preview.estimatedWork.estimatedCanonicalEventsPerRun)} events per run</span>
                  <span>~{formatDecimalForDisplay(previewOutcome.preview.estimatedWork.estimatedWallClockSecondsTotal, { fractionDigits: 1 })} s wall clock</span>
                </div>
                <p className="font-mono text-xs text-muted-foreground break-all">
                  config {previewOutcome.preview.configDigest.algorithm} {previewOutcome.preview.configDigest.hexDigest}
                </p>
                {previewOutcome.preview.fieldIssues.length > 0 ? (
                  <ApiFailureAlert
                    title="Configuration issues"
                    failure={{ code: "validation_failed", message: "fix these before starting runs", requestId: "preview", retryable: false, fieldIssues: previewOutcome.preview.fieldIssues }}
                  />
                ) : null}
                <AssumptionsTable assumptions={previewOutcome.preview.assumptions} />
              </CardContent>
            ) : null}
          </Card>
        </div>
      </div>
    </>
  );
}
