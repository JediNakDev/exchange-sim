import { useId, type ComponentProps, type ReactNode } from "react";
import { cn } from "@/lib/utils";

type ResearchSectionProps = Omit<ComponentProps<"section">, "title"> & {
  title: ReactNode;
  description?: ReactNode;
  actions?: ReactNode;
};

/** A page section: h2 title, optional description and actions, then content. See UI design section 5. */
export function ResearchSection({ title, description, actions, className, children, ...sectionProps }: ResearchSectionProps) {
  const headingId = useId();
  // An explicit aria-label names the region instead of the visible heading, because aria-labelledby would override it.
  const sectionIsNamedByHeading = sectionProps["aria-label"] === undefined && sectionProps["aria-labelledby"] === undefined;
  return (
    <section aria-labelledby={sectionIsNamedByHeading ? headingId : undefined} className={cn("flex min-w-0 flex-col gap-3", className)} {...sectionProps}>
      <div className="flex flex-wrap items-end justify-between gap-x-4 gap-y-2">
        <div className="flex min-w-0 flex-col gap-1">
          <h2 id={headingId} className="text-base font-medium">{title}</h2>
          {description ? <div className="max-w-prose text-xs/relaxed text-muted-foreground">{description}</div> : null}
        </div>
        {actions ? <div className="flex flex-wrap items-center gap-2">{actions}</div> : null}
      </div>
      {children}
    </section>
  );
}
