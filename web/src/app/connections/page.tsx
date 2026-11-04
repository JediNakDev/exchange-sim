import { Suspense } from "react";
import { PageLoadingSkeleton } from "@/components/research/page-loading-skeleton";
import { ConnectionsView } from "@/components/research/connections-view";

export default function ConnectionsPage() {
  return (
    <Suspense fallback={<PageLoadingSkeleton />}>
      <ConnectionsView />
    </Suspense>
  );
}
