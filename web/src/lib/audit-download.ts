import { unwrapApiResult, type ApiSchemas, type ExsimApiClient } from "@/lib/api/exsim-api-client";

// Larger packages use the streaming local utility. Browser download is explicit
// and bounded; never mistake a truncated transfer for a complete audit.
export const MAX_BROWSER_AUDIT_BYTES = 32n * 1024n * 1024n;
export interface BrowserAuditPackage {
  format: "exsim.browser-audit/1";
  inventory: ApiSchemas["AuditInventory"];
  artifacts: { artifactIndex: string; contentHex: string }[];
}

export async function downloadAudit(client: ExsimApiClient, runId: string, signal: AbortSignal, onProgress: (file: string) => void): Promise<BrowserAuditPackage> {
  const inventory = unwrapApiResult(await client.GET("/v1/runs/{runId}/audit-export", { params: { path: { runId } }, signal }));
  if (!("schemaVersion" in inventory) || inventory.runId !== runId) throw new Error("Audit inventory identity mismatch");
  const present = inventory.items.filter((item) => item.present);
  const total = present.reduce((sum, item) => sum + BigInt(item.sizeBytes ?? "0"), 0n);
  if (total > MAX_BROWSER_AUDIT_BYTES) throw new Error("This audit exceeds the 32 MiB browser limit. Use the streaming local download command below; no files have been downloaded.");
  const artifacts: BrowserAuditPackage["artifacts"] = [];
  for (const item of present) {
    if (item.sizeBytes === null || item.sha256 === null) throw new Error("Audit artifact has no size or integrity hash");
    onProgress(item.name);
    const parts: string[] = [];
    let offset = "0";
    let size = 0n;
    while (true) {
      signal.throwIfAborted();
      const chunk = unwrapApiResult(await client.GET("/v1/runs/{runId}/audit-export", { params: { path: { runId }, query: { artifactIndex: item.artifactIndex, offset } }, signal }));
      if (!("contentHex" in chunk) || chunk.runId !== runId || chunk.artifactIndex !== item.artifactIndex || chunk.offset !== offset || !/^(?:[0-9a-fA-F]{2})*$/.test(chunk.contentHex)) throw new Error("Audit chunk identity or encoding mismatch");
      size += BigInt(chunk.contentHex.length / 2);
      if (size > BigInt(item.sizeBytes) || size > MAX_BROWSER_AUDIT_BYTES) throw new Error("Audit transfer exceeded its inventory size");
      parts.push(chunk.contentHex);
      if (chunk.nextOffset === null) break;
      if (BigInt(chunk.nextOffset) !== size || BigInt(chunk.nextOffset) <= BigInt(offset)) throw new Error("Audit continuation did not advance exactly");
      offset = chunk.nextOffset;
    }
    if (size !== BigInt(item.sizeBytes)) throw new Error("Audit transfer is incomplete");
    const contentHex = parts.join("");
    const bytes = Uint8Array.from(contentHex.match(/../g) ?? [], (pair) => parseInt(pair, 16));
    const hash = Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)), (byte) => byte.toString(16).padStart(2, "0")).join("");
    if (hash !== item.sha256) throw new Error("Audit integrity mismatch; no package will be saved");
    artifacts.push({ artifactIndex: item.artifactIndex, contentHex });
  }
  signal.throwIfAborted();
  return { format: "exsim.browser-audit/1", inventory, artifacts };
}
