import { expect, test, type Request as BrowserRequest, type Route } from "@playwright/test";
import { MockResearchService } from "../mock-server/mock-research-service";
import type { ApiSchemas } from "../../src/lib/api/exsim-api-client";

// Rendering/identity fixtures, not a second matching or accounting model.
for (const mode of ["stationary", "pending", "off-page"] as const) {
  test(`sibling fills keep distinct explanations: ${mode}`, async ({ page }) => {
    const service = new MockResearchService();
    const runId = "run_fsw2seed303";
    const response = await service.handleRequest(new Request(`http://fixture.exsim/v1/runs/${runId}/fills?limit=200`));
    const fixture: ApiSchemas["FillPage"] = await response.json();
    const first = fixture.items[0];
    const second = { ...structuredClone(first), eventPosition: fixture.items[1].eventPosition, simulationTimeNs: fixture.items[1].simulationTimeNs, matchId: fixture.items[1].matchId };
    second.relatedEvents.fillEventPosition = second.eventPosition;
    second.ledgerEffect.positionBeforeShares = "100";
    second.ledgerEffect.positionAfterShares = "200";
    let selectedReads = 0;
    let pendingRoute: Route | null = null;
    let firstRequest: BrowserRequest | null = null;
    let firstRequestAborted = false;
    page.on("requestfailed", (request) => { if (request === firstRequest) firstRequestAborted = true; });

    await page.route("**/api/exsim/**", async (route) => {
      const incoming = route.request();
      const url = new URL(incoming.url());
      if (url.pathname === `/api/exsim/v1/runs/${runId}/fills`) {
        if (url.searchParams.has("orderId")) {
          selectedReads++;
          if (mode === "pending" && selectedReads === 1) {
            pendingRoute = route;
            firstRequest = incoming;
            return;
          }
          await route.fulfill({ json: { items: [first, second], page: { nextCursor: null, limit: 200 } } });
        } else if (mode === "off-page") {
          const next = url.searchParams.has("cursor");
          await route.fulfill({ json: { items: [next ? second : first], page: { nextCursor: next ? null : "second-fill-page", limit: 200 } } });
        } else {
          await route.fulfill({ json: { items: [first, second], page: { nextCursor: null, limit: 200 } } });
        }
        return;
      }
      const result = await service.handleRequest(new Request(`http://fixture.exsim${url.pathname.replace("/api/exsim", "")}${url.search}`, { method: incoming.method(), headers: incoming.headers(), body: incoming.postData() }));
      if (url.pathname.endsWith(`/orders/${first.orderId}/lifecycle`)) {
        const lifecycle: ApiSchemas["OrderLifecycle"] = await result.json();
        lifecycle.order.quantity = "200";
        lifecycle.order.cumulativeQuantity = "200";
        lifecycle.order.fillCount = 2;
        await route.fulfill({ json: lifecycle });
        return;
      }
      await route.fulfill({ status: result.status, headers: Object.fromEntries(result.headers), body: await result.text() });
    });

    await page.goto(`/runs/${runId}/inspect`);
    const rows = page.getByRole("table", { name: "Recorded fills" }).locator("tbody tr");
    await expect(rows).toHaveCount(mode === "off-page" ? 1 : 2);
    await rows.first().click();
    await expect.poll(() => selectedReads).toBe(1);
    await expect(page.getByRole("heading", { name: new RegExp(`^Fill ${first.matchId}:`) })).toBeVisible();
    if (mode === "stationary" || mode === "off-page") {
      await page.evaluate(() => new Promise<void>((done) => requestAnimationFrame(() => requestAnimationFrame(() => done()))));
    }
    if (mode === "off-page") {
      await page.getByRole("button", { name: "Next fills" }).click();
      await expect(page.getByText(/Fills, page 2:/)).toBeVisible();
      await rows.first().click();
    } else {
      await rows.nth(1).click();
    }
    await expect.poll(() => selectedReads).toBe(2);
    await expect(page.getByRole("heading", { name: new RegExp(`^Fill ${second.matchId}:`) })).toBeVisible();
    await expect(page.getByRole("heading", { name: new RegExp(`^Fill ${first.matchId}:`) })).toHaveCount(0);
    if (mode === "pending") {
      await expect.poll(() => firstRequestAborted).toBe(true);
      // The old intercepted route may already be gone because its fetch aborted.
      await (pendingRoute as Route | null)?.fulfill({ json: { items: [first, second], page: { nextCursor: null, limit: 200 } } }).catch(() => {});
      await page.evaluate(() => new Promise<void>((done) => requestAnimationFrame(() => requestAnimationFrame(() => done()))));
      await expect(page.getByRole("heading", { name: new RegExp(`^Fill ${second.matchId}:`) })).toBeVisible();
    }
    if (mode === "off-page") {
      await page.getByRole("button", { name: "Previous fills" }).click();
      await expect(page.getByText(/Fills, page 1:/)).toBeVisible();
      await expect(page.getByRole("heading", { name: new RegExp(`^Fill ${second.matchId}:`) })).toBeVisible();
      expect(selectedReads).toBe(2);
    }
  });
}
