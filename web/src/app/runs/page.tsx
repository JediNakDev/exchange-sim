import { Suspense } from "react";
import { RunListView } from "@/components/research/run-list-view";
import { PageLoadingSkeleton } from "@/components/research/page-loading-skeleton";
export default function Page() { return <Suspense fallback={<PageLoadingSkeleton />}><RunListView /></Suspense>; }
