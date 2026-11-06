import { expect, test, type Page, type Route } from "@playwright/test";
import { MockResearchService } from "../mock-server/mock-research-service";
import { BASELINE_CONFIGURATIONS } from "../mock-server/baseline-catalog";
import type { ApiSchemas } from "../src/lib/api/exsim-api-client";

type Intercept = (route: Route, request: Request, forward: () => Promise<Response>) => Promise<boolean>;
const failure = { error: { code: "service_unavailable", message: "Injected lost response", requestId: "browser-regression", retryable: true, fieldIssues: [] } };
async function installService(page: Page, service: MockResearchService, intercept?: Intercept) {
  await page.route("**/api/exsim/v1/**", async (route) => {
    const incoming = route.request();
    const url = new URL(incoming.url());
    const request = new Request(`http://fixture.exsim${url.pathname.replace("/api/exsim", "")}${url.search}`, {
      method: incoming.method(), headers: incoming.headers(), body: incoming.postData(),
    });
    const forward = () => service.handleRequest(request);
    if (intercept && await intercept(route, request, forward)) return;
    const response = await forward();
    await route.fulfill({ status: response.status, headers: Object.fromEntries(response.headers), body: await response.text() });
  });
}

async function requestJson<T>(service: MockResearchService, path: string, body?: unknown, key = "browser-setup-key"): Promise<T> {
  const response = await service.handleRequest(new Request(`http://fixture.exsim${path}`, body === undefined ? {} : {
    method: "POST", body: JSON.stringify(body), headers: { "content-type": "application/json", "idempotency-key": key },
  }));
  expect(response.ok).toBe(true);
  return response.json();
}

test("a partially launched batch remains saved and starts only its missing seed", async ({ page }) => {
  const service = new MockResearchService();
  let posts = 0;
  await installService(page, service, async (route, request, forward) => {
    if (request.method !== "POST" || new URL(request.url).pathname !== "/v1/runs") return false;
    posts++;
    if (posts !== 2) return false;
    expect((await forward()).status).toBe(201); // Mutation happened, reply was lost.
    await route.fulfill({ status: 503, json: failure });
    return true;
  });
  await page.goto("/experiments/new");
  await page.getByLabel("Name", { exact: true }).fill("Partial batch regression");
  await page.getByLabel(/Master seeds/).fill("101, 202, 303");
  await page.getByText("Start runs paused, to step through them event by event").click();
  await page.getByRole("button", { name: "Review assumptions" }).click();
  await page.getByRole("button", { name: "Create and start 3 runs" }).click();
  await expect(page.getByRole("heading", { name: "Partial batch regression" })).toBeVisible();
  await expect(page.getByText("1 of 3 seeds still need runs.", { exact: false })).toBeVisible();
  await page.getByText("Start paused", { exact: true }).click();
  await page.getByRole("button", { name: "Start missing seeds" }).click();
  const experimentId = new URL(page.url()).pathname.split("/").at(-1)!;
  await expect.poll(async () => (await requestJson<ApiSchemas["RunPage"]>(service, `/v1/runs?experimentId=${experimentId}`)).items.map((run) => run.masterSeed).sort()).toEqual(["101", "202", "303"]);
  await expect(page.getByRole("button", { name: "Start missing seeds" })).toHaveCount(0);
  expect(posts).toBe(3);
});

test("variants copy the saved assumptions and expose inventory-aware configuration", async ({ page }) => {
  const service = new MockResearchService();
  const parent = await requestJson<ApiSchemas["Experiment"]>(service, "/v1/experiments/exp_fixedspreadw2");
  let previewBody: ApiSchemas["ExperimentCreateRequest"] | undefined;
  await installService(page, service, async (_route, request) => {
    if (request.method === "POST" && new URL(request.url).pathname === "/v1/experiment-previews") previewBody = JSON.parse(await request.clone().text());
    return false;
  });
  await page.goto(`/experiments/${parent.experimentId}`);
  await page.getByRole("link", { name: "Create variant" }).click();
  await expect(page.getByLabel("Name", { exact: true })).toHaveValue(`${parent.name} variant`);
  await page.getByRole("button", { name: "Review assumptions" }).click();
  await expect.poll(() => previewBody?.parentExperimentId).toBe(parent.experimentId);
  expect(previewBody?.config).toEqual(parent.config);
  await page.getByLabel("Quoting model").click();
  await page.getByRole("option", { name: "Inventory-aware market maker" }).click();
  await page.getByLabel(/Inventory skew/).fill("2");
  await page.getByRole("button", { name: "Review assumptions" }).click();
  await expect.poll(() => previewBody?.config.strategy.strategyKind).toBe("inventory_aware_market_maker");
  expect(previewBody?.config.strategy).toMatchObject({ inventorySkewTicksPerUnit: "2", inventorySkewUnitShares: "100" });
});

test("lost step responses retry the same key and keyboard submits respect the API bound", async ({ page }) => {
  let now = Date.parse("2026-11-07T12:00:00Z");
  const service = new MockResearchService(() => now);
  const baseline = BASELINE_CONFIGURATIONS[0];
  const experiment = await requestJson<ApiSchemas["Experiment"]>(service, "/v1/experiments", { name: "Step retry", derivedFromBaselineId: baseline.baselineId, config: baseline.config }, "step-experiment");
  const run = await requestJson<ApiSchemas["Run"]>(service, "/v1/runs", { experimentId: experiment.experimentId, masterSeed: baseline.config.seedSet.masterSeeds[0], startPaused: true }, "step-run");
  now += 1000;
  const keys: string[] = [];
  await installService(page, service, async (route, request, forward) => {
    if (!new URL(request.url).pathname.endsWith("/step")) return false;
    keys.push(request.headers.get("idempotency-key")!);
    if (keys.length !== 1) return false;
    expect((await forward()).ok).toBe(true);
    await route.fulfill({ status: 503, json: failure });
    return true;
  });
  await page.goto(`/runs/${run.runId}`);
  const input = page.getByLabel("Events to step");
  await input.fill("100001");
  await input.evaluate((element) => element.closest("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })));
  expect(keys).toHaveLength(0);
  await input.fill("1");
  await page.getByRole("button", { name: "Step events" }).click();
  await expect(page.getByText("Control request rejected")).toBeVisible();
  const afterFirst = await requestJson<ApiSchemas["Run"]>(service, `/v1/runs/${run.runId}`);
  await page.getByRole("button", { name: "Step events" }).click();
  await expect.poll(() => keys.length).toBe(2);
  expect(keys[0]).toBe(keys[1]);
  const afterRetry = await requestJson<ApiSchemas["Run"]>(service, `/v1/runs/${run.runId}`);
  expect(afterRetry.progress.processedCanonicalEventCount).toBe(afterFirst.progress.processedCanonicalEventCount);
});

test("all fill/order pages are reachable without losing the selected explanation", async ({ page }) => {
  const service = new MockResearchService();
  await installService(page, service, async (route, request) => {
    const url = new URL(request.url);
    if (!/\/(fills|orders)$/.test(url.pathname)) return false;
    expect(url.searchParams.get("limit")).toBe("200");
    url.searchParams.set("limit", "2"); // Exercise the cursor boundary with a small fixture page.
    const response = await service.handleRequest(new Request(url, request));
    await route.fulfill({ status: response.status, headers: Object.fromEntries(response.headers), body: await response.text() });
    return true;
  });
  await page.goto("/runs/run_fsw2seed303/inspect");
  const fills = page.getByRole("table", { name: "Recorded fills" });
  await expect(fills.locator("tbody tr")).toHaveCount(2);
  await fills.locator("tbody tr").first().click();
  const explanation = page.getByRole("heading", { name: /^Order [0-9]+:/ });
  await expect(explanation).toBeVisible();
  const title = await explanation.innerText();
  const first = await fills.locator("tbody tr").first().innerText();
  await page.getByRole("button", { name: "Next fills" }).click();
  await expect(page.getByText(/Fills, page 2:/)).toBeVisible();
  await expect(explanation).toHaveText(title);
  expect(await fills.locator("tbody tr").first().innerText()).not.toBe(first);
  await page.getByRole("button", { name: "Previous fills" }).click();
  await expect(fills.locator("tbody tr").first()).toHaveText(first, { useInnerText: true });
  await page.getByRole("tab", { name: "Own orders" }).click();
  await expect(page.getByRole("table", { name: "Recorded own orders" }).locator("tbody tr")).toHaveCount(2);
  await page.getByRole("button", { name: "Next orders" }).click();
  await expect(page.getByText(/Orders, page 2:/)).toBeVisible();
});

test("offline activity counts are unavailable and recover through Try again", async ({ page }) => {
  const service = new MockResearchService();
  let fail = true;
  await installService(page, service, async (route, request) => {
    if (!fail || new URL(request.url).pathname !== "/v1/experiments") return false;
    await route.fulfill({ status: 503, json: failure });
    return true;
  });
  await page.goto("/experiments");
  await expect(page.getByLabel("Research activity").locator("dd")).toHaveText(["n/a", "n/a", "n/a"]);
  fail = false;
  await page.getByRole("button", { name: "Try again" }).click();
  await expect(page.getByLabel("Research activity").locator("dd").first()).toHaveText("2");
});

test("research routes retain mobile width and have no browser exceptions", async ({ page }) => {
  const errors: string[] = [];
  page.on("pageerror", (error) => errors.push(error.message));
  await page.setViewportSize({ width: 390, height: 844 });
  await installService(page, new MockResearchService());
  for (const path of ["/experiments", "/experiments/new", "/compare?baseline=exp_fixedspreadw2&candidate=exp_fixedspreadw4", "/runs/run_fsw2seed303/inspect"]) {
    await page.goto(path);
    await expect(page.getByRole("heading", { level: 1 })).toBeVisible();
    expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBe(390);
  }
  expect(errors).toEqual([]);
});
