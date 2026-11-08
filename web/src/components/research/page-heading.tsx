import type { ReactNode } from "react";

export function PageHeading({ title, description, actions }: { title: ReactNode; description?: ReactNode; actions?: ReactNode }) {
  return (
    <header className="flex flex-col gap-4 lg:flex-row lg:items-end lg:justify-between">
      <div className="flex min-w-0 flex-col gap-1.5">
        <h1 className="flex flex-wrap items-center gap-3 text-2xl font-semibold tracking-tight wrap-anywhere">{title}</h1>
        {description ? <div className="max-w-prose text-sm text-muted-foreground">{description}</div> : null}
      </div>
      {actions ? <div className="flex shrink-0 flex-wrap items-center gap-2">{actions}</div> : null}
    </header>
  );
}
