import { Badge } from "@/components/ui/badge";
import type { ApiSchemas } from "@/lib/api/exsim-api-client";

const BADGE_VARIANT_BY_RUN_STATE: Record<ApiSchemas["RunState"], "default" | "secondary" | "destructive" | "outline"> = {
  queued: "outline",
  running: "default",
  paused: "secondary",
  completed: "secondary",
  cancelled: "outline",
  failed: "destructive",
};

export const TERMINAL_RUN_STATES: ApiSchemas["RunState"][] = ["completed", "cancelled", "failed"];

export function isTerminalRunState(runState: ApiSchemas["RunState"]): boolean {
  return TERMINAL_RUN_STATES.includes(runState);
}

export function RunStateBadge({ runState }: { runState: ApiSchemas["RunState"] }) {
  return <Badge className="run-state" data-state={runState} variant={BADGE_VARIANT_BY_RUN_STATE[runState]}>{runState}</Badge>;
}
