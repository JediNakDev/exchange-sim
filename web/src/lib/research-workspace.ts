import type { ApiSchemas } from "@/lib/api/exsim-api-client";

export const STRATEGY_NAMES: Record<ApiSchemas["StrategyKind"], string> = {
  twap_execution: "TWAP execution",
  immediate_sweep_execution: "Immediate sweep",
  fixed_spread_market_maker: "Fixed-spread market maker",
  inventory_aware_market_maker: "Inventory-aware market maker",
};

/** Follow persisted variant lineage. Missing ancestors stay visible; cycles have a stable root. */
export function researchRoot(experiment: ApiSchemas["Experiment"], experiments: ApiSchemas["Experiment"][]) {
  const byId = new Map(experiments.map((item) => [item.experimentId, item]));
  const visited = new Map<string, number>();
  const path: string[] = [];
  let current = experiment;
  while (current.parentExperimentId && byId.has(current.parentExperimentId)) {
    visited.set(current.experimentId, path.length);
    path.push(current.experimentId);
    const parent = byId.get(current.parentExperimentId)!;
    if (visited.has(parent.experimentId)) return path.slice(visited.get(parent.experimentId)!).sort()[0];
    current = parent;
  }
  return current.experimentId;
}

export function researchStudies(experiments: ApiSchemas["Experiment"][]) {
  const groups = new Map<string, ApiSchemas["Experiment"][]>();
  for (const experiment of experiments) {
    const root = researchRoot(experiment, experiments);
    groups.set(root, [...(groups.get(root) ?? []), experiment]);
  }
  return [...groups].map(([id, configurations]) => ({
    baseline: configurations.find((item) => item.experimentId === id)!,
    configurations,
    updatedAt: configurations.reduce((latest, item) => item.createdAt > latest ? item.createdAt : latest, ""),
  })).sort((a, b) => b.updatedAt.localeCompare(a.updatedAt));
}

/** Values remain exact strings; a changed array is one explicit assumption difference. */
export function configurationChanges(before: unknown, after: unknown, path = ""): { path: string; before: string; after: string }[] {
  if (JSON.stringify(before) === JSON.stringify(after)) return [];
  if (before !== null && after !== null && typeof before === "object" && typeof after === "object" && !Array.isArray(before) && !Array.isArray(after)) {
    const a = before as Record<string, unknown>, b = after as Record<string, unknown>;
    return [...new Set([...Object.keys(a), ...Object.keys(b)])].flatMap((key) => configurationChanges(a[key], b[key], path ? `${path}.${key}` : key));
  }
  return [{ path, before: before === undefined ? "Not applicable" : typeof before === "string" ? before : JSON.stringify(before), after: after === undefined ? "Not applicable" : typeof after === "string" ? after : JSON.stringify(after) }];
}
