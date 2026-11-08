import type { ReactNode } from "react";
import { cn } from "@/lib/utils";

/** Label and value facts in two columns. Values render in mono unless `valuesAreData` is false. */
export function DefinitionList({ rows, valuesAreData = true, className }: { rows: [string, ReactNode][]; valuesAreData?: boolean; className?: string }) {
  return (
    <dl className={cn("grid grid-cols-[minmax(0,max-content)_minmax(0,1fr)] gap-x-6 gap-y-1.5 text-xs", className)}>
      {rows.map(([label, value]) => (
        <div key={label} className="contents">
          <dt className="text-muted-foreground">{label}</dt>
          <dd className={cn("break-all tabular-nums", valuesAreData && "font-mono")}>{value}</dd>
        </div>
      ))}
    </dl>
  );
}
