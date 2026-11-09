// Exact fixed-point helpers for the mock fixture generator. Every exact
// amount is a bigint count of 10^-scale units, mirroring the C++ int64
// fixed-point ledger. Only derived statistics (means, deviations) use floats.

export const PRICE_SCALE_DIGITS = 4;
export const MID_SCALE_DIGITS = 5;
export const DERIVED_STATISTIC_SCALE_DIGITS = 6;

const EXACT_DECIMAL_PATTERN = /^(-?)(0|[1-9][0-9]*)(?:\.([0-9]+))?$/;

export function formatScaledInteger(scaledValue: bigint, scaleDigits: number): string {
  const isNegative = scaledValue < 0n;
  const absoluteDigits = (isNegative ? -scaledValue : scaledValue).toString().padStart(scaleDigits + 1, "0");
  const integerPart = absoluteDigits.slice(0, absoluteDigits.length - scaleDigits);
  const fractionPart = absoluteDigits.slice(absoluteDigits.length - scaleDigits);
  const unsignedText = scaleDigits === 0 ? integerPart : `${integerPart}.${fractionPart}`;
  return isNegative ? `-${unsignedText}` : unsignedText;
}

/** Parses an exact decimal string into units of 10^-scaleDigits; throws if precision would be lost. */
export function parseScaledDecimal(decimalText: string, scaleDigits: number): bigint {
  const match = EXACT_DECIMAL_PATTERN.exec(decimalText);
  if (!match) throw new Error(`not an exact decimal string: ${decimalText}`);
  const [, signText, integerText, fractionText = ""] = match;
  const significantFraction = fractionText.replace(/0+$/, "");
  if (significantFraction.length > scaleDigits) {
    throw new Error(`${decimalText} has more than ${scaleDigits} fractional digits`);
  }
  const scaledMagnitude = BigInt(integerText + fractionText.padEnd(scaleDigits, "0").slice(0, scaleDigits));
  return signText === "-" ? -scaledMagnitude : scaledMagnitude;
}

/** Scale of an exact decimal string (number of fractional digits). */
export function countFractionDigits(decimalText: string): number {
  const match = EXACT_DECIMAL_PATTERN.exec(decimalText);
  if (!match) throw new Error(`not an exact decimal string: ${decimalText}`);
  return (match[3] ?? "").length;
}

/** Exact candidate - baseline for two decimal strings, keeping the larger scale. */
export function subtractExactDecimals(minuendText: string, subtrahendText: string): string {
  const scaleDigits = Math.max(countFractionDigits(minuendText), countFractionDigits(subtrahendText));
  return formatScaledInteger(
    parseScaledDecimal(minuendText, scaleDigits) - parseScaledDecimal(subtrahendText, scaleDigits),
    scaleDigits,
  );
}

export function compareExactDecimals(leftText: string, rightText: string): number {
  const scaleDigits = Math.max(countFractionDigits(leftText), countFractionDigits(rightText));
  const difference = parseScaledDecimal(leftText, scaleDigits) - parseScaledDecimal(rightText, scaleDigits);
  return difference === 0n ? 0 : difference < 0n ? -1 : 1;
}

function roundHalfToEven(value: number): number {
  const floorValue = Math.floor(value);
  const remainder = value - floorValue;
  if (remainder > 0.5) return floorValue + 1;
  if (remainder < 0.5) return floorValue;
  return floorValue % 2 === 0 ? floorValue : floorValue + 1;
}

/** Formats a derived float statistic as a 6-digit DerivedDecimal, rounding half-to-even. */
export function formatDerivedStatistic(derivedValue: number): string {
  if (!Number.isFinite(derivedValue)) throw new Error("derived statistic is not finite");
  const scaledValue = roundHalfToEven(derivedValue * 10 ** DERIVED_STATISTIC_SCALE_DIGITS);
  return formatScaledInteger(BigInt(scaledValue === 0 ? 0 : scaledValue), DERIVED_STATISTIC_SCALE_DIGITS);
}

/** Converts an exact decimal string to a float, for derived statistics only. */
export function exactDecimalToDerivedFloat(decimalText: string): number {
  return Number(decimalText);
}

export function floorDivide(dividend: bigint, divisor: bigint): bigint {
  const quotient = dividend / divisor;
  return (dividend % divisor !== 0n && (dividend < 0n) !== (divisor < 0n)) ? quotient - 1n : quotient;
}

export function ceilDivide(dividend: bigint, divisor: bigint): bigint {
  return -floorDivide(-dividend, divisor);
}
