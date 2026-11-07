import { expect, test, type Page } from "@playwright/test";
import { spawn } from "node:child_process";
import { mkdtempSync, rmSync } from "node:fs";
import { readdirSync, readFileSync, readlinkSync, statSync } from "node:fs";
import { createHash } from "node:crypto";
import { tmpdir } from "node:os";
import { resolve, join } from "node:path";
import type { ApiSchemas } from "../src/lib/api/exsim-api-client";

function standaloneInventory() {
  const entries: Record<string, string> = {};
  function visit(directory: string, prefix: string) {
    for (const entry of readdirSync(directory, { withFileTypes: true })) {
      const path = join(directory, entry.name);
      const relative = join(prefix, entry.name);
      if (entry.isDirectory()) {
        entries[relative] = "directory";
        visit(path, relative);
      } else if (entry.isSymbolicLink()) {
        entries[relative] = `link:${readlinkSync(path)}`;
      } else {
        entries[relative] = `${createHash("sha256").update(readFileSync(path)).digest("hex")}:${statSync(path).mode & 0o111}`;
      }
    }
  }
  visit(resolve(".next/standalone"), "");
  return entries;
}

// Real process, ephemeral port and private disposable data. No mock execution
// implementation and no request can reach the installed service.
async function isolatedApi() {
  const data = mkdtempSync(join(tmpdir(), "exsim-browser-execution-"));
  const child = spawn(process.env.EXSIM_API_BINARY ?? resolve("../build/release/exsim_api"), ["--bind", "127.0.0.1", "--port", "3763", "--data-dir", data], { stdio: ["ignore", "pipe", "pipe"] });
  let output = "";
  let diagnostics = "";
  const exited = new Promise<void>((done) => child.once("close", () => done()));
  child.once("error", () => {});
  child.stderr.on("data", (chunk: Buffer) => { diagnostics = (diagnostics + chunk.toString()).slice(-8000); });
  const stop = async () => {
    if (child.exitCode === null) {
      child.kill("SIGTERM");
      const force = setTimeout(() => child.kill("SIGKILL"), 1500);
      try { await exited; } finally { clearTimeout(force); }
    }
    rmSync(data, { recursive: true, force: true });
  };
  let baseUrl: string;
  try {
    baseUrl = await new Promise<string>((done, reject) => {
      const timeout = setTimeout(() => reject(new Error(`API readiness timeout: ${diagnostics}`)), 10_000);
      const finish = (error?: Error, url?: string) => { clearTimeout(timeout); if (error) reject(error); else done(url!); };
      child.once("error", (error) => finish(error));
      child.once("exit", (code) => finish(new Error(`API exited ${code}: ${diagnostics}`)));
      child.stdout.on("data", (chunk: Buffer) => {
        output += chunk.toString();
        const match = output.match(/listening on (http:\/\/\S+)/);
        if (match) finish(undefined, match[1]);
      });
    });

  } catch (error) {
    await stop();
    throw error;
  }
  return { baseUrl, stop };
}

async function openRun(page: Page) {
  const link = page.getByRole("link", { name: /^run_/ }).first();
  await expect(link).toBeVisible();
  await link.click();
  await expect(page.getByText("Run complete.", { exact: false })).toBeVisible();
  return new URL(page.url()).pathname.split("/").at(-1)!;
}

async function fillTimingNs(page: Page, label: string, value: string) {
  await page.getByRole("combobox", { name: `Display unit for ${label}`, exact: true }).click();
  await page.getByRole("option", { name: "ns", exact: true }).click();
  await page.getByRole("textbox", { name: new RegExp(`^${label}`) }).fill(value);
}

async function startReviewed(page: Page, name: string) {
  await page.getByRole("heading", { name: "New experiment" }).scrollIntoViewIfNeeded();
  const nameInput = page.getByLabel("Name", { exact: true });
  await expect(nameInput).toBeVisible();
  await nameInput.fill(name);
  await page.getByRole("button", { name: "Review assumptions" }).click();
  await expect(page.getByRole("button", { name: "Create and start 1 run" })).toBeEnabled();
  await page.getByRole("button", { name: "Create and start 1 run" }).click();
  await expect(page.getByRole("heading", { name, exact: true })).toBeVisible();
  return new URL(page.url()).pathname.split("/").at(-1)!;
}

test("a trader compares real TWAP and sweep runs with explicit parent costs and residuals", async ({ page }) => {
  const releaseBefore = standaloneInventory();
  const api = await isolatedApi();
  try {
    await page.goto("/experiments/new?intent=execution");
    await expect(page.getByLabel("Execution method")).toBeVisible();
    await page.getByLabel(/Master seeds/).fill("11");
    await page.getByLabel(/Parent quantity/).fill("500");
    await page.getByLabel("Parent side", { exact: true }).click();
    await page.getByRole("option", { name: "Sell", exact: true }).click();
    await fillTimingNs(page, "Parent start", "100000000");
    await fillTimingNs(page, "Execution window", "2000000000");
    await fillTimingNs(page, "Slice interval", "1000000000");
    await page.getByText("Fees and risk: costs, position limits and order caps", { exact: true }).click();
    await page.getByLabel(/Max child order quantity/).fill("100");
    await fillTimingNs(page, "Simulation end", "5000000000");
    const baselineId = await startReviewed(page, "Browser TWAP execution");
    const twapRun = await openRun(page);
    await expect(page.getByRole("heading", { name: "Parent-order execution" })).toBeVisible();
    const summary: ApiSchemas["RunSummary"] = await (await fetch(`${api.baseUrl}/v1/runs/${twapRun}/summary`)).json();
    expect(summary.execution).toMatchObject({ parentSide: "sell", parentQuantity: "500", observationStatus: "window_observed", unexecutedOpportunityCost: null });
    const residual = summary.metrics.find((metric) => metric.metricKey === "residual_qty")!;
    expect(BigInt(residual.value!)).toBeGreaterThan(0n);
    await expect(page.getByLabel("Parent execution metrics")).toContainText("Parent residual quantity");
    await expect(page.getByLabel("Parent execution metrics")).toContainText(residual.value!);
    await expect(page.getByText(/Costs cover filled quantity only/)).toBeVisible();
    await page.goto(`/experiments/${baselineId}`);
    await page.getByRole("link", { name: "Create variant" }).click();
    await page.getByLabel("Execution method").click();
    await page.getByRole("option", { name: "Immediate sweep", exact: true }).click();
    const candidateId = await startReviewed(page, "Browser sweep execution");
    const sweepRun = await openRun(page);
    const sweep: ApiSchemas["RunSummary"] = await (await fetch(`${api.baseUrl}/v1/runs/${sweepRun}/summary`)).json();
    expect(sweep.execution?.parentQuantity).toBe("500");
    expect(sweep.metrics.find((metric) => metric.metricKey === "completion_ratio")?.value).not.toBeNull();
    await page.goto(`/compare?baseline=${baselineId}&candidate=${candidateId}`);
    await expect(page.getByRole("cell", { name: "Parent completion", exact: false }).first()).toBeVisible();
    await expect(page.getByRole("cell", { name: "Executed cost including fees", exact: false }).first()).toBeVisible();
    const comparison: ApiSchemas["ExperimentComparison"] = await (await fetch(`${api.baseUrl}/v1/comparisons?baselineExperimentId=${baselineId}&candidateExperimentId=${candidateId}&maxPoints=10`)).json();
    expect(comparison.comparable).toBe(true);
    expect(comparison.seedPairing.pairedSeeds).toEqual(["11"]);
    expect(comparison.metricComparisons.find((metric) => metric.metricKey === "completion_ratio")?.dispersion.sampleCount).toBe(1);
  } finally {
    await api.stop();
    expect(standaloneInventory()).toEqual(releaseBefore);
  }
});

test("inventory-aware quoting is reviewed and executed by the advertised C++ strategy", async ({ page }) => {
  const api = await isolatedApi();
  try {
    await page.goto("/experiments/new");
    await page.getByLabel("Quoting model").click();
    await page.getByRole("option", { name: "Inventory-aware market maker" }).click();
    await page.getByLabel(/Inventory skew/).fill("2");
    await page.getByLabel(/Master seeds/).fill("12");
    await fillTimingNs(page, "Simulation end", "2000000000");
    await startReviewed(page, "Browser inventory-aware execution");
    const runId = await openRun(page);
    const manifest: ApiSchemas["RunManifest"] = await (await fetch(`${api.baseUrl}/v1/runs/${runId}/manifest`)).json();
    expect(manifest.strategyBuildId).toContain("inventory_aware");
    const summary: ApiSchemas["RunSummary"] = await (await fetch(`${api.baseUrl}/v1/runs/${runId}/summary`)).json();
    expect(summary.execution).toBeNull();
    await expect(page.getByRole("heading", { name: "Parent-order execution", exact: true })).toHaveCount(0);
  } finally { await api.stop(); }
});
