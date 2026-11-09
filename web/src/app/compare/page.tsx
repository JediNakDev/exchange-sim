import { Suspense } from "react";
import { PageLoadingSkeleton } from "@/components/research/page-loading-skeleton";
import { ExperimentCompareView } from "@/components/research/experiment-compare-view";

export default function ComparePage() {
  return (
    <Suspense fallback={<PageLoadingSkeleton />}>
      <ExperimentCompareView />
    </Suspense>
  );
}
