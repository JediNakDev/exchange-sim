import type { Metadata } from "next";
import { Suspense, type ReactNode } from "react";
import Link from "next/link";
import { AppNavigation, PrimaryNavigationLinks } from "@/components/research/app-navigation";
import { Toaster } from "@/components/ui/sonner";
import { TooltipProvider } from "@/components/ui/tooltip";
import "./globals.css";

export const metadata: Metadata = {
  title: "exsim research",
  description: "Configure, run, compare and inspect deterministic exsim research runs.",
};

export default function RootLayout({ children }: { children: ReactNode }) {
  return (
    <html lang="en" className="h-full antialiased">
      <body className="flex min-h-full flex-col">
        <TooltipProvider>
          <header className="sticky top-0 z-20 border-b bg-background/95 backdrop-blur">
            <div className="flex h-11 items-center gap-4 px-4">
              <Link href="/experiments" className="font-mono text-sm font-semibold">
                exsim research
              </Link>
              <Suspense fallback={<PrimaryNavigationLinks currentPathname={null} />}>
                <AppNavigation />
              </Suspense>
            </div>
          </header>
          <main className="flex w-full flex-1 flex-col gap-4 px-4 py-4">{children}</main>
          <Toaster />
        </TooltipProvider>
      </body>
    </html>
  );
}
