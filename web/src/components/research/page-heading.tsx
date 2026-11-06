import type { ReactNode } from "react";

export function PageHeading({ title, description, actions }: { title: ReactNode; description?: ReactNode; actions?: ReactNode }) {
  return (
    <div className="research-page-heading">
      <div className="min-w-0">
        <h1>{title}</h1>
        {description ? <div className="page-description">{description}</div> : null}
      </div>
      {actions ? <div className="page-actions">{actions}</div> : null}
    </div>
  );
}
