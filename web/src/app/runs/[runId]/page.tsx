import { Suspense } from "react";
import { PageLoadingSkeleton } from "@/components/research/page-loading-skeleton";
import { RunDetailView } from "@/components/research/run-detail-view";

export default function RunDetailPage() {
  return (
    <Suspense fallback={<PageLoadingSkeleton />}>
      <RunDetailView />
    </Suspense>
  );
}
