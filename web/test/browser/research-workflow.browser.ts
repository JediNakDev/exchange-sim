import { expect, test, type Page, type Route } from "@playwright/test";
import { MockResearchService } from "../mock-server/mock-research-service";
import { BASELINE_CONFIGURATIONS } from "../mock-server/baseline-catalog";
import type { ApiSchemas } from "../../src/lib/api/exsim-api-client";

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
  await page.getByRole("tab", { name: "Runs and recovery", exact: true }).click();
  await expect(page.getByText("1 of 3 seeds still need runs.", { exact: false })).toBeVisible();
  await page.getByText("Start paused", { exact: true }).click();
  await page.getByRole("button", { name: "Start missing seeds" }).click();
  const experimentId = new URL(page.url()).pathname.split("/").at(-1)!;
  await expect.poll(async () => (await requestJson<ApiSchemas["RunPage"]>(service, `/v1/runs?experimentId=${experimentId}`)).items.map((run) => run.masterSeed).sort()).toEqual(["101", "202", "303"]);
  await expect(page.getByRole("button", { name: "Start missing seeds" })).toHaveCount(0);
  expect(posts).toBe(3);
});

test("variants copy saved assumptions and hide models the service cannot execute", async ({ page }) => {
  const service = new MockResearchService();
  const parent = await requestJson<ApiSchemas["Experiment"]>(service, "/v1/experiments/exp_fixedspreadw2");
  let previewBody: ApiSchemas["ExperimentCreateRequest"] | undefined;
  await installService(page, service, async (_route, request) => {
    if (request.method === "POST" && new URL(request.url).pathname === "/v1/experiment-previews") previewBody = JSON.parse(await request.clone().text());
    return false;
  });
  await page.goto(`/experiments/${parent.experimentId}`);
  await page.getByRole("link", { name: "Test another change", exact: true }).click();
  await expect(page.getByLabel("Name", { exact: true })).toHaveValue(`${parent.name} variant`);
  await page.getByRole("button", { name: "Review assumptions" }).click();
  await expect.poll(() => previewBody?.parentExperimentId).toBe(parent.experimentId);
  expect(previewBody?.config).toEqual(parent.config);
  await page.getByLabel("Quoting model").click();
  await expect(page.getByRole("option", { name: "Inventory-aware market maker" })).toHaveCount(0);
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
  await expect(page.getByText("Study list unavailable. Retry the request before interpreting this as an empty library.", { exact: true })).toBeVisible();
  await expect(page.getByRole("table", { name: "Research studies" })).toHaveCount(0);
  fail = false;
  await page.getByRole("button", { name: "Try again" }).click();
  await expect(page.getByRole("table", { name: "Research studies" })).toBeVisible();
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

test("duration drafts survive keyboard clearing and invalid intermediate text", async ({ page }) => {
  const errors: string[] = [];
  page.on("pageerror", (error) => errors.push(error.message));
  await installService(page, new MockResearchService());
  await page.goto("/experiments/new");
  await page.getByText("Latency: feed, order, acknowledgment and cancel paths", { exact: true }).click();
  for (const label of ["Requote interval", "Feed", "Order entry", "Acknowledgment", "Cancel"]) {
    const input = page.getByRole("textbox", { name: new RegExp(`^${label}`) });
    const original = await input.inputValue();
    await input.press("ControlOrMeta+A");
    await input.press("Backspace");
    await expect(input).toHaveValue("");
    await expect(page.getByRole("heading", { name: /^New (study|variant)$/ })).toBeVisible();
    await input.fill("not yet a duration");
    await expect(input).toHaveValue("not yet a duration");
    await input.fill(original);
  }
  await page.getByRole("button", { name: "Review assumptions" }).click();
  await expect(page.getByRole("button", { name: /Create and start/ })).toBeEnabled();
  expect(errors).toEqual([]);
});

test("changing variant source resets reviewed intent and draft state on the same route", async ({ page }) => {
  const service = new MockResearchService();
  const second = await requestJson<ApiSchemas["Experiment"]>(service, "/v1/experiments/exp_fixedspreadw4");
  await installService(page, service);
  await page.goto("/experiments/new?from=exp_fixedspreadw2");
  await page.getByLabel("Name", { exact: true }).fill("Edited source A");
  await page.getByRole("button", { name: "Review assumptions" }).click();
  await expect(page.getByRole("button", { name: /Create and start/ })).toBeEnabled();
  await page.evaluate(() => window.history.pushState(null, "", "/experiments/new?from=exp_fixedspreadw4"));
  await expect(page.getByLabel("Name", { exact: true })).toHaveValue(`${second.name} variant`);
  await expect(page.getByRole("button", { name: /Create and start/ })).toBeDisabled();
  await expect(page.getByLabel(/Half spread/)).toHaveValue("halfSpreadTicks" in second.config.strategy ? second.config.strategy.halfSpreadTicks : "");
});

test("loaded run status becomes visibly stale and controls wait for recovery", async ({ page }) => {
  let now = Date.parse("2026-11-07T12:00:00Z");
  const service = new MockResearchService(() => now);
  const baseline = BASELINE_CONFIGURATIONS[0];
  const experiment = await requestJson<ApiSchemas["Experiment"]>(service, "/v1/experiments", { name: "Stale status", derivedFromBaselineId: baseline.baselineId, config: baseline.config }, "stale-experiment");
  const run = await requestJson<ApiSchemas["Run"]>(service, "/v1/runs", { experimentId: experiment.experimentId, masterSeed: baseline.config.seedSet.masterSeeds[0], startPaused: true }, "stale-run");
  now += 1000;
  let unavailable = false;
  await installService(page, service, async (route, request) => {
    if (!unavailable || new URL(request.url).pathname !== `/v1/runs/${run.runId}`) return false;
    await route.fulfill({ status: 503, json: failure });
    return true;
  });
  await page.goto(`/runs/${run.runId}`);
  await expect(page.getByRole("button", { name: "Step events" })).toBeEnabled();
  unavailable = true;
  await expect(page.getByText("Run status is stale. Refresh before changing the run.")).toBeVisible();
  await expect(page.getByRole("button", { name: "Step events" })).toBeDisabled();
  unavailable = false;
  await page.getByRole("alert").filter({ hasText: "Run status is stale. Refresh before changing the run." }).getByRole("button", { name: "Try again" }).click();
  await expect(page.getByText("Run status is stale. Refresh before changing the run.")).toHaveCount(0);
  await expect(page.getByRole("button", { name: "Step events" })).toBeEnabled();
  await page.goto(`/runs/${run.runId}/inspect`);
  await expect(page.getByRole("heading", { level: 1 })).toBeVisible();
  unavailable = true;
  await expect(page.getByText("Run status is stale. Showing the last known event range.")).toBeVisible();
  unavailable = false;
  await page.getByRole("alert").filter({ hasText: "Run status is stale. Showing the last known event range." }).getByRole("button", { name: "Try again" }).click();
  await expect(page.getByText("Run status is stale. Showing the last known event range.")).toHaveCount(0);
});

test("stationary lifecycle and series failures recover in place", async ({ page }) => {
  const service = new MockResearchService();
  let failLifecycle = true;
  let failSeries = true;
  await installService(page, service, async (route, request) => {
    const path = new URL(request.url).pathname;
    if ((failLifecycle && path.endsWith("/lifecycle")) || (failSeries && path.endsWith("/series"))) {
      await route.fulfill({ status: 503, json: failure });
      return true;
    }
    return false;
  });
  await page.goto("/runs/run_fsw2seed303");
  await expect(page.getByText("Could not load run series")).toBeVisible();
  failSeries = false;
  await page.getByRole("button", { name: "Try again", exact: true }).click();
  await expect(page.getByRole("heading", { name: "Equity change from initial cash (USD)" })).toBeVisible();
  await page.goto("/runs/run_fsw2seed303/inspect");
  const fills = page.getByRole("table", { name: "Recorded fills" });
  await fills.locator("tbody tr").first().click();
  await expect(page.getByText("Could not load order lifecycle")).toBeVisible();
  failLifecycle = false;
  await page.getByRole("button", { name: "Try again", exact: true }).click();
  await expect(page.getByRole("heading", { name: /^Order [0-9]+:/ })).toBeVisible();
});

test("capability discovery does not offer an unknown strategy without an editor", async ({ page }) => {
  const service = new MockResearchService();
  await installService(page, service, async (route, request, forward) => {
    const path = new URL(request.url).pathname;
    if (path !== "/v1/service" && path !== "/v1/baselines") return false;
    const response = await forward();
    const body = await response.json();
    if (path === "/v1/service") body.executableStrategyKinds.unshift("future_strategy");
    else body.items.unshift({ ...structuredClone(body.items[0]), baselineId: "future-baseline", name: "Future strategy", config: { ...body.items[0].config, strategy: { strategyKind: "future_strategy" } } });
    await route.fulfill({ status: 200, json: body });
    return true;
  });
  await page.goto("/experiments/new");
  await expect(page.getByLabel("Name", { exact: true })).toBeVisible();
  await page.getByLabel("Starting preset", { exact: true }).click();
  await expect(page.getByRole("option", { name: "Future strategy" })).toHaveCount(0);
  await expect(page.getByRole("option").first()).toBeVisible();
});

test("library tasks and baseline-candidate selection lead to paired evidence", async ({ page }) => {
  await installService(page, new MockResearchService());
  await page.goto("/experiments");
  await expect(page.getByRole("link", { name: "Research execution", exact: true })).toHaveAttribute("href", "/experiments/new?intent=execution");
  await expect(page.getByRole("link", { name: "Research market making", exact: true })).toHaveAttribute("href", "/experiments/new?intent=market-making");
  await page.getByRole("navigation", { name: "Primary", exact: true }).getByRole("link", { name: "Compare", exact: true }).click();
  await expect(page.getByRole("heading", { name: "Compare", exact: true })).toBeVisible();
  await page.getByRole("link", { name: "Compare this pair", exact: true }).first().click();
  await expect(page.getByRole("heading", { name: "Candidate impact across matched seeds" })).toBeVisible();
  await expect(page.getByLabel("Paired difference to inspect")).toBeVisible();
  await page.getByLabel("Paired difference to inspect").click();
  await page.getByRole("option", { name: /Max drawdown/ }).click();
  await expect(page.getByText("Negative differences favor the candidate.", { exact: false })).toBeVisible();
});

test("experiment overview exposes analysis and interrupted-work distinctions", async ({ page }) => {
  await installService(page, new MockResearchService());
  await page.goto("/experiments/exp_fixedspreadw2");
  await page.getByRole("tab", { name: "Runs and recovery", exact: true }).click();
  const overview = page.getByRole("region", { name: "Experiment overview" });
  await expect(overview.getByText("Seeds with completed runs")).toBeVisible();
  await expect(overview.getByText("Failed or cancelled runs")).toBeVisible();
  await overview.getByRole("link", { name: "Analyze a completed run" }).click();
  await expect(page.getByRole("heading", { name: "P&L and inventory risk" })).toBeVisible();
  await expect(page.getByRole("heading", { name: "Post-fill quality by horizon (USD/share)" })).toBeVisible();
});

test("timing units preserve nanoseconds and invalidate review only when assumptions change", async ({ page }) => {
  let preview: ApiSchemas["ExperimentCreateRequest"] | undefined;
  await installService(page, new MockResearchService(), async (_route, request) => {
    if (new URL(request.url).pathname === "/v1/experiment-previews") preview = JSON.parse(await request.clone().text());
    return false;
  });
  await page.goto("/experiments/new?intent=market-making");
  await page.getByText("Latency: feed, order, acknowledgment and cancel paths", { exact: true }).click();
  const feed = page.getByRole("textbox", { name: /^Feed/ });
  await page.getByRole("combobox", { name: "Display unit for Feed", exact: true }).click();
  await page.getByRole("option", { name: "ms", exact: true }).click();
  await expect(feed).toHaveValue("2");
  await feed.fill("");
  await feed.pressSequentially("0.000001");
  await expect(feed).toHaveValue("0.000001");
  await page.getByRole("button", { name: "Review assumptions" }).click();
  const launch = page.getByRole("button", { name: /Create and start/ });
  await expect(launch).toBeEnabled();
  expect(preview?.config.latency.feedLatencyNs).toBe("1");
  await page.getByRole("combobox", { name: "Display unit for Feed", exact: true }).click();
  await page.getByRole("option", { name: "s", exact: true }).click();
  await expect(feed).toHaveValue("0.000000001");
  await expect(launch).toBeEnabled();
  await feed.fill("0.0000000001");
  await expect(feed).toHaveValue("0.0000000001");
  await expect(launch).toBeDisabled();
  await expect(feed).toHaveAttribute("aria-invalid", "true");
  await feed.fill("0.000000002");
  await page.getByRole("button", { name: "Review assumptions" }).click();
  await expect(launch).toBeEnabled();
  expect(preview?.config.latency.feedLatencyNs).toBe("2");
});

test("paired run and fill inspection retain named experiment and comparison direction", async ({ page }) => {
  await installService(page, new MockResearchService());
  await page.goto("/compare?baseline=exp_fixedspreadw2&candidate=exp_fixedspreadw4");
  const evidence = page.getByRole("heading", { name: "Candidate impact across matched seeds" }).locator("..").locator("..");
  await evidence.getByRole("link", { name: /./ }).first().click();
  const context = page.getByRole("region", { name: "Research context" });
  await expect(context.getByText("Baseline", { exact: true })).toBeVisible();
  await expect(context.getByRole("link", { name: /Fixed spread/ })).toBeVisible();
  const query = new URL(page.url()).search;
  await page.getByRole("link", { name: "Inspect events", exact: true }).click();
  await expect(page).toHaveURL(/\/inspect\?/);
  await expect(context.getByText("Baseline", { exact: true })).toBeVisible();
  expect(new URL(page.url()).search).toBe(query);
  await context.getByRole("link", { name: "Return to baseline / candidate comparison" }).click();
  await expect(page).toHaveURL(/baseline=exp_fixedspreadw2&candidate=exp_fixedspreadw4/);
});

test("keyboard fill selection announces and reaches its explanation without losing the row", async ({ page }) => {
  await installService(page, new MockResearchService());
  await page.goto("/runs/run_fsw2seed303/inspect");
  const row = page.getByRole("table", { name: "Recorded fills" }).locator("tbody tr").first();
  await row.focus();
  await page.keyboard.press("Enter");
  await expect(page.getByRole("status").filter({ hasText: /Selected fill/ })).toBeVisible();
  await expect(row).toBeFocused();
  await page.keyboard.press("Tab");
  await expect(row.getByRole("button", { name: "Read explanation" })).toBeFocused();
  await page.keyboard.press("Enter");
  const explanation = page.getByRole("region", { name: "Selected order explanation" });
  await expect(explanation).toBeFocused();
  await expect(explanation.getByLabel("Selected fill identity")).toBeVisible();
  await explanation.getByRole("button", { name: "Return to selected row" }).click();
  await expect(row).toBeFocused();
});

test("time-series charts have distinct names and a keyboard-readable sampled alternative", async ({ page }) => {
  await installService(page, new MockResearchService());
  await page.goto("/runs/run_fsw2seed303");
  for (const title of ["Equity change from initial cash (USD)", "Inventory (shares)", "Drawdown (USD)"]) {
    await expect(page.getByRole("application", { name: title, exact: true })).toHaveCount(1);
    await page.getByText(`Read sampled values for ${title}`, { exact: true }).focus();
    await page.keyboard.press("Enter");
    const table = page.getByRole("table", { name: `${title} sampled values`, exact: true });
    await expect(table).toBeVisible();
    expect(await table.locator("tbody tr").count()).toBeGreaterThan(0);
  }
});

test("protocol empty states distinguish absent telemetry and offer built-in research", async ({ page }) => {
  await installService(page, new MockResearchService(), async (route, request, forward) => {
    if (!new URL(request.url).pathname.endsWith("/connections")) return false;
    const status = await (await forward()).json();
    await route.fulfill({ status: 200, json: { ...status, serviceState: "not_running", sessions: [], feeds: [], faults: [] } });
    return true;
  });
  await page.goto("/connections");
  await expect(page.getByRole("heading", { name: "No live gateway telemetry reported" })).toBeVisible();
  await expect(page.getByText(/This does not establish whether a separately launched server is running/)).toBeVisible();
  await expect(page.getByText(/No sessions supplied by this API/)).toBeVisible();
  await expect(page.getByText(/No fault telemetry supplied/)).toBeVisible();
  await page.getByRole("link", { name: "Configure built-in strategy research" }).click();
  await expect(page).toHaveURL(/\/experiments\/new$/);
});

test("a field-specific service error opens its section and focuses the invalid input", async ({ page }) => {
  await installService(page, new MockResearchService(), async (route, request) => {
    if (!new URL(request.url).pathname.endsWith("/experiment-previews")) return false;
    await route.fulfill({ status: 422, json: { error: { ...failure.error, code: "validation_failed", retryable: false, fieldIssues: [{ fieldPath: "/config/latency/feedLatencyNs", issue: "Feed latency must be nonnegative." }] } } });
    return true;
  });
  await page.goto("/experiments/new");
  await page.getByRole("button", { name: "Review assumptions" }).click();
  const input = page.getByRole("textbox", { name: /^Feed/ });
  await expect(input).toBeVisible();
  await expect(input).toBeFocused();
  await expect(input).toHaveAttribute("aria-invalid", "true");
  await expect(input).toHaveAccessibleDescription(/Feed latency must be nonnegative/);
  await expect(page.getByRole("button", { name: /Create and start/ })).toBeDisabled();
});

test("changing an order deep link on the same inspection route resets the explanation", async ({ page }) => {
  const service = new MockResearchService();
  const orders = await requestJson<ApiSchemas["OwnOrderPage"]>(service, "/v1/runs/run_fsw2seed303/orders?limit=2");
  expect(orders.items.length).toBe(2);
  await installService(page, service);
  await page.goto(`/runs/run_fsw2seed303/inspect?order=${orders.items[0].orderId}`);
  await expect(page.getByRole("heading", { name: `Order ${orders.items[0].orderId}: latest chain state`, exact: true })).toBeVisible();
  await page.evaluate((orderId) => window.history.pushState(null, "", `/runs/run_fsw2seed303/inspect?order=${orderId}`), orders.items[1].orderId);
  await expect(page.getByRole("heading", { name: `Order ${orders.items[1].orderId}: latest chain state`, exact: true })).toBeVisible();
});
