"use client";

import Link from "next/link";
import { usePathname } from "next/navigation";
import { cn } from "@/lib/utils";

const NAVIGATION_LINKS = [
  { href: "/experiments", label: "Experiments" },
  { href: "/compare", label: "Compare" },
  { href: "/connections", label: "Connections" },
];

/** Primary links; `currentPathname` is null while the pathname has not streamed in yet. */
export function PrimaryNavigationLinks({ currentPathname }: { currentPathname: string | null }) {
  return (
    <nav aria-label="Primary" className="flex items-center gap-1 text-sm">
      {NAVIGATION_LINKS.map((navigationLink) => {
        const isActive =
          currentPathname !== null && (currentPathname.startsWith(navigationLink.href) || (navigationLink.href === "/experiments" && currentPathname.startsWith("/runs")));
        return (
          <Link
            key={navigationLink.href}
            href={navigationLink.href}
            aria-current={isActive ? "page" : undefined}
            className={cn("rounded-md px-2 py-1 hover:bg-accent focus-visible:ring-2 focus-visible:ring-ring focus-visible:outline-none", isActive && "bg-accent font-medium")}
          >
            {navigationLink.label}
          </Link>
        );
      })}
    </nav>
  );
}

/** Highlights the active section. Render inside <Suspense> because the pathname is runtime data. */
export function AppNavigation() {
  return <PrimaryNavigationLinks currentPathname={usePathname()} />;
}
