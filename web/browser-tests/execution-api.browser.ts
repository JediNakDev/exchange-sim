import { expect, test, type Page } from "@playwright/test";
import { spawn, spawnSync } from "node:child_process";
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
  const child = spawn(process.env.EXSIM_API_BINARY ?? resolve("../build/release/exsim_api"), ["--bind", "127.0.0.1", "--port", "3763", "--data-dir", data, "--source-revision", process.env.EXSIM_API_SOURCE_REVISION ?? "5394b84", "--release-id", "persona-browser-test"], { stdio: ["ignore", "pipe", "pipe"] });
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
    await page.setViewportSize({ width: 390, height: 844 });
    await expect(page.getByRole("table", { name: "Outbound request diagnostics", exact: true })).toBeVisible();
    expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBe(390);
    await page.setViewportSize({ width: 1440, height: 1000 });
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


test("native evidence UI excludes censored windows, pins attempts and exports a reproducible audit", async ({ page }) => {
  test.setTimeout(45_000);
  const api = await isolatedApi();
  const downloadDirectory = mkdtempSync(join(tmpdir(), "exsim-persona-audit-"));
  const call = async <T>(path: string, body?: unknown, key = `persona-${Date.now()}-${Math.random().toString(36).slice(2)}`): Promise<T> => {
    const response = await fetch(`${api.baseUrl}${path}`, body === undefined ? {} : { method: "POST", headers: { "Content-Type": "application/json", "Idempotency-Key": key }, body: JSON.stringify(body) });
    expect(response.ok, response.ok ? "" : await response.clone().text()).toBe(true);
    return response.json();
  };
  try {
    const baselines = await call<ApiSchemas["BaselineConfigurationList"]>("/v1/baselines");
    const baseline = baselines.items.find((item) => item.config.strategy.strategyKind === "twap_execution")!;
    const config = structuredClone(baseline.config);
    config.seedSet.masterSeeds = ["101", "102", "103"];
    const original = await call<ApiSchemas["Experiment"]>("/v1/experiments", { name: "Native persona baseline", derivedFromBaselineId: baseline.baselineId, config });
    const launch = async (experimentId: string, seeds: string[]) => {
      const runs: ApiSchemas["Run"][] = [];
      for (const masterSeed of seeds) runs.push(await call<ApiSchemas["Run"]>("/v1/runs", { experimentId, masterSeed, startPaused: false }));
      for (const run of runs) await expect.poll(async () => (await call<ApiSchemas["Run"]>(`/v1/runs/${run.runId}`)).state).toBe("completed");
      return runs;
    };
    const originalRuns = await launch(original.experimentId, config.seedSet.masterSeeds);
    const censoredConfig = structuredClone(config);
    censoredConfig.bounds.endSimulationTimeNs = "5000000000";
    censoredConfig.seedSet.masterSeeds = ["101"];
    const censored = await call<ApiSchemas["Experiment"]>("/v1/experiments", { name: "Censored candidate", derivedFromBaselineId: baseline.baselineId, parentExperimentId: original.experimentId, config: censoredConfig });
    await launch(censored.experimentId, ["101"]);
    await page.goto(`/compare?baseline=${original.experimentId}&candidate=${censored.experimentId}`);
    await expect(page.getByText(/1 execution windows cut short/)).toBeVisible();
    await expect(page.getByRole("heading", { name: "Candidate impact across matched seeds" })).toHaveCount(0);
    await page.getByText("Inspect every planned seed and run attempt", { exact: true }).click();
    await expect(page.getByRole("table", { name: "Comparison attempt ledger" })).toContainText("censored");
    const candidateConfig = structuredClone(config);
    candidateConfig.strategy = { strategyKind: "immediate_sweep_execution", parentSide: "buy", parentQuantity: "500", startTimeNs: "1000000000", executionDurationNs: "30000000000", limitOffsetTicks: "5" };
    const candidate = await call<ApiSchemas["Experiment"]>("/v1/experiments", { name: "Observed candidate", derivedFromBaselineId: baseline.baselineId, parentExperimentId: original.experimentId, config: candidateConfig });
    await launch(candidate.experimentId, config.seedSet.masterSeeds);
    await page.goto(`/compare?baseline=${original.experimentId}&candidate=${candidate.experimentId}`);
    await page.getByRole("button", { name: "Pin selected attempts" }).click();
    await expect(page).toHaveURL(/runSelection=/);
    const pinnedUrl = page.url();
    await launch(original.experimentId, ["101"]);
    await page.goto(pinnedUrl);
    await page.getByText("Inspect every planned seed and run attempt", { exact: true }).click();
    const selectedRow = page.getByRole("table", { name: "Comparison attempt ledger" }).getByRole("row").filter({ hasText: originalRuns[0].runId });
    await expect(selectedRow).toContainText("Selected");
    const receiptEvent = page.waitForEvent("download");
    await page.getByRole("button", { name: "Download comparison receipt" }).click();
    const receipt = await receiptEvent;
    const receiptPath = join(downloadDirectory, "receipt.json");
    await receipt.saveAs(receiptPath);
    expect(JSON.parse(readFileSync(receiptPath, "utf8")).query.runSelection).toBe(new URL(pinnedUrl).searchParams.get("runSelection"));
    await page.goto(`/runs/${originalRuns[1].runId}`);
    await expect(page.getByRole("heading", { name: "Native execution trace" })).toBeVisible();
    await page.getByRole("tab", { name: "Manifest", exact: true }).click();
    await expect(page.getByText("Recorded source revision", { exact: true })).toBeVisible();
    await expect(page.getByText(process.env.EXSIM_API_SOURCE_REVISION ?? "5394b84", { exact: true })).toBeVisible();
    const auditEvent = page.waitForEvent("download");
    await page.getByRole("button", { name: "Download complete audit" }).click();
    const download = await auditEvent;
    const packagePath = join(downloadDirectory, "audit.json");
    await download.saveAs(packagePath);
    const output = join(downloadDirectory, "unpacked");
    const unpack = spawnSync("python3", [resolve("../scripts/research_audit.py"), "unpack", packagePath, output], { encoding: "utf8" });
    expect(unpack.status, unpack.stderr).toBe(0);
    expect(readFileSync(join(output, "runner/events.jsonl"), "utf8").trim().split("\n").length).toBeGreaterThan(1000);
    const verify = spawnSync(resolve("../build/release/exsim_research"), ["verify", join(output, "runner")], { encoding: "utf8" });
    expect(verify.status, verify.stderr).toBe(0);
    const shortConfig = structuredClone(config);
    shortConfig.seedSet.masterSeeds = ["901"];
    shortConfig.bounds.endSimulationTimeNs = "3000000000";
    shortConfig.markoutHorizonsNs = ["5000000000"];
    if (shortConfig.strategy.strategyKind !== "twap_execution") throw new Error("Expected TWAP fixture");
    shortConfig.strategy.startTimeNs = "1000000000";
    shortConfig.strategy.executionDurationNs = "1000000000";
    shortConfig.strategy.sliceIntervalNs = "100000000";
    const shortA = await call<ApiSchemas["Experiment"]>("/v1/experiments", { name: "Unobserved markout baseline", derivedFromBaselineId: baseline.baselineId, config: shortConfig });
    await launch(shortA.experimentId, ["901"]);
    shortConfig.strategy.limitOffsetTicks = "6";
    const shortB = await call<ApiSchemas["Experiment"]>("/v1/experiments", { name: "Unobserved markout candidate", derivedFromBaselineId: baseline.baselineId, config: shortConfig });
    await launch(shortB.experimentId, ["901"]);
    const unobserved = await call<ApiSchemas["ExperimentComparison"]>(`/v1/comparisons?baselineExperimentId=${shortA.experimentId}&candidateExperimentId=${shortB.experimentId}`);
    const markout = unobserved.metricComparisons.find((item) => item.metricKey === "markout_total")!;
    expect(markout.dispersion.sampleCount).toBe(0);
    expect(markout.dispersion.meanDifference).toBeNull();
    expect(markout.dispersion.tiedCount).toBe(0);
    await page.goto(`/compare?baseline=${shortA.experimentId}&candidate=${shortB.experimentId}`);
    const horizon = page.getByRole("row").filter({ has: page.getByRole("cell", { name: "Markout total @ 5 s", exact: false }) });
    await expect(horizon).toContainText("n/a");
  } finally { await api.stop(); rmSync(downloadDirectory, { recursive: true, force: true }); }
});

test("native MM diagnostics expose replacement failures and replacement-chain FIFO evidence", async ({ page }) => {
  const api = await isolatedApi();
  const call = async <T>(path: string, body?: unknown): Promise<T> => {
    const response = await fetch(`${api.baseUrl}${path}`, body === undefined ? {} : { method: "POST", headers: { "Content-Type": "application/json", "Idempotency-Key": `mm-persona-${Date.now()}-${Math.random().toString(36).slice(2)}` }, body: JSON.stringify(body) });
    expect(response.ok, response.ok ? "" : await response.clone().text()).toBe(true);
    return response.json();
  };
  try {
    const fixture = JSON.parse(readFileSync(resolve("../docs/reviews/PERSONA_MM_EVIDENCE_2026-11-08/baseline.json"), "utf8")) as ApiSchemas["Experiment"];
    const config = structuredClone(fixture.config);
    config.seedSet.masterSeeds = ["103"];
    const runStudy = async (name: string, config: ApiSchemas["ExperimentConfig"]) => {
      const experiment = await call<ApiSchemas["Experiment"]>("/v1/experiments", { name, derivedFromBaselineId: fixture.derivedFromBaselineId, config });
      const run = await call<ApiSchemas["Run"]>("/v1/runs", { experimentId: experiment.experimentId, masterSeed: "103", startPaused: false });
      await expect.poll(async () => (await call<ApiSchemas["Run"]>(`/v1/runs/${run.runId}`)).state).toBe("completed");
      return { experiment, run };
    };
    const baseline = await runStudy("Native MM baseline", config);
    const slow = structuredClone(config);
    slow.latency.cancelLatencyNs = "100000000";
    const candidate = await runStudy("Native MM delayed replacements", slow);
    await page.goto(`/runs/${candidate.run.runId}`);
    const diagnostics = page.getByRole("table", { name: "Outbound request diagnostics", exact: true });
    const replaces = diagnostics.getByRole("row").filter({ has: page.getByRole("cell", { name: "Replace", exact: true }) });
    await expect(replaces).toContainText("505");
    await expect(replaces).toContainText("unknown_order: 504");
    const newOrders = diagnostics.getByRole("row").filter({ has: page.getByRole("cell", { name: "New order", exact: true }) });
    await expect(newOrders).toContainText("3 /");
    await page.goto(`/runs/${candidate.run.runId}/inspect?order=7`);
    await expect(page.getByRole("heading", { name: "Replacement transit" })).toBeVisible();
    await expect(page.getByText(/Observed causal fill race/)).toBeVisible();
    await page.goto(`/runs/${baseline.run.runId}/inspect`);
    await page.getByRole("table", { name: "Recorded fills" }).locator("tbody tr").first().click();
    await expect(page.getByLabel("Selected fill identity")).toContainText("order 7, event 45");
    await expect(page.getByRole("heading", { name: "Order 12: latest chain state" })).toBeVisible();
    const fifo = page.getByRole("table", { name: "FIFO queue evidence", exact: true });
    await expect(fifo.getByRole("row").filter({ hasText: "#51" })).toContainText("12");
    const explanation = page.getByRole("region", { name: "Selected order explanation" });
    await expect(explanation.getByRole("button", { name: "Return to selected row", exact: true })).toBeVisible();
    await page.getByRole("button", { name: "Next fills", exact: true }).click();
    await expect(page.getByRole("navigation", { name: "Fills pages" })).toContainText("page 2");
    await expect(page.getByRole("status").filter({ hasText: /Selected fill/ })).not.toContainText("available on the selected row");
    await explanation.getByRole("button", { name: "Return to fills and own orders", exact: true }).click();
    await expect(page.getByLabel("Fills and own orders", { exact: true })).toBeFocused();
    await page.goto(`/compare?baseline=${baseline.experiment.experimentId}&candidate=${candidate.experiment.experimentId}`);
    await expect(page.getByRole("table", { name: "Comparison markout coverage" })).toContainText("135665");
    await page.getByText("Request and quote-update diagnostics by selected attempt", { exact: true }).click();
    await expect(page.getByRole("table", { name: "Outbound request diagnostics", exact: true }).last()).toContainText("unknown_order: 504");
  } finally { await api.stop(); }
});
