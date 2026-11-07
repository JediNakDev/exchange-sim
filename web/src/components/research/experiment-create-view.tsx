"use client";

// Create an experiment from a baseline. Every input stays an exact string; the
// research service validates and flattens the configuration (preview) before the
// person can start runs, so the assumptions shown are the service's own reading.

import { useEffect, useMemo, useState } from "react";
import { useRouter, useSearchParams } from "next/navigation";
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
import { readExperiments, startMissingSeedRuns } from "@/lib/api/research-workflow";
import { useApiResource } from "@/hooks/use-api-resource";
import { createIdempotencyKey, describeApiFailure, getBrowserApiClient, unwrapApiResult, type ApiSchemas } from "@/lib/api/exsim-api-client";
import { formatDecimalForDisplay, formatDurationNs } from "@/lib/exact-decimal-format";
import { humanizeEnumValue } from "@/lib/research-labels";
import { cn } from "@/lib/utils";
import { baselineForIntent, draftFromExperiment, exactDurationDescription, nanosecondsInUnit, strategyIntent, unitValueInNanoseconds, type TimingUnit } from "./experiment-setup";

type ExperimentConfig = ApiSchemas["ExperimentConfig"];

const SCENARIO_PARAMETER_LABELS: Record<string, string> = {
  levels: "Price levels per side",
  quoteSizeShares: "Quote quantity",
  spreadTicks: "Half spread",
  fairValueStepIntervalNs: "Fair value step interval",
  fairValueStepTicks: "Fair value step size",
  requoteIntervalNs: "Requote interval",
  meanInterarrivalNs: "Mean time between orders",
  minimumOrderShares: "Minimum order quantity",
  maximumOrderShares: "Maximum order quantity",
  crossTicks: "Crossing distance",
  limitOrderProbability: "Resting order probability",
  limitOrderLifetimeNs: "Resting order lifetime",
};

/** Both executable market makers share the runner's quoting parameters; the inventory-aware one adds a skew. */
type QuotingStrategyConfig = ApiSchemas["FixedSpreadMarketMakerConfig"] | ApiSchemas["InventoryAwareMarketMakerConfig"];

const NO_PARENT_EXPERIMENT = "none";
const EDITABLE_STRATEGY_KINDS: ApiSchemas["StrategyKind"][] = ["fixed_spread_market_maker", "inventory_aware_market_maker", "twap_execution", "immediate_sweep_execution"];

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

function TimingInput({ id, label, value, onValueChange, invalid }: { id: string; label: string; value: string; onValueChange: (value: string) => void; invalid: boolean }) {
  // Choose readable display units once; requests always retain exact ns strings.
  const [unit, setUnit] = useState<TimingUnit>(() => value.length > 9 ? "s" : value.length > 6 ? "ms" : value.length > 3 ? "µs" : "ns");
  const [inputDraft, setInputDraft] = useState<{ text: string; storedValue: string } | null>(null);
  const converted = nanosecondsInUnit(value, unit);
  const displayed = inputDraft?.storedValue === value ? inputDraft.text : converted ?? value;
  return (
    <div className="flex gap-2">
      <Input id={id} value={displayed} inputMode={unit === "ns" ? "numeric" : "decimal"} className="min-w-0 font-mono" aria-describedby={`${id}-help`} aria-invalid={invalid || converted === null || undefined}
        onChange={(event) => {
          const text = event.target.value;
          const next = unitValueInNanoseconds(text, unit);
          const storedValue = unit === "ns" ? text : next ?? "";
          setInputDraft({ text, storedValue });
          onValueChange(storedValue);
        }} />
      <Select value={unit} onValueChange={(next) => { setInputDraft(null); setUnit(next as TimingUnit); }} disabled={converted === null}>
        <SelectTrigger aria-label={`Display unit for ${label}`} className="w-20 shrink-0"><SelectValue /></SelectTrigger>
        <SelectContent>{(["ns", "µs", "ms", "s"] as const).map((option) => <SelectItem key={option} value={option}>{option}</SelectItem>)}</SelectContent>
      </Select>
    </div>
  );
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
  const isTiming = unit === "ns" || unit === "nanoseconds";
  const helpId = `${inputId}-help`;
  return (
    <Field data-invalid={fieldIssues.length > 0 || undefined}>
      <FieldLabel htmlFor={inputId}>
        {label}
        {unit ? <span className="font-normal text-muted-foreground">({isTiming ? "stored as ns" : unit})</span> : null}
      </FieldLabel>
      {isTiming ? <TimingInput id={inputId} label={label} value={value} onValueChange={onValueChange} invalid={fieldIssues.length > 0} /> : <Input
        id={inputId}
        aria-describedby={description ? helpId : undefined}
        value={value}
        inputMode={inputMode}
        aria-invalid={fieldIssues.length > 0 || undefined}
        className={inputMode === "text" ? undefined : "font-mono"}
        onChange={(changeEvent) => onValueChange(changeEvent.target.value)}
      />}
      {isTiming ? <FieldDescription id={helpId}>{exactDurationDescription(value) ?? "Enter a nonnegative duration with whole-nanosecond precision."}{description && description !== exactDurationDescription(value) ? ` · ${description}` : ""}</FieldDescription> : description ? <FieldDescription id={helpId}>{description}</FieldDescription> : null}
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
  const searchParams = useSearchParams();
  const parentId = searchParams.get("from");
  const intent = searchParams.get("intent");
  return <ExperimentEditor key={`${parentId ?? "new"}:${intent ?? "all"}`} parentId={parentId} intent={intent} />;
}

function ExperimentEditor({ parentId, intent }: { parentId: string | null; intent: string | null }) {
  const router = useRouter();
  const apiClient = getBrowserApiClient();
  const serviceResource = useApiResource("service-capabilities", async () => unwrapApiResult(await apiClient.GET("/v1/service")), null);
  const baselineList = useApiResource("baselines", async () => unwrapApiResult(await apiClient.GET("/v1/baselines")), null);
  const experimentList = useApiResource("experiments-for-parent", () => readExperiments(apiClient), null);
  const parentResource = useApiResource(parentId ? `variant:${parentId}` : null, async () => unwrapApiResult(await apiClient.GET("/v1/experiments/{experimentId}", { params: { path: { experimentId: parentId! } } })), null);
  const [editedDraft, setEditedDraft] = useState<ExperimentDraft | null>(null);
  const [previewOutcome, setPreviewOutcome] = useState<PreviewOutcome | null>(null);
  const [requestFailure, setRequestFailure] = useState<ApiSchemas["ApiError"] | null>(null);
  const [isSubmitting, setIsSubmitting] = useState(false);
  const [isPreviewing, setIsPreviewing] = useState(false);
  const [startRunsPaused, setStartRunsPaused] = useState(false);

  const supportedKinds = serviceResource.data?.executableStrategyKinds.filter((kind) => EDITABLE_STRATEGY_KINDS.includes(kind));
  const executableBaselines = baselineList.data?.items.filter((baseline) => supportedKinds?.includes(baseline.config.strategy.strategyKind)) ?? [];
  const firstBaseline = baselineForIntent(executableBaselines, intent);
  const parent = parentResource.data;
  const draft = editedDraft ?? (parent ? draftFromExperiment(parent) : !parentId && firstBaseline ? draftFromBaseline(firstBaseline) : null);
  const createRequest = draft ? buildCreateRequest(draft) : null;
  const createRequestText = createRequest ? JSON.stringify(createRequest) : null;
  const isPreviewCurrent = previewOutcome !== null && previewOutcome.requestText === createRequestText;
  const issuesByFieldPath = useMemo(() => {
    const groupedIssues = new Map<string, string[]>();
    for (const fieldIssue of [...(isPreviewCurrent ? previewOutcome.preview.fieldIssues : []), ...(requestFailure?.fieldIssues ?? [])]) {
      groupedIssues.set(fieldIssue.fieldPath, [...(groupedIssues.get(fieldIssue.fieldPath) ?? []), fieldIssue.issue]);
    }
    return groupedIssues;
  }, [isPreviewCurrent, previewOutcome, requestFailure]);

  useEffect(() => {
    const issues = requestFailure?.fieldIssues ?? previewOutcome?.preview.fieldIssues ?? [];
    for (const issue of issues) {
      const control = document.getElementById(`field-${issue.fieldPath.replaceAll("/", "-")}`);
      if (control) {
        control.focus();
        break;
      }
    }
  }, [previewOutcome, requestFailure]);

  if (serviceResource.failure) return <ApiFailureAlert failure={serviceResource.failure} title="Could not load executable strategies" onRetry={serviceResource.reload} />;
  if (parentResource.failure) return <ApiFailureAlert failure={parentResource.failure} title="Could not load the source experiment" onRetry={parentResource.reload} />;
  if (baselineList.failure) return <ApiFailureAlert failure={baselineList.failure} title="Could not load baselines" onRetry={baselineList.reload} />;
  if (baselineList.data && supportedKinds && executableBaselines.length === 0) return <p className="text-sm text-muted-foreground">No executable baselines are available. Check the research service and reload this page.</p>;
  if (!parentId && baselineList.data && supportedKinds && !firstBaseline) return (
    <Card><CardHeader><CardTitle>Research task unavailable</CardTitle><CardDescription>The service has no executable {intent} baseline. Choose another task or check service capabilities.</CardDescription></CardHeader>
      <CardContent><Button variant="outline" onClick={() => router.push("/experiments/new")}>Choose another research task</Button></CardContent></Card>
  );
  if (!draft || !createRequest || !baselineList.data || !supportedKinds) return <p className="text-sm text-muted-foreground">Loading baselines...</p>;

  const updateDraft = (applyChange: (nextDraft: ExperimentDraft) => void) => {
    const nextDraft = structuredClone(draft);
    applyChange(nextDraft);
    setEditedDraft(nextDraft);
    setRequestFailure(null);
  };
  const strategyConfig = draft.config.strategy.strategyKind === "fixed_spread_market_maker" || draft.config.strategy.strategyKind === "inventory_aware_market_maker" ? draft.config.strategy : null;
  const executionConfig = draft.config.strategy.strategyKind === "twap_execution" || draft.config.strategy.strategyKind === "immediate_sweep_execution" ? draft.config.strategy : null;
  const updateExecution = (applyChange: (strategy: ApiSchemas["TwapExecutionConfig"] | ApiSchemas["ImmediateSweepExecutionConfig"]) => void) => updateDraft((next) => applyChange(next.config.strategy as ApiSchemas["TwapExecutionConfig"] | ApiSchemas["ImmediateSweepExecutionConfig"]));
  const updateQuotingStrategy = (applyChange: (nextStrategy: QuotingStrategyConfig) => void) => updateDraft((nextDraft) => applyChange(nextDraft.config.strategy as QuotingStrategyConfig));

  const requestPreview = async () => {
    if (isPreviewing || isSubmitting) return;
    setIsPreviewing(true);
    setRequestFailure(null);
    try {
      const preview = unwrapApiResult(await apiClient.POST("/v1/experiment-previews", { body: createRequest }));
      setPreviewOutcome({ requestText: JSON.stringify(createRequest), preview, experimentIdempotencyKey: previewOutcome?.requestText === JSON.stringify(createRequest) ? previewOutcome.experimentIdempotencyKey : createIdempotencyKey("experiment") });
    } catch (caught) {
      setRequestFailure(describeApiFailure(caught));
    } finally {
      setIsPreviewing(false);
    }
  };

  const createExperimentAndStartRuns = async () => {
    if (!isPreviewCurrent || !previewOutcome.preview.valid || !supportedKinds.includes(draft.config.strategy.strategyKind) || isSubmitting || isPreviewing) return;
    setIsSubmitting(true);
    setRequestFailure(null);
    try {
      const experiment = unwrapApiResult(
        await apiClient.POST("/v1/experiments", { body: createRequest, params: { header: { "Idempotency-Key": previewOutcome.experimentIdempotencyKey } } }),
      );
      try {
        await startMissingSeedRuns(apiClient, experiment, startRunsPaused);
        toast.success(`Saved ${experiment.name}; seed runs queued`);
      } catch {
        toast.error("Experiment saved. Some seed runs may still need starting; continue from its Runs by seed panel.");
      }
      router.push(`/experiments/${experiment.experimentId}`);
    } catch (caught) {
      setRequestFailure(describeApiFailure(caught));
      setIsSubmitting(false);
    }
  };

  const latency = draft.config.latency;
  const parentCandidates = experimentList.data?.items ?? (parent ? [parent] : []);

  return (
    <>
      <PageHeading title="New experiment" description="Define the trading decision, hold market assumptions constant, then review and launch paired-seed research." />
      {experimentList.failure ? <ApiFailureAlert failure={experimentList.failure} title="Could not load parent experiments" onRetry={experimentList.reload} /> : null}
      <div className="grid gap-4 xl:grid-cols-[minmax(0,1.6fr)_minmax(280px,1fr)]">
        <form
          id="experiment-setup"
          key={draft.baselineId}
          className="experiment-form flex flex-col"
          onSubmit={(submitEvent) => {
            submitEvent.preventDefault();
            void requestPreview();
          }}
        >
          <fieldset aria-label="Experiment configuration" disabled={isSubmitting || isPreviewing} className="flex min-w-0 flex-col gap-5">
          <section aria-label="Research task" className="border-b pb-5">
            <h2 className="text-lg font-semibold">What are you testing?</h2>
            <div className="mt-3 grid gap-2 sm:grid-cols-2">
              {(["execution", "market-making"] as const).map((task) => {
                const baseline = baselineForIntent(executableBaselines, task);
                const selected = strategyIntent(draft.config.strategy.strategyKind) === task;
                return <Button key={task} type="button" variant={selected ? "secondary" : "outline"} aria-pressed={selected} disabled={!baseline} className="h-auto flex-col items-start gap-1 whitespace-normal px-4 py-3 text-left" onClick={() => { if (baseline && !selected) { setEditedDraft(draftFromBaseline(baseline)); setRequestFailure(null); } }}>
                  <span className="font-semibold">{task === "execution" ? "Execute a parent order" : "Test a market maker"}</span>
                  <span className="text-xs font-normal text-muted-foreground">{!baseline ? "No executable baseline available" : task === "execution" ? "Completion, execution cost and residual quantity" : "Spread capture, inventory exposure and adverse selection"}</span>
                </Button>;
              })}
            </div>
            {parent ? <p className="mt-3 text-sm text-muted-foreground">Variant of {parent.name}. Source assumptions are copied exactly. Choosing a different task or baseline replaces them.</p> : null}
          </section>
          <FieldSet>
            <FieldLegend>Experiment and baseline</FieldLegend>
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
                    {executableBaselines.map((baseline) => (
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

          {strategyConfig ? (
            <FieldSet>
              <FieldLegend>Research strategy</FieldLegend>
              <Field>
                <FieldLabel htmlFor="strategy-kind">Quoting model</FieldLabel>
                <Select value={strategyConfig.strategyKind} onValueChange={(kind) => updateDraft((nextDraft) => {
                  const previous = nextDraft.config.strategy as QuotingStrategyConfig;
                  const common = { halfSpreadTicks: previous.halfSpreadTicks, quoteQuantity: previous.quoteQuantity, requoteIntervalNs: previous.requoteIntervalNs, requoteThresholdTicks: previous.requoteThresholdTicks, postOnly: previous.postOnly };
                  nextDraft.config.strategy = kind === "inventory_aware_market_maker"
                    ? { ...common, strategyKind: "inventory_aware_market_maker", inventorySkewTicksPerUnit: "1", inventorySkewUnitShares: "100", scaleSizeWithInventory: true }
                    : { ...common, strategyKind: "fixed_spread_market_maker" };
                })}>
                  <SelectTrigger id="strategy-kind" className="w-full"><SelectValue /></SelectTrigger>
                  <SelectContent>{supportedKinds.includes("fixed_spread_market_maker") ? <SelectItem value="fixed_spread_market_maker">Fixed-spread market maker</SelectItem> : null}{supportedKinds.includes("inventory_aware_market_maker") ? <SelectItem value="inventory_aware_market_maker">Inventory-aware market maker</SelectItem> : null}</SelectContent>
                </Select>
                <FieldDescription>Inventory-aware quoting skews prices away from accumulated inventory and can reduce the size that adds exposure.</FieldDescription>
              </Field>
              <FieldDescription>Quotes one level each side of the strategy-visible mid and requotes by cancel/replace on a timer.</FieldDescription>
              <FieldGroup className="grid gap-3 md:grid-cols-2">
                <TextField label="Half spread" unit="ticks from the visible mid" fieldPath="/config/strategy/halfSpreadTicks" inputMode="numeric" value={strategyConfig.halfSpreadTicks} onValueChange={(halfSpreadTicks) => updateQuotingStrategy((nextStrategy) => void (nextStrategy.halfSpreadTicks = halfSpreadTicks))} issuesByFieldPath={issuesByFieldPath} />
                <TextField label="Quote quantity" unit="shares" fieldPath="/config/strategy/quoteQuantity" inputMode="numeric" value={strategyConfig.quoteQuantity} onValueChange={(quoteQuantity) => updateQuotingStrategy((nextStrategy) => void (nextStrategy.quoteQuantity = quoteQuantity))} issuesByFieldPath={issuesByFieldPath} />
                <TextField label="Requote interval" unit="ns" fieldPath="/config/strategy/requoteIntervalNs" inputMode="numeric" value={strategyConfig.requoteIntervalNs} description={exactDurationDescription(strategyConfig.requoteIntervalNs)} onValueChange={(intervalNs) => updateQuotingStrategy((nextStrategy) => void (nextStrategy.requoteIntervalNs = intervalNs))} issuesByFieldPath={issuesByFieldPath} />
                <TextField label="Requote threshold" unit="ticks" description="Minimum move of the desired quote before a resting quote is replaced." fieldPath="/config/strategy/requoteThresholdTicks" inputMode="numeric" value={strategyConfig.requoteThresholdTicks} onValueChange={(thresholdTicks) => updateQuotingStrategy((nextStrategy) => void (nextStrategy.requoteThresholdTicks = thresholdTicks))} issuesByFieldPath={issuesByFieldPath} />
                {strategyConfig.strategyKind === "inventory_aware_market_maker" ? (
                  <>
                    <TextField label="Inventory skew" unit="ticks per skew unit" fieldPath="/config/strategy/inventorySkewTicksPerUnit" inputMode="numeric" value={strategyConfig.inventorySkewTicksPerUnit} onValueChange={(skewTicks) => updateQuotingStrategy((nextStrategy) => void ((nextStrategy as ApiSchemas["InventoryAwareMarketMakerConfig"]).inventorySkewTicksPerUnit = skewTicks))} issuesByFieldPath={issuesByFieldPath} />
                    <TextField label="Skew unit" unit="shares" fieldPath="/config/strategy/inventorySkewUnitShares" inputMode="numeric" value={strategyConfig.inventorySkewUnitShares} onValueChange={(skewUnitShares) => updateQuotingStrategy((nextStrategy) => void ((nextStrategy as ApiSchemas["InventoryAwareMarketMakerConfig"]).inventorySkewUnitShares = skewUnitShares))} issuesByFieldPath={issuesByFieldPath} />
                    <Field orientation="horizontal" className="md:col-span-2">
                      <Checkbox id="scale-size-with-inventory" checked={strategyConfig.scaleSizeWithInventory} onCheckedChange={(checkedState) => updateQuotingStrategy((nextStrategy) => void ((nextStrategy as ApiSchemas["InventoryAwareMarketMakerConfig"]).scaleSizeWithInventory = checkedState === true))} />
                      <FieldLabel htmlFor="scale-size-with-inventory">Shrink the side that adds inventory as the position nears its limit</FieldLabel>
                    </Field>
                  </>
                ) : null}
                <Field orientation="horizontal" className="md:col-span-2">
                  <Checkbox id="post-only" checked={strategyConfig.postOnly} onCheckedChange={(checkedState) => updateQuotingStrategy((nextStrategy) => void (nextStrategy.postOnly = checkedState === true))} />
                  <FieldLabel htmlFor="post-only">Post only: never cross the visible opposite side</FieldLabel>
                </Field>
              </FieldGroup>
            </FieldSet>
          ) : null}

          {executionConfig ? (
            <FieldSet>
              <FieldLegend>Parent-order execution</FieldLegend>
              <FieldDescription>Children are IOC limits priced through the delivered opposite touch by the protection distance, clamped to the instrument band. The child risk cap and position headroom can leave a residual parent quantity.</FieldDescription>
              <FieldGroup className="grid gap-3 md:grid-cols-2">
                <Field>
                  <FieldLabel htmlFor="execution-method">Execution method</FieldLabel>
                  <Select value={executionConfig.strategyKind} onValueChange={(kind) => updateDraft((next) => {
                    const previous = next.config.strategy as ApiSchemas["TwapExecutionConfig"] | ApiSchemas["ImmediateSweepExecutionConfig"];
                    const common = { parentSide: previous.parentSide, parentQuantity: previous.parentQuantity, startTimeNs: previous.startTimeNs ?? "1000000000", executionDurationNs: previous.executionDurationNs ?? "30000000000", limitOffsetTicks: previous.limitOffsetTicks ?? "5" };
                    next.config.strategy = kind === "twap_execution" ? { ...common, strategyKind: "twap_execution", sliceIntervalNs: "1000000000", sliceOrderType: "limit" } : { ...common, strategyKind: "immediate_sweep_execution" };
                  })}>
                    <SelectTrigger id="execution-method" className="w-full"><SelectValue /></SelectTrigger>
                    <SelectContent>
                      {supportedKinds.includes("twap_execution") ? <SelectItem value="twap_execution">TWAP</SelectItem> : null}
                      {supportedKinds.includes("immediate_sweep_execution") ? <SelectItem value="immediate_sweep_execution">Immediate sweep</SelectItem> : null}
                    </SelectContent>
                  </Select>
                </Field>
                <Field>
                  <FieldLabel htmlFor="parent-side">Parent side</FieldLabel>
                  <Select value={executionConfig.parentSide} onValueChange={(side) => updateExecution((next) => { next.parentSide = side as ApiSchemas["Side"]; })}>
                    <SelectTrigger id="parent-side" className="w-full"><SelectValue /></SelectTrigger>
                    <SelectContent><SelectItem value="buy">Buy</SelectItem><SelectItem value="sell">Sell</SelectItem></SelectContent>
                  </Select>
                </Field>
                <TextField label="Parent quantity" unit="shares" fieldPath="/config/strategy/parentQuantity" inputMode="numeric" value={executionConfig.parentQuantity} onValueChange={(value) => updateExecution((next) => { next.parentQuantity = value; })} issuesByFieldPath={issuesByFieldPath} />
                <TextField label="Parent start" unit="ns" fieldPath="/config/strategy/startTimeNs" inputMode="numeric" value={executionConfig.startTimeNs ?? "1000000000"} onValueChange={(value) => updateExecution((next) => { next.startTimeNs = value; })} issuesByFieldPath={issuesByFieldPath} />
                <TextField label="Execution window" unit="ns" fieldPath="/config/strategy/executionDurationNs" inputMode="numeric" value={executionConfig.executionDurationNs ?? "30000000000"} onValueChange={(value) => updateExecution((next) => { next.executionDurationNs = value; })} issuesByFieldPath={issuesByFieldPath} />
                <TextField label="Price protection" unit="ticks through the visible opposite touch" fieldPath="/config/strategy/limitOffsetTicks" inputMode="numeric" value={executionConfig.limitOffsetTicks ?? "5"} onValueChange={(value) => updateExecution((next) => { next.limitOffsetTicks = value; })} issuesByFieldPath={issuesByFieldPath} />
                {executionConfig.strategyKind === "twap_execution" ? <TextField label="Slice interval" unit="ns" fieldPath="/config/strategy/sliceIntervalNs" inputMode="numeric" value={executionConfig.sliceIntervalNs} onValueChange={(value) => updateExecution((next) => { if (next.strategyKind === "twap_execution") next.sliceIntervalNs = value; })} issuesByFieldPath={issuesByFieldPath} /> : null}
              </FieldGroup>
              <FieldDescription>{executionConfig.strategyKind === "twap_execution" ? "TWAP schedules floor(window / interval) cumulative targets. Each slice sends at most one child; skipped or capped quantities can catch up on later slices. There is no extra final slice." : "Sweep retries remaining quantity on delivered market updates after its prior child completes, through the execution window. It may send several capped children."} A completed simulation does not guarantee a completed parent order.</FieldDescription>
            </FieldSet>
          ) : null}

          <FieldSet>
            <FieldLegend>Run plan</FieldLegend>
            <TextField label="Simulation end" unit="ns" fieldPath="/config/bounds/endSimulationTimeNs" inputMode="numeric" value={draft.config.bounds.endSimulationTimeNs} description={exactDurationDescription(draft.config.bounds.endSimulationTimeNs)} onValueChange={(endTimeText) => updateDraft((nextDraft) => void (nextDraft.config.bounds.endSimulationTimeNs = endTimeText))} issuesByFieldPath={issuesByFieldPath} />
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

          <details className="rounded-lg border p-4" open={issuesByFieldPath.size > 0 || undefined}>
            <summary className="cursor-pointer text-sm font-semibold">Market scenario: background liquidity and order flow</summary>
            <p className="mt-3 text-xs text-muted-foreground">Supported research stresses: background liquidity, taker order flow and delivery latency. Scheduled price jumps, halts, packet gaps and recovery scenarios are not available in this deterministic research workflow. Protocol fault testing is a separate mode.</p>
            <div className="pt-4">
                <FieldSet>
                  <FieldLegend>Scenario</FieldLegend>
                  <FieldDescription>
                    <code className="font-mono">{draft.config.scenario.scenarioId}</code>: {draft.config.scenario.description}
                  </FieldDescription>
                  {draft.config.scenario.backgroundAgents.map((agent, agentIndex) => (
                    <FieldGroup key={agent.agentKind} className="grid gap-3 md:grid-cols-2">
                      <p className="text-sm font-medium md:col-span-2">{humanizeEnumValue(agent.agentKind)}</p>
                      {agent.parameters.map((parameter, parameterIndex) => (
                        <TextField
                          key={parameter.name}
                          label={SCENARIO_PARAMETER_LABELS[parameter.name] ?? humanizeEnumValue(parameter.name)}
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
            </div>
          </details>

          <details className="rounded-lg border p-4" open={issuesByFieldPath.size > 0 || undefined}>
            <summary className="cursor-pointer text-sm font-semibold">Latency: feed, order, acknowledgment and cancel paths</summary>
            <div className="pt-4">
                <FieldSet>
                  <FieldLegend>Latency</FieldLegend>
                  <FieldDescription>Each path is configured separately. Feed: exchange to strategy market data. Order entry: strategy to exchange. Acknowledgment: exchange to strategy reports. Cancel / replace: quote cancellation and replacement requests to exchange; both share this configured path.</FieldDescription>
                  <FieldGroup className="grid gap-3 md:grid-cols-2">
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
                    <TextField label="Feed" unit="ns" fieldPath="/config/latency/feedLatencyNs" inputMode="numeric" value={latency.feedLatencyNs} description={exactDurationDescription(latency.feedLatencyNs)} onValueChange={(latencyNs) => updateDraft((nextDraft) => void (nextDraft.config.latency.feedLatencyNs = latencyNs))} issuesByFieldPath={issuesByFieldPath} />
                    <TextField label="Order entry" unit="ns" fieldPath="/config/latency/orderEntryLatencyNs" inputMode="numeric" value={latency.orderEntryLatencyNs} description={exactDurationDescription(latency.orderEntryLatencyNs)} onValueChange={(latencyNs) => updateDraft((nextDraft) => void (nextDraft.config.latency.orderEntryLatencyNs = latencyNs))} issuesByFieldPath={issuesByFieldPath} />
                    <TextField label="Acknowledgment" unit="ns" fieldPath="/config/latency/acknowledgmentLatencyNs" inputMode="numeric" value={latency.acknowledgmentLatencyNs} description={exactDurationDescription(latency.acknowledgmentLatencyNs)} onValueChange={(latencyNs) => updateDraft((nextDraft) => void (nextDraft.config.latency.acknowledgmentLatencyNs = latencyNs))} issuesByFieldPath={issuesByFieldPath} />
                    <TextField label="Cancel / replace" unit="ns" fieldPath="/config/latency/cancelLatencyNs" inputMode="numeric" value={latency.cancelLatencyNs} description={exactDurationDescription(latency.cancelLatencyNs)} onValueChange={(latencyNs) => updateDraft((nextDraft) => void (nextDraft.config.latency.cancelLatencyNs = latencyNs))} issuesByFieldPath={issuesByFieldPath} />
                    <TextField label="Jitter" unit="ns" fieldPath="/config/latency/jitterNs" inputMode="numeric" value={latency.jitterNs} onValueChange={(latencyNs) => updateDraft((nextDraft) => void (nextDraft.config.latency.jitterNs = latencyNs))} issuesByFieldPath={issuesByFieldPath} />
                    <TextField label="Strategy compute" unit="ns" description="Decision to departure, before order entry or cancel latency." fieldPath="/config/latency/strategyComputeNs" inputMode="numeric" value={latency.strategyComputeNs ?? "0"} onValueChange={(latencyNs) => updateDraft((nextDraft) => void (nextDraft.config.latency.strategyComputeNs = latencyNs))} issuesByFieldPath={issuesByFieldPath} />
                    <Field orientation="horizontal" className="md:col-span-2">
                      <Checkbox id="order-channel-fifo" checked={latency.orderChannelFifo ?? true} onCheckedChange={(checkedState) => updateDraft((nextDraft) => void (nextDraft.config.latency.orderChannelFifo = checkedState === true))} />
                      <FieldLabel htmlFor="order-channel-fifo">One FIFO order-entry channel: a cancel never overtakes an earlier order</FieldLabel>
                    </Field>
                  </FieldGroup>
                </FieldSet>
            </div>
          </details>

          <details className="rounded-lg border p-4" open={issuesByFieldPath.size > 0 || undefined}>
            <summary className="cursor-pointer text-sm font-semibold">Fees and risk: costs, position limits and order caps</summary>
            <div className="pt-4">
                <FieldSet>
                  <FieldLegend>Fees and risk</FieldLegend>
                  <FieldGroup className="grid gap-3 md:grid-cols-2">
                    <TextField label="Fee schedule id" fieldPath="/config/fees/feeScheduleId" inputMode="text" value={draft.config.fees.feeScheduleId} onValueChange={(feeScheduleId) => updateDraft((nextDraft) => void (nextDraft.config.fees.feeScheduleId = feeScheduleId))} issuesByFieldPath={issuesByFieldPath} />
                    <TextField label="Maker fee" unit="USD/share, negative is a rebate" fieldPath="/config/fees/makerFeePerShare" value={draft.config.fees.makerFeePerShare} onValueChange={(feeText) => updateDraft((nextDraft) => void (nextDraft.config.fees.makerFeePerShare = feeText))} issuesByFieldPath={issuesByFieldPath} />
                    <TextField label="Taker fee" unit="USD/share" fieldPath="/config/fees/takerFeePerShare" value={draft.config.fees.takerFeePerShare} onValueChange={(feeText) => updateDraft((nextDraft) => void (nextDraft.config.fees.takerFeePerShare = feeText))} issuesByFieldPath={issuesByFieldPath} />
                    <TextField label="Max absolute position" unit="shares" fieldPath="/config/risk/maxAbsolutePositionShares" inputMode="numeric" value={draft.config.risk.maxAbsolutePositionShares} onValueChange={(limitText) => updateDraft((nextDraft) => void (nextDraft.config.risk.maxAbsolutePositionShares = limitText))} issuesByFieldPath={issuesByFieldPath} />
                    <TextField label="Max open orders" unit="count" fieldPath="/config/risk/maxOpenOrders" inputMode="numeric" value={draft.config.risk.maxOpenOrders} onValueChange={(limitText) => updateDraft((nextDraft) => void (nextDraft.config.risk.maxOpenOrders = limitText))} issuesByFieldPath={issuesByFieldPath} />
                    <TextField label={executionConfig ? "Max child order quantity" : "Max order quantity"} unit="shares" fieldPath="/config/risk/maxOrderQuantity" inputMode="numeric" value={draft.config.risk.maxOrderQuantity} onValueChange={(limitText) => updateDraft((nextDraft) => void (nextDraft.config.risk.maxOrderQuantity = limitText))} issuesByFieldPath={issuesByFieldPath} />
                  </FieldGroup>
                </FieldSet>
            </div>
          </details>

          <details className="rounded-lg border p-4" open={issuesByFieldPath.size > 0 || undefined}>
            <summary className="cursor-pointer text-sm font-semibold">Accounting: initial cash and markout horizons</summary>
            <div className="pt-4">
                <FieldSet>
                  <FieldLegend>Accounting</FieldLegend>
                  <FieldGroup className="grid gap-3 md:grid-cols-2">
                    <TextField label="Initial cash" unit="USD" fieldPath="/config/initialCash" value={draft.config.initialCash} onValueChange={(cashText) => updateDraft((nextDraft) => void (nextDraft.config.initialCash = cashText))} issuesByFieldPath={issuesByFieldPath} />
                    <TextField label="Markout horizons" description={splitListText(draft.markoutHorizonsText).map((value) => exactDurationDescription(value) ?? "Invalid horizon").join(", ")} unit="ns, comma separated" fieldPath="/config/markoutHorizonsNs" inputMode="text" value={draft.markoutHorizonsText} onValueChange={(horizonsText) => updateDraft((nextDraft) => void (nextDraft.markoutHorizonsText = horizonsText))} issuesByFieldPath={issuesByFieldPath} />
                  </FieldGroup>
                </FieldSet>
            </div>
          </details>

          </fieldset>
        </form>

        <div className="flex min-w-0 flex-col gap-4 xl:sticky xl:top-22 xl:self-start">
          <Card size="sm">
            <CardHeader><CardTitle>Review and launch</CardTitle><CardDescription role="status">{isSubmitting ? "Saving the experiment and starting seed runs..." : isPreviewing ? "Validating assumptions with the research service..." : !supportedKinds.includes(draft.config.strategy.strategyKind) ? "This strategy is not executable on the connected service." : isPreviewCurrent ? previewOutcome.preview.valid ? "Ready to launch. The current configuration passed service review." : "Changes required. Fix the issues below and review again." : previewOutcome ? "Review required. Assumptions changed since the last review." : "Draft. Review assumptions before starting runs."}</CardDescription></CardHeader>
            <CardContent className="flex flex-col gap-4">
              <p className="text-sm">{createRequest.config.seedSet.masterSeeds.length} seed runs · {exactDurationDescription(draft.config.bounds.endSimulationTimeNs) ?? "Invalid duration"} per run</p>
          <Field orientation="horizontal">
            <Checkbox id="start-runs-paused" disabled={isSubmitting || isPreviewing} checked={startRunsPaused} onCheckedChange={(checkedState) => setStartRunsPaused(checkedState === true)} />
            <FieldLabel htmlFor="start-runs-paused">Start runs paused, to step through them event by event</FieldLabel>
          </Field>

          <div className="flex flex-wrap gap-2">
            <Button type="submit" form="experiment-setup" disabled={isSubmitting || isPreviewing || !supportedKinds.includes(draft.config.strategy.strategyKind)} variant={isPreviewCurrent ? "outline" : "default"}>
              {isPreviewing ? "Reviewing..." : "Review assumptions"}
            </Button>
            <Button type="button" disabled={!isPreviewCurrent || !previewOutcome?.preview.valid || isSubmitting || isPreviewing || !supportedKinds.includes(draft.config.strategy.strategyKind)} onClick={() => void createExperimentAndStartRuns()}>
              {isSubmitting ? "Starting..." : `Create and start ${createRequest.config.seedSet.masterSeeds.length} ${createRequest.config.seedSet.masterSeeds.length === 1 ? "run" : "runs"}`}
            </Button>
          </div>

            </CardContent>
          </Card>
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
            <CardContent className="pb-4">
              <dl className="grid grid-cols-[auto_minmax(0,1fr)] gap-x-4 gap-y-2 text-sm">
                <dt className="text-muted-foreground">Market</dt><dd>{humanizeEnumValue(draft.config.marketWorld)}</dd>
                <dt className="text-muted-foreground">Instrument</dt><dd>{draft.config.instrument.symbol} · tick {draft.config.instrument.tickSize} · lot {draft.config.instrument.lotSize}</dd>
                <dt className="text-muted-foreground">Strategy</dt><dd>{humanizeEnumValue(draft.config.strategy.strategyKind)}</dd>
                {executionConfig ? <>
                  <dt className="text-muted-foreground">Parent order</dt><dd>{humanizeEnumValue(executionConfig.parentSide)} {executionConfig.parentQuantity} shares</dd>
                  <dt className="text-muted-foreground">Start / window</dt><dd>{exactDurationDescription(executionConfig.startTimeNs) ?? "Invalid"} / {exactDurationDescription(executionConfig.executionDurationNs) ?? "Invalid"}</dd>
                </> : strategyConfig ? <>
                  <dt className="text-muted-foreground">Quotes</dt><dd>{strategyConfig.quoteQuantity} shares per side, {strategyConfig.halfSpreadTicks} ticks from visible mid</dd>
                </> : null}
                <dt className="text-muted-foreground">Seeds</dt><dd className="break-all">{draft.masterSeedsText || "None"}</dd>
                <dt className="text-muted-foreground">Maker / taker</dt><dd>{draft.config.fees.makerFeePerShare} / {draft.config.fees.takerFeePerShare} USD/share</dd>
                <dt className="text-muted-foreground">Delay model</dt><dd>{humanizeEnumValue(latency.latencyModel)}</dd>
                <dt className="text-muted-foreground">Feed / order</dt><dd>{exactDurationDescription(latency.feedLatencyNs) ?? "Invalid"} / {exactDurationDescription(latency.orderEntryLatencyNs) ?? "Invalid"}</dd>
                <dt className="text-muted-foreground">Ack / cancel</dt><dd>{exactDurationDescription(latency.acknowledgmentLatencyNs) ?? "Invalid"} / {exactDurationDescription(latency.cancelLatencyNs) ?? "Invalid"}</dd>
                <dt className="text-muted-foreground">Position cap</dt><dd>{draft.config.risk.maxAbsolutePositionShares} shares</dd>
                <dt className="text-muted-foreground">Mark</dt><dd>{humanizeEnumValue(draft.config.markConvention)}</dd>
              </dl>
              <p className="mt-4 text-xs text-muted-foreground">{draft.config.marketWorld === "interactive_synthetic" ? "Seeded synthetic participants share the book. Results describe this market model, not calibrated real-market performance." : "Interpret results under the declared market model."} {executionConfig ? "Completion is an outcome, not a launch guarantee. Compare cost together with filled and residual quantity." : "Assess net P&L with fees, inventory and eligible markouts. One seed cannot establish a robust improvement."} Keep seeds and market assumptions matched when comparing alternatives.</p>
            </CardContent>
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
                <details open={previewOutcome.preview.fieldIssues.length > 0 || undefined}>
                  <summary className="cursor-pointer text-sm font-medium">Full service assumptions ({previewOutcome.preview.assumptions.filter((line) => line.differsFromBaseline).length} changed from baseline)</summary>
                  <div className="mt-3"><AssumptionsTable assumptions={previewOutcome.preview.assumptions} /></div>
                </details>
              </CardContent>
            ) : null}
          </Card>
        </div>
      </div>
    </>
  );
}
