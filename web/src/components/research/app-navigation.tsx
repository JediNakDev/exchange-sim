"use client";

import Link from "next/link";
import { usePathname } from "next/navigation";
import { FlaskConicalIcon, GitCompareArrowsIcon, NetworkIcon, PlusIcon } from "lucide-react";
import { SidebarMenu, SidebarMenuButton, SidebarMenuItem } from "@/components/ui/sidebar";
import { Breadcrumb, BreadcrumbItem, BreadcrumbList, BreadcrumbPage, BreadcrumbSeparator } from "@/components/ui/breadcrumb";

const NAVIGATION_LINKS = [
  { href: "/experiments", label: "Experiments", icon: FlaskConicalIcon },
  { href: "/compare", label: "Compare", icon: GitCompareArrowsIcon },
  { href: "/connections", label: "Connections", icon: NetworkIcon },
];

function isNavigationLinkActive(href: string, currentPathname: string | null): boolean {
  if (currentPathname === null || currentPathname === "/experiments/new") return false;
  return currentPathname.startsWith(href) || (href === "/experiments" && currentPathname.startsWith("/runs"));
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
        <SidebarMenuButton asChild isActive={isActive} tooltip="New experiment" variant="outline">
          <Link href="/experiments/new" aria-current={isActive ? "page" : undefined}>
            <PlusIcon aria-hidden="true" />
            <span>New experiment</span>
          </Link>
        </SidebarMenuButton>
      </SidebarMenuItem>
    </SidebarMenu>
  );
}

function workspaceSectionLabel(pathname: string): string {
  if (pathname.includes("/inspect")) return "Fill inspection";
  if (pathname.startsWith("/runs")) return "Run analysis";
  if (pathname === "/experiments/new") return "New experiment";
  if (pathname === "/compare") return "Paired comparison";
  if (pathname === "/connections") return "Connections";
  if (pathname === "/experiments") return "Experiment library";
  return "Experiment details";
}

export function WorkspaceBreadcrumb({ currentPathname }: { currentPathname: string | null }) {
  return (
    <Breadcrumb>
      <BreadcrumbList>
        <BreadcrumbItem className="hidden sm:inline-flex">Research</BreadcrumbItem>
        <BreadcrumbSeparator className="hidden sm:inline-flex" />
        <BreadcrumbItem>
          <BreadcrumbPage>{currentPathname === null ? "Research workspace" : workspaceSectionLabel(currentPathname)}</BreadcrumbPage>
        </BreadcrumbItem>
      </BreadcrumbList>
    </Breadcrumb>
  );
}

export function WorkspaceContext() {
  return <WorkspaceBreadcrumb currentPathname={usePathname()} />;
}
