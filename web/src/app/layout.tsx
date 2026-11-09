import type { Metadata } from "next";
import { Suspense, type ReactNode } from "react";
import Link from "next/link";
import { Geist_Mono, Inter } from "next/font/google";
import { AppNavigation, UtilityNavigation, NewExperimentNavigationLink, PrimaryNavigationLinks, WorkspaceBreadcrumb, WorkspaceContext } from "@/components/research/app-navigation";
import { ThemeMenu } from "@/components/research/theme-menu";
import { ThemeProvider } from "@/components/theme-provider";
import { Separator } from "@/components/ui/separator";
import { Sidebar, SidebarContent, SidebarFooter, SidebarGroup, SidebarHeader, SidebarInset, SidebarProvider, SidebarRail, SidebarTrigger } from "@/components/ui/sidebar";
import { Toaster } from "@/components/ui/sonner";
import { TooltipProvider } from "@/components/ui/tooltip";
import { cn } from "@/lib/utils";
import "./globals.css";

const interFont = Inter({ subsets: ["latin"], variable: "--font-inter" });
const geistMonoFont = Geist_Mono({ subsets: ["latin"], variable: "--font-geist-mono" });

export const metadata: Metadata = {
  title: "exsim research",
  description: "Configure, run, compare and inspect deterministic exsim research runs.",
};

function OrderBookMark({ className }: { className?: string }) {
  return (
    <svg viewBox="0 0 36 36" fill="none" aria-hidden="true" className={className}>
      <path d="M17 5v26" stroke="currentColor" strokeOpacity=".25" />
      <path d="M5 9h8M2 17h11M7 25h6" stroke="currentColor" strokeWidth="4" />
      <path d="M21 9h6M21 17h10M21 25h13" className="stroke-primary" strokeWidth="4" />
    </svg>
  );
}

export default function RootLayout({ children }: { children: ReactNode }) {
  return (
    <html lang="en" suppressHydrationWarning className={cn("h-full antialiased", interFont.variable, geistMonoFont.variable)}>
      <body>
        <ThemeProvider>
          <TooltipProvider>
            <a
              href="#research-content"
              className="fixed top-2 left-2 z-50 -translate-y-16 bg-background px-3 py-2 text-xs font-medium ring-1 ring-ring focus:translate-y-0"
            >
              Skip to research content
            </a>
            <SidebarProvider>
              <Sidebar collapsible="icon" role="complementary" aria-label="Research workspace">
                <SidebarHeader className="h-12 justify-center border-b border-sidebar-border">
                  <Link href="/research" aria-label="exsim research" className="flex items-center gap-2 px-1.5 text-base font-semibold tracking-tight">
                    <OrderBookMark className="size-5 shrink-0" />
                    <span className="group-data-[collapsible=icon]:hidden">exsim</span>
                  </Link>
                </SidebarHeader>
                <SidebarContent>
                  <SidebarGroup>
                    <Suspense fallback={<PrimaryNavigationLinks currentPathname={null} />}>
                      <AppNavigation />
                    </Suspense>
                  </SidebarGroup>
                  <SidebarGroup>
                    <Suspense fallback={null}>
                      <NewExperimentNavigationLink />
                    </Suspense>
                  </SidebarGroup>
                </SidebarContent>
                <SidebarFooter className="border-t border-sidebar-border">
                  <UtilityNavigation />
                  <p className="px-2 py-1 text-xs/relaxed text-muted-foreground group-data-[collapsible=icon]:hidden">
                    Research runs use built-in models and simulated time.
                  </p>
                </SidebarFooter>
                <SidebarRail />
              </Sidebar>
              <SidebarInset className="min-w-0">
                <header className="sticky top-0 z-20 flex h-12 shrink-0 items-center gap-2 border-b bg-background/95 px-4 backdrop-blur supports-backdrop-filter:bg-background/80">
                  <SidebarTrigger className="-ml-1" />
                  <Separator orientation="vertical" className="mr-1 h-4 data-vertical:self-center" />
                  <Suspense fallback={<WorkspaceBreadcrumb currentPathname={null} />}>
                    <WorkspaceContext />
                  </Suspense>
                  <div className="ml-auto">
                    <ThemeMenu />
                  </div>
                </header>
                <div tabIndex={-1} id="research-content" className="mx-auto flex w-full max-w-screen-2xl min-w-0 flex-1 flex-col gap-8 px-4 py-6 outline-none sm:px-6">
                  {children}
                </div>
              </SidebarInset>
            </SidebarProvider>
            <Toaster />
          </TooltipProvider>
        </ThemeProvider>
      </body>
    </html>
  );
}
