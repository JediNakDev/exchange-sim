"use client";

import Link from "next/link";
import { usePathname } from "next/navigation";
import { FlaskConicalIcon, GitCompareArrowsIcon, NetworkIcon, ChevronRightIcon } from "lucide-react";
import { cn } from "@/lib/utils";

const NAVIGATION_LINKS = [
  { href: "/experiments", label: "Experiments", icon: FlaskConicalIcon },
  { href: "/compare", label: "Compare", icon: GitCompareArrowsIcon },
  { href: "/connections", label: "Connections", icon: NetworkIcon },
];

export function PrimaryNavigationLinks({ currentPathname }: { currentPathname: string | null }) {
  return (
    <nav aria-label="Primary" className="workspace-navigation">
      {NAVIGATION_LINKS.map(({ href, label, icon: Icon }) => {
        const isActive = currentPathname !== null && (currentPathname.startsWith(href) || (href === "/experiments" && currentPathname.startsWith("/runs")));
        return (
          <Link key={href} href={href} aria-current={isActive ? "page" : undefined} className={cn("workspace-nav-link", isActive && "is-active")}>
            <Icon size={18} strokeWidth={1.7} aria-hidden="true" />
            <span>{label}</span>
            {isActive ? <ChevronRightIcon className="nav-current-marker" size={15} aria-hidden="true" /> : null}
          </Link>
        );
      })}
    </nav>
  );
}

export function AppNavigation() {
  return <PrimaryNavigationLinks currentPathname={usePathname()} />;
}

export function WorkspaceContext() {
  const pathname = usePathname();
  const section = pathname.includes("/inspect") ? "Fill inspection"
    : pathname.startsWith("/runs") ? "Run analysis"
    : pathname === "/experiments/new" ? "New experiment"
    : pathname === "/compare" ? "Paired comparison"
    : pathname === "/connections" ? "Connections"
    : pathname === "/experiments" ? "Experiment library" : "Experiment details";
  return <span className="workspace-context"><span>Research</span><ChevronRightIcon size={13} aria-hidden="true" /><span>{section}</span></span>;
}
