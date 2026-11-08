import { Badge } from "@/components/ui/badge";
import type { ApiSchemas } from "@/lib/api/exsim-api-client";
import { cn } from "@/lib/utils";

const STATE_DOT_CLASS_BY_RUN_STATE: Record<ApiSchemas["RunState"], string> = {
  queued: "bg-muted-foreground",
  running: "bg-primary animate-pulse",
  paused: "bg-warning",
  completed: "bg-positive",
  cancelled: "bg-muted-foreground",
  failed: "bg-negative",
};

export const TERMINAL_RUN_STATES: ApiSchemas["RunState"][] = ["completed", "cancelled", "failed"];

export function isTerminalRunState(runState: ApiSchemas["RunState"]): boolean {
  return TERMINAL_RUN_STATES.includes(runState);
}

export function RunStateDot({ runState }: { runState: ApiSchemas["RunState"] }) {
  return <span aria-hidden="true" className={cn("size-1.5 shrink-0 rounded-full", STATE_DOT_CLASS_BY_RUN_STATE[runState])} />;
}

export function RunStateBadge({ runState, children }: { runState: ApiSchemas["RunState"]; children?: React.ReactNode }) {
  return (
    <Badge variant="outline" data-state={runState} className={cn("font-normal", runState === "failed" && "border-negative/40 text-negative")}>
      <RunStateDot runState={runState} />
      {children ?? runState}
    </Badge>
  );
}
