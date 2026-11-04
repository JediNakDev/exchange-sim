import { Suspense } from "react";
import { PageLoadingSkeleton } from "@/components/research/page-loading-skeleton";
import { RunInspectView } from "@/components/research/run-inspect-view";

export default function RunInspectPage() {
  return (
    <Suspense fallback={<PageLoadingSkeleton />}>
      <RunInspectView />
    </Suspense>
  );
}
