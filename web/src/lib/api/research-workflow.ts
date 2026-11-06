import { unwrapApiResult, type ApiSchemas, type ExsimApiClient } from "./exsim-api-client";

/** Follow opaque cursors with unchanged filters. Never publish a partial list as a complete one. */
export async function readAllPages<Item>(
  readPage: (cursor?: string) => Promise<{ items: Item[]; page: ApiSchemas["PageInfo"] }>,
  identity: (item: Item) => string,
): Promise<{ items: Item[] }> {
  const items = new Map<string, Item>();
  const seenCursors = new Set<string>();
  let cursor: string | undefined;
  do {
    const page = await readPage(cursor);
    for (const item of page.items) items.set(identity(item), item);
    cursor = page.page.nextCursor ?? undefined;
    if (cursor !== undefined) {
      if (seenCursors.has(cursor)) throw new Error("The service repeated a page cursor. Refresh to load the complete list.");
      seenCursors.add(cursor);
    }
  } while (cursor !== undefined);
  return { items: [...items.values()] };
}

export function readExperiments(client: ExsimApiClient) {
  return readAllPages(
    async (cursor) => unwrapApiResult(await client.GET("/v1/experiments", { params: { query: { limit: 200, cursor } } })),
    (experiment) => experiment.experimentId,
  );
}

export function readExperimentRuns(client: ExsimApiClient, experimentId: string) {
  return readAllPages(
    async (cursor) => unwrapApiResult(await client.GET("/v1/runs", { params: { query: { experimentId, limit: 200, cursor } } })),
    (run) => run.runId,
  );
}

/** Stable across retries, refreshes and tabs. Do not truncate opaque IDs or exact seeds. */
export async function initialRunIdempotencyKey(experimentId: string, masterSeed: string): Promise<string> {
  const bytes = new TextEncoder().encode(JSON.stringify([experimentId, masterSeed]));
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return `initial-run-${Array.from(new Uint8Array(digest), (byte) => byte.toString(16).padStart(2, "0")).join("")}`;
}

/** Start only seeds with no recorded run. Failed/cancelled runs remain visible, never silently replaced. */
export async function startMissingSeedRuns(client: ExsimApiClient, experiment: ApiSchemas["Experiment"], startPaused: boolean): Promise<number> {
  const recorded = await readExperimentRuns(client, experiment.experimentId);
  const recordedSeeds = new Set(recorded.items.map((run) => run.masterSeed));
  let startedCount = 0;
  for (const masterSeed of experiment.config.seedSet.masterSeeds) {
    if (recordedSeeds.has(masterSeed)) continue;
    unwrapApiResult(await client.POST("/v1/runs", {
      body: { experimentId: experiment.experimentId, masterSeed, startPaused },
      params: { header: { "Idempotency-Key": await initialRunIdempotencyKey(experiment.experimentId, masterSeed) } },
    }));
    startedCount += 1;
  }
  return startedCount;
}

/** Step is a bounded scheduler request; exact event positions remain strings. */
export function validStepEventCount(value: string): boolean {
  return /^[1-9][0-9]{0,5}$/.test(value) && BigInt(value) <= 100000n;
}


/** Search ordered fill pages without retaining unrelated evidence or reading past a match. */
export async function readSelectedFill(client: ExsimApiClient, runId: string, orderId: string, eventPosition: string, signal: AbortSignal): Promise<ApiSchemas["Fill"]> {
  let cursor: string | undefined;
  const seen = new Set<string>();
  do {
    signal.throwIfAborted();
    const page = unwrapApiResult(await client.GET("/v1/runs/{runId}/fills", { params: { path: { runId }, query: { orderId, limit: 200, cursor } }, signal }));
    const selected = page.items.find((fill) => fill.eventPosition === eventPosition && fill.orderId === orderId);
    if (selected) return selected;
    if (page.items.some((fill) => BigInt(fill.eventPosition) > BigInt(eventPosition))) break;
    cursor = page.page.nextCursor ?? undefined;
    if (cursor !== undefined) {
      if (seen.has(cursor)) throw new Error("The service repeated a fill cursor. Try refreshing this order.");
      seen.add(cursor);
    }
  } while (cursor !== undefined);
  throw new Error("The selected fill is missing from the available order evidence. Retry to refresh it.");
}
