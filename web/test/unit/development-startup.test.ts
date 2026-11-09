import { expect, test } from "bun:test";
import { mkdtempSync, writeFileSync, existsSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

test("an occupied API port cannot start the frontend against an older service", async () => {
  const oldApi = Bun.serve({ hostname: "127.0.0.1", port: 0, fetch: () => Response.json({ apiVersion: "v1" }) });
  const directory = mkdtempSync(join(tmpdir(), "exsim-dev-startup-"));
  const marker = join(directory, "frontend-started");
  writeFileSync(join(directory, "bunx"), '#!/bin/sh\nprintf started > "$EXSIM_TEST_NEXT_STARTED"\nexit 0\n', { mode: 0o755 });
  const child = Bun.spawn([process.execPath, "scripts/run-development-servers.ts", "--mock"], {
    cwd: resolve(import.meta.dir, "../.."),
    env: { ...process.env, PATH: `${directory}:${process.env.PATH}`, EXSIM_API_PORT: String(oldApi.port), EXSIM_TEST_NEXT_STARTED: marker },
    stdout: "pipe", stderr: "pipe",
  });
  const timeout = setTimeout(() => child.kill(), 8000);
  try {
    const [exit, error] = await Promise.all([child.exited, new Response(child.stderr).text(), new Response(child.stdout).text()]);
    expect(exit).not.toBe(0);
    expect(error).toContain("frontend was not started");
    expect(existsSync(marker)).toBe(false);
  } finally {
    clearTimeout(timeout);
    child.kill();
    oldApi.stop(true);
    rmSync(directory, { recursive: true, force: true });
  }
}, 10_000);
