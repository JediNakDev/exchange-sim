import { expect, test } from "bun:test";
import { createExsimApiClient } from "../../src/lib/api/exsim-api-client";
import { downloadAudit, MAX_BROWSER_AUDIT_BYTES } from "../../src/lib/audit-download";

const hash = async (text: string) => Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text))), (byte) => byte.toString(16).padStart(2, "0")).join("");
async function service(options: { wrongHash?: boolean; wrongOffset?: boolean; tooLarge?: boolean } = {}) {
  const text = 'event 9007199254740993\n';
  const inventory = { runId: "run_testaudit", schemaVersion: "exsim.audit-export/1", state: "completed", reproductionSupported: true, verificationCommand: "verify", reproductionCommand: "reproduce", items: [{ artifactIndex: "0", name: "runner/events.jsonl", present: true, sizeBytes: options.tooLarge ? (MAX_BROWSER_AUDIT_BYTES + 1n).toString() : String(text.length), sha256: options.wrongHash ? "0".repeat(64) : await hash(text) }] };
  let requests = 0;
  const client = createExsimApiClient("http://audit.test", async (request) => {
    requests++;
    const query = new URL(request.url).searchParams;
    const body = query.has("artifactIndex") ? { runId: inventory.runId, artifactIndex: "0", offset: options.wrongOffset ? "1" : "0", contentHex: Array.from(new TextEncoder().encode(text), (byte) => byte.toString(16).padStart(2, "0")).join(""), nextOffset: null } : inventory;
    return Response.json(body);
  });
  return { client, requests: () => requests, text };
}

test("audit download preserves all bytes and exact identifiers after SHA-256 verification", async () => {
  const fixture = await service();
  const result = await downloadAudit(fixture.client, "run_testaudit", new AbortController().signal, () => {});
  expect(result.artifacts).toHaveLength(1);
  expect(new TextDecoder().decode(Uint8Array.from(result.artifacts[0].contentHex.match(/../g)!, (pair) => parseInt(pair, 16)))).toBe(fixture.text);
  expect(fixture.requests()).toBe(2);
});

test("audit corruption and mismatched offsets cannot produce a complete package", async () => {
  for (const option of [{ wrongHash: true }, { wrongOffset: true }]) {
    const fixture = await service(option);
    await expect(downloadAudit(fixture.client, "run_testaudit", new AbortController().signal, () => {})).rejects.toThrow();
  }
});

test("oversized browser audits are refused before reading any artifact chunks", async () => {
  const fixture = await service({ tooLarge: true });
  await expect(downloadAudit(fixture.client, "run_testaudit", new AbortController().signal, () => {})).rejects.toThrow("32 MiB");
  expect(fixture.requests()).toBe(1);
});
