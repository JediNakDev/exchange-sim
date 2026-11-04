// Display helpers for the API's exact decimal strings (prices, cash, quantities,
// ids, nanosecond times). Everything here works on the decimal text or on BigInt.
// Nothing converts an exact value to an IEEE-754 number; chart floats live only
// in chart-derived-floats.ts.

const DECIMAL_TEXT_PATTERN = /^(-?)([0-9]+)(?:\.([0-9]+))?$/;

export interface ParsedDecimalText {
  isNegative: boolean;
  integerDigits: string;
  fractionDigits: string;
}

export function parseDecimalText(decimalText: string): ParsedDecimalText {
  const match = DECIMAL_TEXT_PATTERN.exec(decimalText.trim());
  if (!match) throw new Error(`not an exact decimal string: ${JSON.stringify(decimalText)}`);
  const integerDigits = match[2].replace(/^0+(?=[0-9])/, "");
  const fractionDigits = match[3] ?? "";
  const isZero = /^0*$/.test(integerDigits) && /^0*$/.test(fractionDigits);
  return { isNegative: match[1] === "-" && !isZero, integerDigits, fractionDigits };
}

export function isDecimalText(candidateText: string): boolean {
  return DECIMAL_TEXT_PATTERN.test(candidateText.trim());
}

/** Exact conversion to an integer count of 10^-scaleDigits units; throws if precision would be lost. */
export function decimalTextToScaledBigInt(decimalText: string, scaleDigits: number): bigint {
  const parsed = parseDecimalText(decimalText);
  if (parsed.fractionDigits.replace(/0+$/, "").length > scaleDigits) {
    throw new Error(`${decimalText} has more than ${scaleDigits} significant fractional digits`);
  }
  const magnitude = BigInt(parsed.integerDigits + parsed.fractionDigits.padEnd(scaleDigits, "0").slice(0, scaleDigits));
  return parsed.isNegative ? -magnitude : magnitude;
}

export function scaledBigIntToDecimalText(scaledValue: bigint, scaleDigits: number): string {
  const isNegative = scaledValue < 0n;
  const magnitudeDigits = (isNegative ? -scaledValue : scaledValue).toString().padStart(scaleDigits + 1, "0");
  const integerDigits = magnitudeDigits.slice(0, magnitudeDigits.length - scaleDigits);
  const fractionDigits = magnitudeDigits.slice(magnitudeDigits.length - scaleDigits);
  const body = scaleDigits === 0 ? integerDigits : `${integerDigits}.${fractionDigits}`;
  return isNegative ? `-${body}` : body;
}

function sharedScaleDigits(leftText: string, rightText: string): number {
  return Math.max(parseDecimalText(leftText).fractionDigits.length, parseDecimalText(rightText).fractionDigits.length);
}

export function compareDecimalTexts(leftText: string, rightText: string): -1 | 0 | 1 {
  const scaleDigits = sharedScaleDigits(leftText, rightText);
  const difference = decimalTextToScaledBigInt(leftText, scaleDigits) - decimalTextToScaledBigInt(rightText, scaleDigits);
  return difference < 0n ? -1 : difference > 0n ? 1 : 0;
}

export function subtractDecimalTexts(minuendText: string, subtrahendText: string): string {
  const scaleDigits = sharedScaleDigits(minuendText, subtrahendText);
  return scaledBigIntToDecimalText(decimalTextToScaledBigInt(minuendText, scaleDigits) - decimalTextToScaledBigInt(subtrahendText, scaleDigits), scaleDigits);
}

export function addDecimalTexts(leftText: string, rightText: string): string {
  const scaleDigits = sharedScaleDigits(leftText, rightText);
  return scaledBigIntToDecimalText(decimalTextToScaledBigInt(leftText, scaleDigits) + decimalTextToScaledBigInt(rightText, scaleDigits), scaleDigits);
}

export function decimalTextSign(decimalText: string): -1 | 0 | 1 {
  return compareDecimalTexts(decimalText, "0");
}

/** Rounds to `fractionDigitCount` digits, ties to even, on the decimal text itself. */
export function roundDecimalTextHalfEven(decimalText: string, fractionDigitCount: number): string {
  const parsed = parseDecimalText(decimalText);
  if (parsed.fractionDigits.length <= fractionDigitCount) {
    const padded = parsed.fractionDigits.padEnd(fractionDigitCount, "0");
    const body = fractionDigitCount === 0 ? parsed.integerDigits : `${parsed.integerDigits}.${padded}`;
    return parsed.isNegative ? `-${body}` : body;
  }
  const keptDigits = BigInt(parsed.integerDigits + parsed.fractionDigits.slice(0, fractionDigitCount));
  const droppedDigits = parsed.fractionDigits.slice(fractionDigitCount);
  const halfwayDigits = "5".padEnd(droppedDigits.length, "0");
  let roundedMagnitude = keptDigits;
  if (droppedDigits > halfwayDigits || (droppedDigits === halfwayDigits && keptDigits % 2n === 1n)) roundedMagnitude += 1n;
  return scaledBigIntToDecimalText(parsed.isNegative ? -roundedMagnitude : roundedMagnitude, fractionDigitCount);
}

function groupIntegerDigits(integerDigits: string): string {
  return integerDigits.replace(/\B(?=(\d{3})+(?!\d))/g, ",");
}

export interface DecimalDisplayOptions {
  /** Round (half-to-even) or pad to this many fractional digits. Omit to keep the API's digits. */
  fractionDigits?: number;
  groupThousands?: boolean;
  /** "always" prefixes "+" on positive values, useful for differences. */
  signDisplay?: "auto" | "always";
}

export function formatDecimalForDisplay(decimalText: string | null | undefined, options: DecimalDisplayOptions = {}): string {
  if (decimalText === null || decimalText === undefined) return "n/a";
  const roundedText = options.fractionDigits === undefined ? decimalText : roundDecimalTextHalfEven(decimalText, options.fractionDigits);
  const parsed = parseDecimalText(roundedText);
  const integerPart = options.groupThousands === false ? parsed.integerDigits : groupIntegerDigits(parsed.integerDigits);
  const body = parsed.fractionDigits.length > 0 ? `${integerPart}.${parsed.fractionDigits}` : integerPart;
  if (parsed.isNegative) return `-${body}`;
  const isZero = /^[0.,]*$/.test(body);
  return options.signDisplay === "always" && !isZero ? `+${body}` : body;
}

export function formatIntegerText(integerText: string | null | undefined): string {
  return formatDecimalForDisplay(integerText, { groupThousands: true });
}

/** Moves the decimal point left by `digitCount` places, exactly. */
export function shiftDecimalPointLeft(decimalText: string, digitCount: number): string {
  const parsed = parseDecimalText(decimalText);
  const scaledMagnitude = BigInt(parsed.integerDigits + parsed.fractionDigits);
  return scaledBigIntToDecimalText(parsed.isNegative ? -scaledMagnitude : scaledMagnitude, parsed.fractionDigits.length + digitCount);
}

const NANOSECONDS_PER_SECOND_DIGITS = 9;

/** Simulation timestamp in seconds, for example "12.345678 s" (exact, rounded half-to-even for display). */
export function formatSimulationTimeNs(nanosecondText: string | null | undefined, fractionDigits = 6): string {
  if (nanosecondText === null || nanosecondText === undefined) return "n/a";
  return `${formatDecimalForDisplay(shiftDecimalPointLeft(nanosecondText, NANOSECONDS_PER_SECOND_DIGITS), { fractionDigits })} s`;
}

/** Duration with an automatically chosen unit (ns, µs, ms, s), exact up to display rounding. */
export function formatDurationNs(nanosecondText: string | null | undefined): string {
  if (nanosecondText === null || nanosecondText === undefined) return "n/a";
  const magnitudeDigits = parseDecimalText(nanosecondText).integerDigits.length;
  const [shiftDigits, unitLabel] = magnitudeDigits > 9 ? [9, "s"] : magnitudeDigits > 6 ? [6, "ms"] : magnitudeDigits > 3 ? [3, "µs"] : [0, "ns"];
  const shiftedText = shiftDecimalPointLeft(nanosecondText, shiftDigits);
  const trimmedText = roundDecimalTextHalfEven(shiftedText, Math.min(3, shiftDigits)).replace(/(\.\d*?)0+$/, "$1").replace(/\.$/, "");
  return `${formatDecimalForDisplay(trimmedText)} ${unitLabel}`;
}

/** Converts seconds typed by a person ("1.5") to an exact nanosecond integer string. */
export function secondsTextToNanosecondText(secondsText: string): string {
  return decimalTextToScaledBigInt(secondsText, NANOSECONDS_PER_SECOND_DIGITS).toString();
}

/** Adds a signed integer offset to an unsigned integer string, clamped to [minimum, maximum]. */
export function offsetUnsignedIntegerText(integerText: string, offset: bigint, minimumText: string, maximumText: string): string {
  const shifted = BigInt(integerText) + offset;
  const minimum = BigInt(minimumText);
  const maximum = BigInt(maximumText);
  return (shifted < minimum ? minimum : shifted > maximum ? maximum : shifted).toString();
}

export function formatWallClockTime(rfc3339Text: string | null | undefined): string {
  if (!rfc3339Text) return "n/a";
  return rfc3339Text.replace("T", " ").replace(/\.\d+Z$/, "Z");
}
