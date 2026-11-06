// Baseline configurations served by the mock API, plus the seeded fixture
// experiments of the first vertical slice: the fixed-spread strategy at two
// quote widths on the same five master seeds.

import type { components } from "../src/lib/api/generated-schema";

type Schemas = components["schemas"];

export const FIRST_SLICE_MASTER_SEEDS = ["101", "202", "303", "404", "505"];

export function createFixedSpreadBaselineConfig(halfSpreadTicks: string): Schemas["ExperimentConfig"] {
  return {
    marketWorld: "interactive_synthetic",
    instrument: {
      symbol: "EXSM",
      tickSize: "0.0100",
      lotSize: "1",
      maxOrderQuantity: "10000",
      minPrice: "0.0100",
      maxPrice: "1000.0000",
      referencePrice: "100.0000",
      contractMultiplier: "1",
    },
    scenario: {
      scenarioId: "synthetic.mm-and-noise-takers.v1",
      description: "Background market maker quoting two levels around a random-walk fair value; partially informed noise takers send market orders.",
      backgroundAgents: [
        {
          agentKind: "market_maker",
          agentCount: 1,
          parameters: [
            { name: "fairValueStepIntervalNs", value: "500000000", unit: "nanoseconds", description: "Interval between fair value steps and market maker requotes." },
            { name: "fairValueMoveProbability", value: "0.5", unit: "ratio", description: "Probability that the fair value moves one tick at a step." },
            { name: "bestLevelShares", value: "300", unit: "shares", description: "Quantity quoted one tick from the fair value." },
            { name: "deeperLevelShares", value: "500", unit: "shares", description: "Quantity quoted two ticks from the fair value." },
          ],
        },
        {
          agentKind: "noise_taker",
          agentCount: 1,
          parameters: [
            { name: "meanInterarrivalNs", value: "350000000", unit: "nanoseconds", description: "Mean of exponential waiting time between market orders." },
            { name: "minimumOrderLots", value: "1", unit: "count", description: "Smallest market order in lots." },
            { name: "maximumOrderLots", value: "6", unit: "count", description: "Largest market order in lots." },
            { name: "sharesPerLot", value: "100", unit: "shares", description: "Shares per taker lot." },
            { name: "informedProbability", value: "0.6", unit: "ratio", description: "Probability a taker trades in the direction of the next fair value move." },
          ],
        },
      ],
    },
    strategy: {
      strategyKind: "fixed_spread_market_maker",
      halfSpreadTicks,
      quoteQuantity: "100",
      requoteIntervalNs: "0",
      requoteThresholdTicks: "1",
      postOnly: true,
    },
    seedSet: { masterSeeds: [...FIRST_SLICE_MASTER_SEEDS] },
    latency: {
      latencyModel: "constant",
      feedLatencyNs: "2000000",
      orderEntryLatencyNs: "1500000",
      acknowledgmentLatencyNs: "1000000",
      cancelLatencyNs: "4000000",
      jitterNs: "0",
    },
    fees: { feeScheduleId: "maker-rebate-20-taker-30", makerFeePerShare: "-0.0020", takerFeePerShare: "0.0030" },
    risk: { maxAbsolutePositionShares: "500", maxOpenOrders: "4", maxOrderQuantity: "1000", onBreach: "block_orders_that_increase_exposure" },
    initialCash: "1000000.0000",
    markConvention: "exchange_mid",
    markoutHorizonsNs: ["1000000000", "5000000000", "30000000000"],
    bounds: { endSimulationTimeNs: "60000000000", maxCanonicalEventCount: "1000000", wallClockTimeLimitSeconds: 600 },
  };
}

export const BASELINE_CONFIGURATIONS: Schemas["BaselineConfiguration"][] = [
  {
    baselineId: "fixed-spread-2-ticks",
    name: "Fixed spread, 2-tick width",
    description: "Quotes one tick either side of the strategy-visible mid, joining the background market maker at the touch.",
    config: createFixedSpreadBaselineConfig("1"),
  },
  {
    baselineId: "fixed-spread-4-ticks",
    name: "Fixed spread, 4-tick width",
    description: "Quotes two ticks either side of the strategy-visible mid, one level behind the touch.",
    config: createFixedSpreadBaselineConfig("2"),
  },
];
