// Independently seeded random streams for the mock fixture generator.
// Derivation: derivedSeed = splitmix64(masterSeed XOR streamIndex); each
// stream is a mulberry32 generator seeded with the low 32 bits.

const UINT64_MASK = (1n << 64n) - 1n;

export const RANDOM_STREAM_NAMES = [
  "background_market_maker",
  "background_noise_taker",
  "strategy",
  "latency_jitter",
] as const;
export type RandomStreamName = (typeof RANDOM_STREAM_NAMES)[number];

export const SEED_DERIVATION_FUNCTION_DESCRIPTION =
  "derivedSeed = splitmix64(masterSeed XOR streamIndex) with streamIndex = position in [background_market_maker, background_noise_taker, strategy, latency_jitter]; mulberry32 seeded with the low 32 bits";

export function splitmix64(inputValue: bigint): bigint {
  let mixedValue = (inputValue + 0x9e3779b97f4a7c15n) & UINT64_MASK;
  mixedValue = ((mixedValue ^ (mixedValue >> 30n)) * 0xbf58476d1ce4e5b9n) & UINT64_MASK;
  mixedValue = ((mixedValue ^ (mixedValue >> 27n)) * 0x94d049bb133111ebn) & UINT64_MASK;
  return mixedValue ^ (mixedValue >> 31n);
}

export function deriveStreamSeed(masterSeed: bigint, streamName: RandomStreamName): bigint {
  return splitmix64((masterSeed ^ BigInt(RANDOM_STREAM_NAMES.indexOf(streamName))) & UINT64_MASK);
}

export class SeededRandomStream {
  private generatorState: number;

  constructor(derivedSeed: bigint) {
    this.generatorState = Number(derivedSeed & 0xffffffffn) >>> 0;
  }

  /** Uniform float in [0, 1). */
  nextUniform(): number {
    this.generatorState = (this.generatorState + 0x6d2b79f5) >>> 0;
    let mixed = this.generatorState;
    mixed = Math.imul(mixed ^ (mixed >>> 15), mixed | 1);
    mixed ^= mixed + Math.imul(mixed ^ (mixed >>> 7), mixed | 61);
    return ((mixed ^ (mixed >>> 14)) >>> 0) / 4294967296;
  }

  /** Uniform integer in [minimumInclusive, maximumInclusive]. */
  nextIntegerInclusive(minimumInclusive: number, maximumInclusive: number): number {
    return minimumInclusive + Math.floor(this.nextUniform() * (maximumInclusive - minimumInclusive + 1));
  }

  /** Exponential waiting time with the given mean, truncated to whole nanoseconds (at least 1). */
  nextExponentialNanoseconds(meanNanoseconds: bigint): bigint {
    const waitingTime = -Math.log(1 - this.nextUniform()) * Number(meanNanoseconds);
    return BigInt(Math.max(1, Math.floor(waitingTime)));
  }
}
