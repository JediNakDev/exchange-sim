import { Suspense } from "react";
import { PageLoadingSkeleton } from "@/components/research/page-loading-skeleton";
import { ExperimentCreateView } from "@/components/research/experiment-create-view";

export default function NewExperimentPage() {
  return (
    <Suspense fallback={<PageLoadingSkeleton />}>
      <ExperimentCreateView />
    </Suspense>
  );
}
