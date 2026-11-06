import type { Metadata } from "next";
import { Suspense, type ReactNode } from "react";
import Link from "next/link";
import { ArrowUpRightIcon, PanelsTopLeftIcon } from "lucide-react";
import { AppNavigation, PrimaryNavigationLinks, WorkspaceContext } from "@/components/research/app-navigation";
import { Toaster } from "@/components/ui/sonner";
import { TooltipProvider } from "@/components/ui/tooltip";
import "@fontsource/ibm-plex-sans/400.css";
import "@fontsource/ibm-plex-sans/500.css";
import "@fontsource/ibm-plex-sans/600.css";
import "@fontsource/ibm-plex-mono/400.css";
import "@fontsource/ibm-plex-mono/500.css";
import "./globals.css";

export const metadata: Metadata = {
  title: "exsim research",
  description: "Configure, run, compare and inspect deterministic exsim research runs.",
};

export default function RootLayout({ children }: { children: ReactNode }) {
  return (
    <html lang="en" className="h-full antialiased">
      <body>
        <TooltipProvider>
          <a href="#research-content" className="skip-link">Skip to research content</a>
          <div className="research-shell">
            <aside className="research-rail" aria-label="Research workspace">
              <Link href="/experiments" className="research-brand" aria-label="exsim research">
                <svg viewBox="0 0 36 36" fill="none" aria-hidden="true"><path d="M17 5v26" stroke="currentColor" strokeOpacity=".3" /><path d="M5 9h8M2 17h11M7 25h6" stroke="currentColor" strokeWidth="4" /><path d="M21 9h6M21 17h10M21 25h13" stroke="#8bc9c7" strokeWidth="4" /></svg>
                <span>exsim<span className="brand-period">.</span></span>
              </Link>
              <p className="rail-description">Strategy research, explained.</p>
              <Suspense fallback={<PrimaryNavigationLinks currentPathname={null} />}><AppNavigation /></Suspense>
              <div className="rail-model-note">
                <PanelsTopLeftIcon size={18} aria-hidden="true" />
                <div><strong>Interactive synthetic</strong><p>Controlled markets.<br />Reproducible experiments.</p></div>
              </div>
              <Link href="/experiments/new" className="rail-create-link">Build an experiment <ArrowUpRightIcon size={15} aria-hidden="true" /></Link>
              <div className="rail-footer"><span className="model-dot" />Simulated time</div>
            </aside>
            <div className="research-workspace">
              <header className="workspace-bar">
                <Suspense fallback={<span className="workspace-context">Research workspace</span>}><WorkspaceContext /></Suspense>
                <span className="workspace-mode">Research mode <span className="mode-divider" /> Single venue</span>
              </header>
              <main tabIndex={-1} id="research-content" className="research-content">{children}</main>
              <footer className="workspace-footer"><span>exsim research</span><span>Inspect the assumptions behind every result.</span></footer>
            </div>
          </div>
          <Toaster theme="light" />
        </TooltipProvider>
      </body>
    </html>
  );
}
