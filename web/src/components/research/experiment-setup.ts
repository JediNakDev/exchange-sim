import type { ApiSchemas } from "@/lib/api/exsim-api-client";

export type ResearchIntent = "execution" | "market-making";
export type TimingUnit = "ns" | "µs" | "ms" | "s";
const UNIT_DIGITS: Record<TimingUnit, number> = { ns: 0, "µs": 3, ms: 6, s: 9 };

export function strategyIntent(kind: ApiSchemas["StrategyKind"]): ResearchIntent {
  return kind === "twap_execution" || kind === "immediate_sweep_execution" ? "execution" : "market-making";
}

/** Only selects service-provided baselines. Never invent a configuration for a missing task. */
export function baselineForIntent(baselines: ApiSchemas["BaselineConfiguration"][], intent: string | null) {
  return intent === "execution" || intent === "market-making"
    ? baselines.find((baseline) => strategyIntent(baseline.config.strategy.strategyKind) === intent) ?? null
    : baselines[0] ?? null;
}

/** Decimal point shifts only; no Number conversion, rounding or loss of nanoseconds. */
export function nanosecondsInUnit(value: string, unit: TimingUnit): string | null {
  if (!/^\d+$/.test(value)) return null;
  const digits = UNIT_DIGITS[unit];
  if (digits === 0) return value;
  const padded = value.padStart(digits + 1, "0");
  const fraction = padded.slice(-digits).replace(/0+$/, "");
  return `${BigInt(padded.slice(0, -digits))}${fraction ? `.${fraction}` : ""}`;
}

export function unitValueInNanoseconds(value: string, unit: TimingUnit): string | null {
  if (!/^\d+(?:\.\d+)?$/.test(value)) return null;
  const [integer, fraction = ""] = value.split(".");
  const digits = UNIT_DIGITS[unit];
  if (fraction.slice(digits).replace(/0/g, "") !== "") return null;
  return BigInt(integer + fraction.slice(0, digits).padEnd(digits, "0")).toString();
}

export function exactDurationDescription(value: string): string | undefined {
  if (!/^(0|[1-9][0-9]{0,19})$/.test(value)) return undefined;
  const unit: TimingUnit = value.length > 9 ? "s" : value.length > 6 ? "ms" : value.length > 3 ? "µs" : "ns";
  return `${nanosecondsInUnit(value, unit)} ${unit}`;
}

/** A variant always starts from its saved parent, independently of task intent. */
export function draftFromExperiment(parent: ApiSchemas["Experiment"]) {
  return {
    baselineId: parent.derivedFromBaselineId,
    name: `${parent.name} variant`,
    description: parent.description ?? "",
    parentExperimentId: parent.experimentId,
    config: structuredClone(parent.config),
    masterSeedsText: parent.config.seedSet.masterSeeds.join(", "),
    markoutHorizonsText: parent.config.markoutHorizonsNs.join(", "),
  };
}
