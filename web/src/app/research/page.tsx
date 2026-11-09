import { Suspense } from "react";
import { ResearchHomeView } from "@/components/research/research-home-view";
import { PageLoadingSkeleton } from "@/components/research/page-loading-skeleton";
export default function ResearchPage() { return <Suspense fallback={<PageLoadingSkeleton />}><ResearchHomeView /></Suspense>; }
