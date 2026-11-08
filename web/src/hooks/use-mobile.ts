import { useSyncExternalStore } from "react";

const MOBILE_BREAKPOINT_PX = 768;
const MOBILE_MEDIA_QUERY = `(max-width: ${MOBILE_BREAKPOINT_PX - 1}px)`;

function subscribeToMobileMediaQuery(onViewportChange: () => void) {
  const mobileMediaQueryList = window.matchMedia(MOBILE_MEDIA_QUERY);
  mobileMediaQueryList.addEventListener("change", onViewportChange);
  return () => mobileMediaQueryList.removeEventListener("change", onViewportChange);
}

export function useIsMobile(): boolean {
  return useSyncExternalStore(
    subscribeToMobileMediaQuery,
    () => window.matchMedia(MOBILE_MEDIA_QUERY).matches,
    () => false,
  );
}
