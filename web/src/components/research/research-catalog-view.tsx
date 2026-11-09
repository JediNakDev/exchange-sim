"use client";
import Link from "next/link";
import { ArrowLeftRightIcon, TimerIcon, GlobeIcon } from "lucide-react";
import { useApiResource } from "@/hooks/use-api-resource";
import { getBrowserApiClient, unwrapApiResult } from "@/lib/api/exsim-api-client";
import { readExperiments } from "@/lib/api/research-workflow";
import { STRATEGY_NAMES } from "@/lib/research-workspace";
import { formatDurationNs } from "@/lib/exact-decimal-format";
import { PageHeading } from "@/components/research/page-heading";
import { ResearchSection } from "@/components/research/research-section";
import { DefinitionList } from "@/components/research/definition-list";
import { ApiFailureAlert } from "@/components/research/api-failure-alert";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { Collapsible, CollapsibleContent, CollapsibleTrigger } from "@/components/ui/collapsible";

const STRATEGY_GUIDES = {
  twap_execution: { question: "How much execution cost can a schedule save, and what remains unfilled?", evidence: "Completion, residual shares, executed shortfall and decision timing", change: "Slice interval, limit protection, child cap" },
  immediate_sweep_execution: { question: "What does immediate liquidity cost against a scheduled baseline?", evidence: "Filled quantity, residual shares, cost including fees", change: "Limit protection, child cap, order latency" },
  fixed_spread_market_maker: { question: "Does earning the spread survive fees, inventory risk and adverse selection?", evidence: "Net P&L, inventory, drawdown and eligible-share markouts", change: "Quote width, quote size, cancel/replace latency" },
  inventory_aware_market_maker: { question: "Does inventory skew control exposure without sacrificing too much return?", evidence: "Net P&L, inventory extremes, drawdown and fill quality", change: "Skew per inventory unit, size scaling, quote width" },
};

export function ResearchCatalogView({ mode }: { mode: "strategies" | "scenarios" }) {
  const client = getBrowserApiClient();
  const resource = useApiResource("research-catalog", async () => {
    const [baselines, service, experiments] = await Promise.all([
      client.GET("/v1/baselines").then(unwrapApiResult), client.GET("/v1/service").then(unwrapApiResult), readExperiments(client),
    ]);
    return { baselines: baselines.items, service, experiments: experiments.items };
  }, null);
  const data = resource.data;
  return <>
    <PageHeading title={mode === "strategies" ? "Strategies" : "Scenarios"} description={mode === "strategies" ? "Choose a research baseline by the trading question it answers. Start a study or revisit existing work." : "Understand the market you are testing against. Reuse explicit synthetic assumptions, not a claim of live-market realism."} />
    {resource.failure ? <ApiFailureAlert failure={resource.failure} onRetry={resource.reload} /> : null}
    {!data ? <p role="status" className="text-sm text-muted-foreground">Loading supported research configurations…</p> : mode === "strategies" ? <div className="grid min-w-0 grid-cols-1 gap-8 xl:grid-cols-2">{data.service.executableStrategyKinds.filter((kind) => Object.hasOwn(STRATEGY_GUIDES, kind)).map((kind) => {
      const guide = STRATEGY_GUIDES[kind];
      const preset = data.baselines.find((b) => b.config.strategy.strategyKind === kind) ?? (kind === "inventory_aware_market_maker" ? data.baselines.find((b) => b.config.strategy.strategyKind === "fixed_spread_market_maker") : undefined);
      const existing = data.experiments.filter((e) => e.config.strategy.strategyKind === kind);
      return <ResearchSection key={kind} className="border bg-card p-5" title={<span className="flex items-center gap-2">{kind.endsWith("execution") ? <TimerIcon className="size-4 text-primary" /> : <ArrowLeftRightIcon className="size-4 text-primary" />}{STRATEGY_NAMES[kind]}</span>} description={guide.question}>
        <DefinitionList rows={[["Evaluate", guide.evidence], ["Useful treatments", guide.change], ["Execution", "Built-in C++ strategy, synthetic single venue"]]} />
        <div className="mt-5 flex flex-wrap gap-2">{preset ? <Button asChild size="sm"><Link href={`/experiments/new?baseline=${preset.baselineId}&strategy=${kind}&intent=${kind.endsWith("execution") ? "execution" : "market-making"}`}>Start a study</Link></Button> : <p className="text-xs text-muted-foreground">No compatible starting preset supplied by this service.</p>}<Button asChild variant="outline" size="sm"><Link href={`/research?strategy=${kind}`}>Research history ({existing.length})</Link></Button></div>
        {existing.length ? <p className="mt-3 text-xs text-muted-foreground">Latest configuration: <Link className="underline" href={`/experiments/${existing[0].experimentId}`}>{existing[0].name}</Link></p> : null}
      </ResearchSection>;
    })}</div> : <>
      <div className="flex items-start gap-3 border border-warning/40 bg-warning/5 p-4"><GlobeIcon className="mt-0.5 size-4 shrink-0 text-warning" /><p className="max-w-prose text-sm">Interactive synthetic market. Your orders share a FIFO book with simulated participants. Results measure behaviour under these assumptions; they do not establish historical or live profitability.</p></div>
      {[...new Set(data.baselines.map((b) => b.config.scenario.scenarioId))].map((scenarioId) => { const presets = data.baselines.filter((b) => b.config.scenario.scenarioId === scenarioId); const config = presets[0].config; return <ResearchSection key={scenarioId} title="Synthetic liquidity and order flow" description={config.scenario.description}>
        <div className="mb-5 flex flex-wrap gap-2"><Badge variant="outline">FIFO priority</Badge><Badge variant="outline">Single venue</Badge><Badge variant="outline">Seeded participants</Badge></div>
        <div className="grid gap-6 xl:grid-cols-2"><div><h3 className="mb-3 text-sm font-medium">Market participants</h3>{config.scenario.backgroundAgents.map((agent) => <div key={agent.agentKind} className="mb-4 border p-4"><p className="mb-2 text-sm font-medium">{agent.agentKind === "market_maker" ? "Background market maker" : "Noise taker"}, {agent.agentCount} participant</p><DefinitionList rows={agent.parameters.map((p) => [p.name, `${p.value} ${p.unit}`])} /></div>)}</div><div><h3 className="mb-3 text-sm font-medium">Default trading conditions</h3><DefinitionList rows={[["Instrument", `${config.instrument.symbol}, tick ${config.instrument.tickSize}`], ["Feed delay", formatDurationNs(config.latency.feedLatencyNs)], ["Outbound order delay", formatDurationNs(config.latency.orderEntryLatencyNs)], ["Cancel / replace delay", formatDurationNs(config.latency.cancelLatencyNs)], ["Maker fee USD/share", config.fees.makerFeePerShare], ["Taker fee USD/share", config.fees.takerFeePerShare], ["Position limit, shares", config.risk.maxAbsolutePositionShares]]} /><p className="mt-4 max-w-prose text-xs text-muted-foreground">Values above describe the first service preset. The selected preset and your study review are authoritative. Latency and fees can be varied separately; neither simulates a feed gap, a halt or calibrated market impact.</p></div></div>
        <Collapsible className="mt-3"><CollapsibleTrigger asChild><Button size="sm" variant="ghost">Read scenario identity</Button></CollapsibleTrigger><CollapsibleContent className="break-all p-3 font-mono text-xs">{scenarioId}</CollapsibleContent></Collapsible>
        <div className="mt-5 border bg-card"><Table aria-label="Scenario starting presets"><TableHeader><TableRow><TableHead>Starting strategy</TableHead><TableHead>Simulation bound</TableHead><TableHead>Action</TableHead></TableRow></TableHeader><TableBody>{presets.map((preset) => <TableRow key={preset.baselineId}><TableCell>{preset.name}</TableCell><TableCell className="font-mono">{formatDurationNs(preset.config.bounds.endSimulationTimeNs)}</TableCell><TableCell><Button asChild variant="outline" size="sm"><Link href={`/experiments/new?baseline=${preset.baselineId}`}>Use this preset</Link></Button></TableCell></TableRow>)}</TableBody></Table></div>
      </ResearchSection>; })}
    </>}
  </>;
}
