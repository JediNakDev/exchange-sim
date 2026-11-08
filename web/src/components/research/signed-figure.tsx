import { cn } from "@/lib/utils";

export type FavourableDirection = "higher" | "lower" | "neutral";

const SIGN_TONE_BY_FAVOURABILITY = { favourable: "text-positive", unfavourable: "text-negative", neutral: "text-muted-foreground" } as const;

function signTone(sign: string, favourableDirection: FavourableDirection): keyof typeof SIGN_TONE_BY_FAVOURABILITY {
  if (favourableDirection === "neutral" || sign === "") return "neutral";
  const isPositive = sign === "+";
  return isPositive === (favourableDirection === "higher") ? "favourable" : "unfavourable";
}

/**
 * Renders an already formatted exact decimal string with a coloured sign and ink digits.
 * Inline spans only, so textContent, innerText and the accessible name equal the input string.
 * The string is never parsed as a number. Text that is not a signed decimal renders unchanged.
 */
export function SignedFigure({ formattedValue, favourableDirection = "higher", className }: { formattedValue: string; favourableDirection?: FavourableDirection; className?: string }) {
  const leadingCharacter = formattedValue.charAt(0);
  const hasSign = leadingCharacter === "+" || leadingCharacter === "-";
  const isDecimal = /^[+-]?[0-9]/.test(formattedValue);
  if (!isDecimal) return <span className={cn("font-sans text-muted-foreground", className)}>{formattedValue}</span>;
  const sign = hasSign ? leadingCharacter : "";
  return (
    <span className={cn("font-mono wrap-anywhere tabular-nums", className)}>
      {sign ? <span className={cn("font-medium", SIGN_TONE_BY_FAVOURABILITY[signTone(sign, favourableDirection)])}>{sign}</span> : null}
      {hasSign ? formattedValue.slice(1) : formattedValue}
    </span>
  );
}
