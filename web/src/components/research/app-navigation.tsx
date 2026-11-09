"use client";

import Link from "next/link";
import { Fragment } from "react";
import { usePathname, useSearchParams } from "next/navigation";
import { FlaskConicalIcon, GitCompareArrowsIcon, NetworkIcon, PlusIcon, LayersIcon, GlobeIcon, ListChecksIcon } from "lucide-react";
import { SidebarMenu, SidebarMenuButton, SidebarMenuItem } from "@/components/ui/sidebar";
import { Breadcrumb, BreadcrumbItem, BreadcrumbLink, BreadcrumbList, BreadcrumbPage, BreadcrumbSeparator } from "@/components/ui/breadcrumb";

const NAVIGATION_LINKS = [
  { href: "/research", label: "Research", icon: FlaskConicalIcon },
  { href: "/strategies", label: "Strategies", icon: LayersIcon },
  { href: "/scenarios", label: "Scenarios", icon: GlobeIcon },
  { href: "/runs", label: "Runs", icon: ListChecksIcon },
  { href: "/compare", label: "Compare", icon: GitCompareArrowsIcon },
];

function isNavigationLinkActive(href: string, currentPathname: string | null): boolean {
  if (currentPathname === null || currentPathname === "/experiments/new") return false;
  return currentPathname.startsWith(href) || (href === "/research" && currentPathname.startsWith("/experiments"));
}

export function PrimaryNavigationLinks({ currentPathname }: { currentPathname: string | null }) {
  return (
    <nav aria-label="Primary">
      <SidebarMenu>
        {NAVIGATION_LINKS.map(({ href, label, icon: NavigationIcon }) => {
          const isActive = isNavigationLinkActive(href, currentPathname);
          return (
            <SidebarMenuItem key={href}>
              <SidebarMenuButton asChild isActive={isActive} tooltip={label} className="data-[active=true]:shadow-[inset_2px_0_0_var(--color-primary)]">
                <Link href={href} aria-current={isActive ? "page" : undefined}>
                  <NavigationIcon aria-hidden="true" />
                  <span>{label}</span>
                </Link>
              </SidebarMenuButton>
            </SidebarMenuItem>
          );
        })}
      </SidebarMenu>
    </nav>
  );
}

export function AppNavigation() {
  return <PrimaryNavigationLinks currentPathname={usePathname()} />;
}

export function NewExperimentNavigationLink() {
  const isActive = usePathname() === "/experiments/new";
  return (
    <SidebarMenu>
      <SidebarMenuItem>
        <SidebarMenuButton asChild isActive={isActive} tooltip="New study" variant="outline">
          <Link href="/experiments/new" aria-current={isActive ? "page" : undefined}>
            <PlusIcon aria-hidden="true" />
            <span>New study</span>
          </Link>
        </SidebarMenuButton>
      </SidebarMenuItem>
    </SidebarMenu>
  );
}

function workspaceSectionLabel(pathname: string): string {
  if (pathname.includes("/inspect")) return "Fill inspection";
  if (pathname === "/runs") return "Run queue and results";
  if (pathname.startsWith("/runs")) return "Run analysis";
  if (pathname === "/research") return "Studies";
  if (pathname === "/strategies") return "Strategy library";
  if (pathname === "/scenarios") return "Market scenarios";
  if (pathname === "/experiments/new") return "New study";
  if (pathname === "/compare") return "Paired comparison";
  if (pathname === "/connections") return "Connections";
  if (pathname === "/experiments") return "Experiment library";
  return "Study workspace";
}

export function WorkspaceBreadcrumb({ currentPathname, sourceId, contextQuery = "" }: { currentPathname: string | null; sourceId?: string | null; contextQuery?: string }) {
  const path = currentPathname ?? "/research";
  const runId = /^\/runs\/([^/]+)/.exec(path)?.[1];
  const crumbs: { label: string; href?: string }[] = [{ label: "Research", href: "/research" }];
  if (runId) {
    crumbs.push({ label: "Runs", href: "/runs" });
    if (path.endsWith("/inspect")) crumbs.push({ label: "Run analysis", href: `/runs/${encodeURIComponent(runId)}${contextQuery}` });
  } else if (path === "/experiments/new" && sourceId) {
    crumbs.push({ label: "Source study", href: `/experiments/${encodeURIComponent(sourceId)}` });
  }
  if (path === "/research" || path === "/") crumbs[0] = { label: "Research" };
  else crumbs.push({ label: path === "/experiments/new" && sourceId ? "New variant" : workspaceSectionLabel(path) });
  return <Breadcrumb className="min-w-0 overflow-x-auto"><BreadcrumbList className="flex-nowrap whitespace-nowrap">{crumbs.map((crumb, index) => <Fragment key={`${index}:${crumb.label}`}>
    {index ? <BreadcrumbSeparator /> : null}
    <BreadcrumbItem className="shrink-0">{crumb.href ? <BreadcrumbLink asChild><Link href={crumb.href}>{crumb.label}</Link></BreadcrumbLink> : <BreadcrumbPage>{crumb.label}</BreadcrumbPage>}</BreadcrumbItem>
  </Fragment>)}</BreadcrumbList></Breadcrumb>;
}

export function WorkspaceContext() {
  const params = useSearchParams();
  const context = new URLSearchParams();
  for (const key of ["baseline", "candidate", "runSelection"]) { const value = params.get(key); if (value) context.set(key, value); }
  return <WorkspaceBreadcrumb currentPathname={usePathname()} sourceId={params.get("from")} contextQuery={context.size ? `?${context}` : ""} />;
}

export function UtilityNavigation() {
  return <SidebarMenu><SidebarMenuItem><SidebarMenuButton asChild tooltip="Connections"><Link href="/connections"><NetworkIcon aria-hidden="true" /><span>Connections</span></Link></SidebarMenuButton></SidebarMenuItem></SidebarMenu>;
}
