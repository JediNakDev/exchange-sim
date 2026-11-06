"use client";

import { useState } from "react";

/** Opaque API cursors have no previous-page operation; keep the visited path. */
export function useCursorHistory() {
  const [cursors, setCursors] = useState<(string | undefined)[]>([undefined]);
  return {
    cursor: cursors.at(-1),
    pageNumber: cursors.length,
    next: (cursor: string) => setCursors((previous) => [...previous, cursor]),
    previous: () => setCursors((previous) => previous.length > 1 ? previous.slice(0, -1) : previous),
  };
}
