import { expect, test, type Page, type Route } from "@playwright/test";
import { MockResearchService } from "../mock-server/mock-research-service";
import type { ApiSchemas } from "../src/lib/api/exsim-api-client";

const runId = "run_fsw2seed303";
const otherRunId = "run_fsw2seed101";
type Fill = ApiSchemas["Fill"];

async function fixture() {
  const service = new MockResearchService();
  const response = await service.handleRequest(new Request(`http://fixture.exsim/v1/runs/${runId}/fills?limit=200`));
  const fills: ApiSchemas["FillPage"] = await response.json();
  const first = structuredClone(fills.items[0]);
  const second = structuredClone(first);
  second.eventPosition = fills.items[1].eventPosition;
  second.simulationTimeNs = fills.items[1].simulationTimeNs;
  second.matchId = fills.items[1].matchId;
  second.relatedEvents.fillEventPosition = second.eventPosition;
  second.relatedEvents.fillReportDeliveredEventPosition = "93";
  second.ledgerEffect.positionBeforeShares = "100";
  second.ledgerEffect.positionAfterShares = "200";
  second.markouts[0].status = "available";
  second.markouts[0].markoutTotal = "17.25";
  return { service, first, second };
}

const fillPage = (items: Fill[], nextCursor: string | null = null) => ({ items, page: { nextCursor, limit: 200 } });
const rows = (page: Page) => page.getByRole("table", { name: "Recorded fills" }).locator("tbody tr");
const heading = (page: Page, fill: Fill) => page.getByRole("heading", { name: new RegExp(`^Fill ${fill.matchId}:`) });
const position = (page: Page) => page.getByRole("row").filter({ has: page.getByRole("cell", { name: "Position (shares)", exact: true }) }).getByRole("cell").last();

// Exercise the hook's completion guard even when transport ignores AbortSignal.
// Ordinary focused coverage separately verifies real browser fetch cancellation.
async function ignoreSelectedAbort(page: Page) {
  await page.addInitScript(() => {
    const original = window.fetch;
    const detachedFetch: typeof original = Object.assign((input: RequestInfo | URL, init?: RequestInit) => {
      const request = new Request(input, init);
      const url = new URL(request.url);
      if (url.pathname.endsWith("/fills") && url.searchParams.has("orderId")) {
        return original(new Request(request, { signal: new AbortController().signal }));
      }
      return original(input, init);
    }, original);
    window.fetch = detachedFetch;
  });
}

async function forwardFixture(route: Route, service: MockResearchService) {
  const incoming = route.request();
  const url = new URL(incoming.url());
  const path = url.pathname.replace("/api/exsim", "").replace(otherRunId, runId);
  const response = await service.handleRequest(new Request(`http://fixture.exsim${path}${url.search}`, {
    method: incoming.method(), headers: incoming.headers(), body: incoming.postData(),
  }));
  if (url.pathname === `/api/exsim/v1/runs/${otherRunId}`) {
    const run: ApiSchemas["Run"] = await response.json();
    await route.fulfill({ json: { ...run, runId: otherRunId } });
    return;
  }
  await route.fulfill({ status: response.status, headers: Object.fromEntries(response.headers), body: await response.text() });
}

async function painted(page: Page) {
  await page.evaluate(() => new Promise<void>((done) => requestAnimationFrame(() => requestAnimationFrame(() => done()))));
}

test("late uncancelled sibling response cannot replace ledger, markout or related-event links", async ({ page }) => {
  const { service, first, second } = await fixture();
  await ignoreSelectedAbort(page);
  let reads = 0;
  let pending: Route | null = null;
  const freshSecond = structuredClone(second);
  freshSecond.ledgerEffect.positionAfterShares = "222";
  await page.route("**/api/exsim/**", async (route) => {
    const url = new URL(route.request().url());
    if (url.pathname.endsWith("/fills")) {
      if (url.searchParams.has("orderId") && ++reads === 1) { pending = route; return; }
      await route.fulfill({ json: fillPage(url.searchParams.has("orderId") ? [first, freshSecond] : [first, second]) });
      return;
    }
    await forwardFixture(route, service);
  });
  await page.goto(`/runs/${runId}/inspect`);
  await rows(page).nth(0).click();
  await expect.poll(() => pending !== null).toBe(true);
  await rows(page).nth(1).click();
  await expect(position(page)).toHaveText("222"); // Selected response, not table fallback.
  await expect(heading(page, second)).toBeVisible();
  await expect(page.getByRole("button", { name: "Fill #90", exact: true })).toBeVisible();
  await expect(page.getByRole("button", { name: "Fill report delivered #93", exact: true })).toBeVisible();
  await expect(page.getByRole("cell", { name: "+17.25", exact: true })).toBeVisible();
  const oldResponse = page.waitForResponse((response) => response.url().includes("orderId=") && response.request().method() === "GET");
  await (pending as Route | null)!.fulfill({ json: fillPage([first, second]) });
  await oldResponse;
  await painted(page);
  await expect(position(page)).toHaveText("222");
  await expect(heading(page, first)).toHaveCount(0);
  expect(reads).toBe(2);
});

test("run route change resets selection and cursor and rejects a late colliding run result", async ({ page }) => {
  const { service, first, second } = await fixture();
  await ignoreSelectedAbort(page);
  let pending: Route | null = null;
  const newSecond = structuredClone(second);
  newSecond.ledgerEffect.positionAfterShares = "333";
  await page.route("**/api/exsim/**", async (route) => {
    const url = new URL(route.request().url());
    const otherRun = url.pathname.includes(otherRunId);
    if (url.pathname.endsWith("/fills")) {
      if (url.searchParams.has("orderId")) {
        if (!otherRun) { pending = route; return; }
        await route.fulfill({ json: fillPage([first, newSecond]) });
      } else {
        const next = url.searchParams.has("cursor");
        await route.fulfill({ json: fillPage([next ? second : first], next ? null : "page-two") });
      }
      return;
    }
    await forwardFixture(route, service);
  });
  await page.goto(`/runs/${runId}/inspect`);
  await page.getByRole("button", { name: "Next fills" }).click();
  await expect(page.getByText(/Fills, page 2:/)).toBeVisible();
  await rows(page).first().click();
  await expect.poll(() => pending !== null).toBe(true);
  // Follow real Next links, keeping the document alive and the old read pending.
  const documentId = await page.evaluate(() => performance.timeOrigin);
  await page.getByRole("link", { name: "Run summary", exact: true }).click();
  await expect(page).toHaveURL(new RegExp(`/runs/${runId}$`));
  await expect(page.getByRole("heading", { name: "Seed 303 completed", exact: true })).toBeVisible();
  await page.getByRole("region", { name: "Research context" }).getByRole("link", { name: "Fixed spread 2 ticks (baseline)", exact: true }).click();
  await page.getByRole("tab", { name: "Runs and recovery", exact: true }).click();
  await page.getByRole("row").filter({ has: page.getByRole("link", { name: otherRunId, exact: true }) }).getByRole("link", { name: "events", exact: true }).click();
  expect(await page.evaluate(() => performance.timeOrigin)).toBe(documentId);
  await expect(page.getByText(otherRunId, { exact: true })).toBeVisible();
  await expect(page.getByText(/Fills, page 1:/)).toBeVisible();
  await expect(page.getByRole("heading", { name: /^Fill \d+:/ })).toHaveCount(0);
  await expect(page.getByRole("table", { name: "Recorded fills" }).locator('tbody tr[data-state="selected"]')).toHaveCount(0);
  await page.getByRole("button", { name: "Next fills" }).click();
  await expect(page.getByText(/Fills, page 2:/)).toBeVisible();
  await rows(page).first().click();
  await expect(position(page)).toHaveText("333");
  const oldResponse = page.waitForResponse((response) => response.url().includes(`/runs/${runId}/fills?`) && response.url().includes("orderId="));
  await (pending as Route | null)!.fulfill({ json: fillPage([first, second]) });
  await oldResponse;
  await painted(page);
  await expect(position(page)).toHaveText("333");
  await expect(heading(page, second)).toBeVisible();
});

test("selecting an own order clears fill evidence and aborts its pending search", async ({ page }) => {
  const { service, first, second } = await fixture();
  let pending: Route | null = null;
  let aborted = false;
  page.on("requestfailed", (request) => {
    if (request.url().includes("/fills?") && request.url().includes("orderId=")) aborted = true;
  });
  await page.route("**/api/exsim/**", async (route) => {
    const url = new URL(route.request().url());
    if (url.pathname.endsWith("/fills")) {
      if (url.searchParams.has("orderId")) { pending = route; return; }
      await route.fulfill({ json: fillPage([first, second]) });
      return;
    }
    await forwardFixture(route, service);
  });
  await page.goto(`/runs/${runId}/inspect`);
  await rows(page).first().click();
  await expect.poll(() => pending !== null).toBe(true);
  await expect(heading(page, first)).toBeVisible();
  await page.getByRole("tab", { name: "Own orders", exact: true }).click();
  await page.getByRole("table", { name: "Recorded own orders" }).locator("tbody tr").first().click();
  await expect.poll(() => aborted).toBe(true);
  await expect(page.getByRole("heading", { name: /^Order / })).toBeVisible();
  await expect(heading(page, first)).toHaveCount(0);
  await (pending as Route | null)!.fulfill({ json: fillPage([first, second]) }).catch(() => {});
  await painted(page);
  await expect(heading(page, first)).toHaveCount(0);
  await expect(page.getByText("Could not refresh the selected fill", { exact: true })).toHaveCount(0);
});

test("off-page failed sibling search retries exact identity through cursors and stops at the match", async ({ page }) => {
  const { service, first, second } = await fixture();
  const wrongOrder = structuredClone(second);
  wrongOrder.orderId = "999999";
  wrongOrder.ledgerEffect.positionAfterShares = "999999";
  const freshSecond = structuredClone(second);
  freshSecond.ledgerEffect.positionAfterShares = "444";
  let searches = 0;
  const searchCursors: (string | null)[] = [];
  await page.route("**/api/exsim/**", async (route) => {
    const url = new URL(route.request().url());
    if (url.pathname.endsWith("/fills")) {
      if (url.searchParams.has("orderId")) {
        searches++;
        searchCursors.push(url.searchParams.get("cursor"));
        if (searches === 1) await route.fulfill({ json: fillPage([first]) });
        else if (searches === 2) await route.fulfill({ status: 503, json: { error: { code: "service_unavailable", message: "review selected search unavailable", requestId: "review", retryable: true, fieldIssues: [] } } });
        else if (!url.searchParams.has("cursor")) await route.fulfill({ json: fillPage([first, wrongOrder], "selected-next") });
        else if (url.searchParams.get("cursor") === "selected-next") await route.fulfill({ json: fillPage([freshSecond], "must-not-read") });
        else throw new Error("selected search read past its match");
      } else {
        const next = url.searchParams.has("cursor");
        await route.fulfill({ json: fillPage([next ? second : first], next ? null : "table-next") });
      }
      return;
    }
    await forwardFixture(route, service);
  });
  await page.goto(`/runs/${runId}/inspect`);
  await rows(page).first().click();
  await expect(heading(page, first)).toBeVisible();
  await expect.poll(() => searches).toBe(1);
  await painted(page);
  await page.getByRole("button", { name: "Next fills" }).click();
  await expect(page.getByText(/Fills, page 2:/)).toBeVisible();
  await rows(page).first().click();
  await expect(page.getByText("Could not refresh the selected fill", { exact: true })).toBeVisible();
  await expect(heading(page, second)).toBeVisible(); // Current table fallback.
  await page.getByRole("button", { name: "Previous fills" }).click();
  await expect(page.getByText(/Fills, page 1:/)).toBeVisible();
  await expect(page.getByRole("heading", { name: /^Fill \d+:/ })).toHaveCount(0);
  await page.getByRole("button", { name: "Try again", exact: true }).click();
  await expect(position(page)).toHaveText("444");
  await expect(heading(page, second)).toBeVisible();
  await expect(heading(page, first)).toHaveCount(0);
  await expect(page.getByText("Could not refresh the selected fill", { exact: true })).toHaveCount(0);
  expect(searchCursors).toEqual([null, null, null, "selected-next"]);
});

test("run progress refresh cancels the prior search and reloads the same selected identity", async ({ page }) => {
  const { service, first, second } = await fixture();
  let advanced = false;
  let reads = 0;
  let pending: Route | null = null;
  let aborted = false;
  const refreshed = structuredClone(second);
  refreshed.ledgerEffect.positionAfterShares = "666";
  page.on("requestfailed", (request) => {
    if (request.url().includes("/fills?") && request.url().includes("orderId=")) aborted = true;
  });
  await page.route("**/api/exsim/**", async (route) => {
    const url = new URL(route.request().url());
    if (url.pathname === `/api/exsim/v1/runs/${runId}`) {
      const response = await service.handleRequest(new Request(`http://fixture.exsim/v1/runs/${runId}`));
      const run: ApiSchemas["Run"] = await response.json();
      run.state = advanced ? "completed" : "paused";
      if (advanced) run.progress.processedCanonicalEventCount = (BigInt(run.progress.processedCanonicalEventCount) + 1n).toString();
      await route.fulfill({ json: run });
      return;
    }
    if (url.pathname.endsWith("/fills")) {
      if (url.searchParams.has("orderId")) {
        if (++reads === 1) { pending = route; return; }
        await route.fulfill({ json: fillPage([first, refreshed]) });
      } else await route.fulfill({ json: fillPage([first, second]) });
      return;
    }
    await forwardFixture(route, service);
  });
  await page.goto(`/runs/${runId}/inspect`);
  await rows(page).nth(1).click();
  await expect.poll(() => pending !== null).toBe(true);
  await expect(position(page)).toHaveText("200");
  advanced = true;
  await expect.poll(() => reads).toBe(2);
  await expect.poll(() => aborted).toBe(true);
  await expect(position(page)).toHaveText("666");
  await expect(rows(page).nth(1)).toHaveAttribute("data-state", "selected");
  await (pending as Route | null)!.fulfill({ json: fillPage([first, second]) }).catch(() => {});
  await painted(page);
  await expect(position(page)).toHaveText("666");
  await expect(heading(page, second)).toBeVisible();
  expect(reads).toBe(2);
});
