"use client";

// Loads one API resource on the client and optionally polls it. State is keyed
// by `resourceKey` so a change of key never shows the previous resource's data.
// `pollPolicy` is a fixed interval, null (load once), or a function of the latest
// data, so a run can poll quickly while active and stop once terminal.

import { useCallback, useEffect, useEffectEvent, useState } from "react";
import { describeApiFailure, type ApiSchemas } from "@/lib/api/exsim-api-client";

interface KeyedResourceState<ResourceData> {
  resourceKey: string | null;
  data: ResourceData | null;
  failure: ApiSchemas["ApiError"] | null;
}

export interface ApiResourceState<ResourceData> {
  data: ResourceData | null;
  failure: ApiSchemas["ApiError"] | null;
  isLoading: boolean;
  reload: () => void;
}

export function useApiResource<ResourceData>(
  resourceKey: string | null,
  loadResource: () => Promise<ResourceData>,
  pollPolicy: number | null | ((latestData: ResourceData) => number | null),
): ApiResourceState<ResourceData> {
  const [resourceState, setResourceState] = useState<KeyedResourceState<ResourceData>>({ resourceKey: null, data: null, failure: null });
  const [reloadRequestCount, setReloadRequestCount] = useState(0);
  const loadResourceForEffect = useEffectEvent(loadResource);
  const resolveNextPollDelay = useEffectEvent((latestData: ResourceData | null): number | null => {
    if (typeof pollPolicy !== "function") return pollPolicy;
    return latestData === null ? 5000 : pollPolicy(latestData);
  });
  const fixedPollInterval = typeof pollPolicy === "function" ? "adaptive" : pollPolicy;

  useEffect(() => {
    if (resourceKey === null) return;
    let isCancelled = false;
    let pollTimer: ReturnType<typeof setTimeout> | null = null;
    const loadOnce = async () => {
      let loadedData: ResourceData | null = null;
      try {
        loadedData = await loadResourceForEffect();
        if (!isCancelled) setResourceState({ resourceKey, data: loadedData, failure: null });
      } catch (caught) {
        if (!isCancelled) setResourceState((previous) => ({ resourceKey, data: previous.resourceKey === resourceKey ? previous.data : null, failure: describeApiFailure(caught) }));
      }
      const nextPollDelayMs = isCancelled ? null : resolveNextPollDelay(loadedData);
      if (nextPollDelayMs !== null) pollTimer = setTimeout(loadOnce, nextPollDelayMs);
    };
    void loadOnce();
    return () => {
      isCancelled = true;
      if (pollTimer !== null) clearTimeout(pollTimer);
    };
  }, [resourceKey, fixedPollInterval, reloadRequestCount]);

  const reload = useCallback(() => setReloadRequestCount((count) => count + 1), []);
  const isCurrent = resourceState.resourceKey === resourceKey && resourceKey !== null;
  return {
    data: isCurrent ? resourceState.data : null,
    failure: isCurrent ? resourceState.failure : null,
    isLoading: resourceKey !== null && (!isCurrent || (resourceState.data === null && resourceState.failure === null)),
    reload,
  };
}
