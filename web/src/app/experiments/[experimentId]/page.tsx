import { Suspense } from "react";
import { PageLoadingSkeleton } from "@/components/research/page-loading-skeleton";
import { ExperimentDetailView } from "@/components/research/experiment-detail-view";

export default function ExperimentDetailPage() {
  return (
    <Suspense fallback={<PageLoadingSkeleton />}>
      <ExperimentDetailView />
    </Suspense>
  );
}
