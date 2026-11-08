import { describe, expect, test } from "bun:test";
import {
  addDecimalTexts,
  compareDecimalTexts,
  decimalTextToScaledBigInt,
  formatDecimalForDisplay,
  formatDurationNs,
  formatSimulationTimeNs,
  offsetUnsignedIntegerText,
  roundDecimalTextHalfEven,
  scaledBigIntToDecimalText,
  secondsTextToNanosecondText,
  shiftDecimalPointLeft,
  subtractDecimalTexts,
} from "../src/lib/exact-decimal-format";

describe("exact decimal parsing and arithmetic", () => {
  test("scaled conversion is exact and refuses precision loss", () => {
    expect(decimalTextToScaledBigInt("100.0100", 4)).toBe(1_000_100n);
    expect(decimalTextToScaledBigInt("-0.0020", 4)).toBe(-20n);
    expect(decimalTextToScaledBigInt("12.5", 4)).toBe(125_000n);
    expect(decimalTextToScaledBigInt("1.50000", 4)).toBe(15_000n);
    expect(() => decimalTextToScaledBigInt("0.00001", 4)).toThrow();
    expect(() => decimalTextToScaledBigInt("1e3", 4)).toThrow();
  });

  test("values beyond 2^53 survive round trips", () => {
    const largeCash = "92233720368547.7580";
    expect(scaledBigIntToDecimalText(decimalTextToScaledBigInt(largeCash, 4), 4)).toBe(largeCash);
    expect(addDecimalTexts(largeCash, "0.0001")).toBe("92233720368547.7581");
    expect(formatDecimalForDisplay("18446744073709551615")).toBe("18,446,744,073,709,551,615");
  });

  test("subtraction and comparison align scales", () => {
    expect(subtractDecimalTexts("100.00500", "100.0100")).toBe("-0.00500");
    expect(subtractDecimalTexts("-1.5", "-1.5")).toBe("0.0");
    expect(compareDecimalTexts("0.10", "0.1")).toBe(0);
    expect(compareDecimalTexts("-0.0001", "0")).toBe(-1);
    expect(compareDecimalTexts("99.9999", "100")).toBe(-1);
  });
});

describe("display rounding is half-to-even on the text", () => {
  test.each([
    ["0.125", 2, "0.12"],
    ["0.135", 2, "0.14"],
    ["0.1251", 2, "0.13"],
    ["-2.5", 0, "-2"],
    ["-3.5", 0, "-4"],
    ["2.675", 2, "2.68"],
    ["1.005", 2, "1.00"],
    ["9.995", 2, "10.00"],
    ["7", 3, "7.000"],
  ])("%s to %i digits is %s", (decimalText, fractionDigits, expectedText) => {
    expect(roundDecimalTextHalfEven(decimalText, fractionDigits)).toBe(expectedText);
  });

  test("formatting groups thousands, keeps API digits and shows signs on request", () => {
    expect(formatDecimalForDisplay("1000000.0000")).toBe("1,000,000.0000");
    expect(formatDecimalForDisplay("-19888.80000", { fractionDigits: 2 })).toBe("-19,888.80");
    expect(formatDecimalForDisplay("12.34567", { signDisplay: "always" })).toBe("+12.34567");
    expect(formatDecimalForDisplay("0.0000", { signDisplay: "always" })).toBe("0.0000");
    expect(formatDecimalForDisplay("-0.0000")).toBe("0.0000");
    expect(formatDecimalForDisplay(null)).toBe("n/a");
  });
});

describe("nanosecond helpers", () => {
  test("simulation time and durations are exact shifts", () => {
    expect(shiftDecimalPointLeft("92028921", 9)).toBe("0.092028921");
    expect(formatSimulationTimeNs("60000000000", 3)).toBe("60.000 s");
    expect(formatSimulationTimeNs("92028921")).toBe("0.092029 s");
    expect(formatDurationNs("1500000")).toBe("1.5 ms");
    expect(formatDurationNs("4000000")).toBe("4 ms");
    expect(formatDurationNs("30000000000")).toBe("30 s");
    expect(formatDurationNs("750")).toBe("750 ns");
    expect(formatDurationNs("2500")).toBe("2.5 µs");
    expect(secondsTextToNanosecondText("1.5")).toBe("1500000000");
  });

  test("event position stepping clamps without floats", () => {
    expect(offsetUnsignedIntegerText("3", -25n, "0", "3565")).toBe("0");
    expect(offsetUnsignedIntegerText("3560", 25n, "0", "3565")).toBe("3565");
    expect(offsetUnsignedIntegerText("18446744073709551614", 1n, "0", "18446744073709551615")).toBe("18446744073709551615");
  });
});
