import type { ComponentProps, ReactNode } from "react";
import { cn } from "@/lib/utils";

export type KeyFigure = { label: ReactNode; value: ReactNode; detail?: ReactNode; key?: string };

/** A ruled strip of 2 to 5 headline figures. Values are data, so they render in mono. */
export function KeyFigureList({ figures, className, ...listProps }: ComponentProps<"dl"> & { figures: KeyFigure[] }) {
  return (
    <dl className={cn("grid grid-cols-2 border-t border-l md:grid-cols-[repeat(auto-fit,minmax(11rem,1fr))]", className)} {...listProps}>
      {figures.map((figure, figureIndex) => (
        <div key={figure.key ?? (typeof figure.label === "string" ? figure.label : figureIndex)} className="flex min-w-0 flex-col gap-1 border-r border-b px-4 py-3">
          <dt className="text-xs text-muted-foreground">{figure.label}</dt>
          <dd className="font-mono text-lg tabular-nums wrap-anywhere">{figure.value}</dd>
          {figure.detail ? <dd className="text-xs text-muted-foreground">{figure.detail}</dd> : null}
        </div>
      ))}
    </dl>
  );
}
