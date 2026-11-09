import { describe, expect, test } from "bun:test";
import type { ApiSchemas } from "../../src/lib/api/exsim-api-client";
import { BASELINE_CONFIGURATIONS } from "../mock-server/baseline-catalog";
import { baselineForIntent, draftFromExperiment, exactDurationDescription, nanosecondsInUnit, strategyIntent, unitValueInNanoseconds, type TimingUnit } from "../../src/components/research/experiment-setup";

describe("task selection uses executable service baselines", () => {
  const maker = BASELINE_CONFIGURATIONS[0];
  const execution = structuredClone(maker);
  execution.baselineId = "execution";
  execution.config.strategy = { strategyKind: "immediate_sweep_execution", parentSide: "buy", parentQuantity: "9007199254740993", startTimeNs: "1000000001", executionDurationNs: "30000000000", limitOffsetTicks: "5" };
  test("intent picks the requested family without changing any config strings", () => {
    const before = JSON.stringify([maker, execution]);
    expect(baselineForIntent([maker, execution], "execution")).toBe(execution);
    expect(baselineForIntent([execution, maker], "market-making")).toBe(maker);
    expect(JSON.stringify([maker, execution])).toBe(before);
  });
  test("missing capability is unavailable instead of silently choosing another task", () => {
    expect(baselineForIntent([maker], "execution")).toBeNull();
    expect(baselineForIntent([], "market-making")).toBeNull();
    expect(baselineForIntent([maker], null)).toBe(maker);
    expect(baselineForIntent([maker], "unknown")).toBe(maker);
  });
  test("all editable strategies map to their task", () => {
    expect(strategyIntent("twap_execution")).toBe("execution");
    expect(strategyIntent("immediate_sweep_execution")).toBe("execution");
    expect(strategyIntent("fixed_spread_market_maker")).toBe("market-making");
    expect(strategyIntent("inventory_aware_market_maker")).toBe("market-making");
  });
});

describe("exact setup timing", () => {
  test("unit changes round trip full uint64 values and one-nanosecond fractions", () => {
    for (const value of ["0", "1", "999999", "1000000001", "9007199254740993", "18446744073709551615"]) {
      for (const unit of ["ns", "µs", "ms", "s"] as TimingUnit[]) {
        expect(unitValueInNanoseconds(nanosecondsInUnit(value, unit)!, unit)).toBe(value);
      }
    }
  });
  test("typed decimal durations convert without floating point", () => {
    expect(unitValueInNanoseconds("0.000000001", "s")).toBe("1");
    expect(unitValueInNanoseconds("1.500001", "ms")).toBe("1500001");
    expect(unitValueInNanoseconds("1.0000000000", "s")).toBe("1000000000");
    expect(nanosecondsInUnit("1500001", "ms")).toBe("1.500001");
  });
  test("rejects sub-nanosecond precision and malformed drafts without rounding", () => {
    for (const value of ["", "-1", "1e9", "NaN", " 1", "1.", ".5", "0.0000000001"]) {
      expect(unitValueInNanoseconds(value, "s")).toBeNull();
    }
    expect(unitValueInNanoseconds("1.1", "ns")).toBeNull();
    expect(nanosecondsInUnit("bad", "ms")).toBeNull();
    expect(exactDurationDescription("bad")).toBeUndefined();
  });
  test("readable descriptions preserve the last nanosecond", () => {
    expect(exactDurationDescription("1000000001")).toBe("1.000000001 s");
    expect(exactDurationDescription("1500001")).toBe("1.500001 ms");
    expect(exactDurationDescription("0")).toBe("0 ns");
  });
});

test("variant draft copies the saved configuration exactly and owns independent edits", () => {
  const config = structuredClone(BASELINE_CONFIGURATIONS[0].config);
  config.seedSet.masterSeeds = ["18446744073709551615"];
  config.latency.feedLatencyNs = "9007199254740993";
  config.fees.makerFeePerShare = "-0.00000001";
  const parent = { experimentId: "saved", derivedFromBaselineId: "original", name: "Saved assumptions", config } as ApiSchemas["Experiment"];
  const draft = draftFromExperiment(parent);
  expect(draft.config).toEqual(config);
  expect(draft.config).not.toBe(config);
  expect(draft.parentExperimentId).toBe("saved");
  expect(draft.baselineId).toBe("original");
  expect(draft.masterSeedsText).toBe("18446744073709551615");
  expect(draft.markoutHorizonsText).toBe(config.markoutHorizonsNs.join(", "));
  draft.config.latency.feedLatencyNs = "1";
  expect(config.latency.feedLatencyNs).toBe("9007199254740993");
});
