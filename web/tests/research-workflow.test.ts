import { describe, expect, test } from "bun:test";
import { MockResearchService } from "../mock-server/mock-research-service";
import { BASELINE_CONFIGURATIONS } from "../mock-server/baseline-catalog";
import { createExsimApiClient, unwrapApiResult } from "../src/lib/api/exsim-api-client";
import { initialRunIdempotencyKey, readAllPages, readExperimentRuns, readSelectedFill, startMissingSeedRuns, validStepEventCount } from "../src/lib/api/research-workflow";

const pageInfo = (nextCursor: string | null) => ({ nextCursor, limit: 1 });

describe("complete research lists", () => {
  test("follows opaque cursors, including empty intermediate pages, without duplicate identities", async () => {
    const cursors: (string | undefined)[] = [];
    const result = await readAllPages(async (cursor) => {
      cursors.push(cursor);
      if (cursor === undefined) return { items: [{ id: "1", value: "old" }], page: pageInfo("opaque-next") };
      if (cursor === "opaque-next") return { items: [], page: pageInfo("opaque-final") };
      return { items: [{ id: "1", value: "fresh" }, { id: "9007199254740993", value: "exact" }], page: pageInfo(null) };
    }, (item) => item.id);
    expect(cursors).toEqual([undefined, "opaque-next", "opaque-final"]);
    expect(result.items).toEqual([{ id: "1", value: "fresh" }, { id: "9007199254740993", value: "exact" }]);
  });
  test("fails on a cursor cycle instead of publishing a truncated list", async () => {
    await expect(readAllPages(async () => ({ items: [1], page: pageInfo("same") }), String)).rejects.toThrow("repeated a page cursor");
  });
  test("does not turn a failed later page into a complete list", async () => {
    await expect(readAllPages(async (cursor) => {
      if (cursor) throw new Error("offline");
      return { items: [1], page: pageInfo("next") };
    }, String)).rejects.toThrow("offline");
  });
});

describe("recoverable seed launches", () => {
  async function experimentFor(service: MockResearchService) {
    const client = createExsimApiClient("http://mock.exsim.test", (request) => service.handleRequest(request));
    const baseline = BASELINE_CONFIGURATIONS[0];
    const config = structuredClone(baseline.config);
    config.seedSet.masterSeeds = ["9007199254740992", "9007199254740993", "18446744073709551615"];
    return unwrapApiResult(await client.POST("/v1/experiments", {
      body: { name: "Recovery experiment", derivedFromBaselineId: baseline.baselineId, config },
      params: { header: { "Idempotency-Key": "recovery-create-experiment" } },
    }));
  }
  test("stable bounded keys preserve full experiment and seed identity", async () => {
    const first = await initialRunIdempotencyKey("exp_" + "a".repeat(120), "9007199254740992");
    expect(first).toBe(await initialRunIdempotencyKey("exp_" + "a".repeat(120), "9007199254740992"));
    expect(first).not.toBe(await initialRunIdempotencyKey("exp_" + "a".repeat(120), "9007199254740993"));
    expect(first).not.toBe(await initialRunIdempotencyKey("exp_" + "a".repeat(119) + "b", "9007199254740992"));
    expect(first).toMatch(/^[A-Za-z0-9_-]{8,128}$/);
  });
  test("an accepted run with a lost reply is not duplicated by a fresh client retry", async () => {
    const service = new MockResearchService();
    const experiment = await experimentFor(service);
    let loseReply = true;
    const client = createExsimApiClient("http://mock.exsim.test", async (request) => {
      const response = await service.handleRequest(request);
      if (loseReply && request.method === "POST" && new URL(request.url).pathname === "/v1/runs") {
        loseReply = false;
        expect(response.status).toBe(201);
        throw new Error("reply lost after commit");
      }
      return response;
    });
    await expect(startMissingSeedRuns(client, experiment, true)).rejects.toThrow("reply lost");
    const fresh = createExsimApiClient("http://mock.exsim.test", (request) => service.handleRequest(request));
    expect(await startMissingSeedRuns(fresh, experiment, true)).toBe(2);
    expect(await startMissingSeedRuns(fresh, experiment, true)).toBe(0);
    const runs = (await readExperimentRuns(fresh, experiment.experimentId)).items;
    expect(runs).toHaveLength(3);
    expect(new Set(runs.map((run) => run.masterSeed)).size).toBe(3);
  });
  test("two simultaneous launchers use the same seed keys", async () => {
    const service = new MockResearchService();
    const experiment = await experimentFor(service);
    const client = createExsimApiClient("http://mock.exsim.test", (request) => service.handleRequest(request));
    await Promise.all([startMissingSeedRuns(client, experiment, true), startMissingSeedRuns(client, experiment, true)]);
    expect((await readExperimentRuns(client, experiment.experimentId)).items).toHaveLength(3);
  });
});

test("step bounds match the API, including keyboard form submissions", () => {
  for (const value of ["1", "99999", "100000"]) expect(validStepEventCount(value)).toBe(true);
  for (const value of ["0", "01", "-1", "1.1", "1e2", "100001", "9999999", "", " 1", "18446744073709551615"]) expect(validStepEventCount(value)).toBe(false);
});


describe("selected fill reads", () => {
  test("stops at a match without enumerating unrelated later pages", async () => {
    const service = new MockResearchService();
    const base = createExsimApiClient("http://mock.exsim.test", (request) => service.handleRequest(request));
    const fills = unwrapApiResult(await base.GET("/v1/runs/{runId}/fills", { params: { path: { runId: "run_fsw2seed303" }, query: { limit: 1 } } }));
    const target = fills.items[0];
    let reads = 0;
    const client = createExsimApiClient("http://mock.exsim.test", async (request) => {
      reads++;
      if (reads > 1) throw new Error("unrelated later page must not be read");
      const url = new URL(request.url);
      url.searchParams.set("limit", "1");
      return service.handleRequest(new Request(url, request));
    });
    expect((await readSelectedFill(client, "run_fsw2seed303", target.orderId, target.eventPosition, new AbortController().signal)).eventPosition).toBe(target.eventPosition);
    expect(reads).toBe(1);
  });
  test("aborted selection starts no network work", async () => {
    const controller = new AbortController();
    controller.abort();
    let reads = 0;
    const client = createExsimApiClient("http://mock.exsim.test", async () => { reads++; return Response.json({}); });
    await expect(readSelectedFill(client, "run_fsw2seed303", "10", "55", controller.signal)).rejects.toThrow();
    expect(reads).toBe(0);
  });
  test("missing selected evidence is explicit rather than perpetual loading", async () => {
    const client = createExsimApiClient("http://mock.exsim.test", async () => Response.json({ items: [], page: { nextCursor: null, limit: 200 } }));
    await expect(readSelectedFill(client, "run_fsw2seed303", "10", "55", new AbortController().signal)).rejects.toThrow("selected fill is missing");
  });
});
