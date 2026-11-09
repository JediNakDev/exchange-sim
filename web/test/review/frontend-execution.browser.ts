import { expect, test } from "@playwright/test";
import { MockResearchService } from "../mock-server/mock-research-service";
import type { ApiSchemas } from "../../src/lib/api/exsim-api-client";

// Focused real-renderer reproducer. Two fills share an order, which the normal
// mock fixture does not exercise. All research traffic stays in this fixture.
test("FER-01: a second fill of the same order replaces the selected explanation", async ({ page }) => {
  const service = new MockResearchService();
  const runId = "run_fsw2seed303";
  const fixtureResponse = await service.handleRequest(new Request(`http://fixture.exsim/v1/runs/${runId}/fills?limit=200`));
  const fixture: ApiSchemas["FillPage"] = await fixtureResponse.json();
  const first = fixture.items[0];
  const second = { ...structuredClone(first), eventPosition: fixture.items[1].eventPosition, simulationTimeNs: fixture.items[1].simulationTimeNs, matchId: fixture.items[1].matchId };
  second.relatedEvents.fillEventPosition = second.eventPosition;
  second.ledgerEffect.positionBeforeShares = "100";
  second.ledgerEffect.positionAfterShares = "200";
  expect(second.eventPosition).not.toBe(first.eventPosition);
  expect(second.matchId).not.toBe(first.matchId);
  let selectedReads = 0;

  await page.route("**/api/exsim/**", async (route) => {
    const incoming = route.request();
    const url = new URL(incoming.url());
    if (url.pathname === `/api/exsim/v1/runs/${runId}/fills`) {
      if (url.searchParams.has("orderId")) selectedReads++;
      await route.fulfill({ json: { items: [first, second], page: { nextCursor: null, limit: 200 } } });
      return;
    }
    const response = await service.handleRequest(new Request(`http://fixture.exsim${url.pathname.replace("/api/exsim", "")}${url.search}`, {
      method: incoming.method(), headers: incoming.headers(), body: incoming.postData(),
    }));
    if (url.pathname.endsWith(`/orders/${first.orderId}/lifecycle`)) {
      const lifecycle: ApiSchemas["OrderLifecycle"] = await response.json();
      lifecycle.order.quantity = "200";
      lifecycle.order.cumulativeQuantity = "200";
      lifecycle.order.fillCount = 2;
      await route.fulfill({ json: lifecycle });
      return;
    }
    await route.fulfill({ status: response.status, headers: Object.fromEntries(response.headers), body: await response.text() });
  });

  await page.goto(`/runs/${runId}/inspect`);
  const rows = page.getByRole("table", { name: "Recorded fills" }).locator("tbody tr");
  await expect(rows).toHaveCount(2);
  await rows.nth(0).click();
  await expect.poll(() => selectedReads).toBe(1);
  await expect(page.getByRole("heading", { name: new RegExp(`^Fill ${first.matchId}:`) })).toBeVisible();
  // Let the first selected resource commit before selecting its sibling.
  await page.evaluate(() => new Promise<void>((done) => requestAnimationFrame(() => requestAnimationFrame(() => done()))));
  await rows.nth(1).click();
  await expect(rows.nth(1)).toHaveAttribute("data-state", "selected");
  await expect(page.getByRole("heading", { name: new RegExp(`^Fill ${second.matchId}:`) })).toBeVisible();
  await expect(page.getByRole("heading", { name: new RegExp(`^Fill ${first.matchId}:`) })).toHaveCount(0);
  expect(selectedReads).toBe(2);
});
