import { Suspense } from "react";
import { ResearchCatalogView } from "@/components/research/research-catalog-view";
import { PageLoadingSkeleton } from "@/components/research/page-loading-skeleton";
export default function Page() { return <Suspense fallback={<PageLoadingSkeleton />}><ResearchCatalogView mode="scenarios" /></Suspense>; }
