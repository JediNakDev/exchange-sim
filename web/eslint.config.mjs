import { defineConfig, globalIgnores } from "eslint/config";
import nextVitals from "eslint-config-next/core-web-vitals";
import nextTs from "eslint-config-next/typescript";

// Exact decimal strings from the API must never become IEEE-754 numbers in the
// UI. Plot positions are the one exception and live in chart-derived-floats.ts.
const exactDecimalRestrictions = [
  { selector: "CallExpression[callee.name='Number']", message: "Do not convert API decimal strings with Number(); use src/lib/exact-decimal-format.ts, or src/lib/chart-derived-floats.ts for plot positions." },
  { selector: "CallExpression[callee.name='parseFloat']", message: "Do not parse API decimal strings as floats; use src/lib/exact-decimal-format.ts." },
  { selector: "CallExpression[callee.object.name='Number'][callee.property.name='parseFloat']", message: "Do not parse API decimal strings as floats; use src/lib/exact-decimal-format.ts." },
];

const eslintConfig = defineConfig([
  ...nextVitals,
  ...nextTs,
  {
    files: ["src/**/*.{ts,tsx}"],
    ignores: ["src/lib/chart-derived-floats.ts"],
    rules: { "no-restricted-syntax": ["error", ...exactDecimalRestrictions] },
  },
  globalIgnores([".next/**", "out/**", "build/**", "next-env.d.ts", "src/lib/api/generated-schema.d.ts"]),
]);

export default eslintConfig;
