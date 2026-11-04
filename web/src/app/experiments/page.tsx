import { Suspense } from "react";
import { PageLoadingSkeleton } from "@/components/research/page-loading-skeleton";
import { ExperimentListView } from "@/components/research/experiment-list-view";

export default function ExperimentsPage() {
  return (
    <Suspense fallback={<PageLoadingSkeleton />}>
      <ExperimentListView />
    </Suspense>
  );
}
