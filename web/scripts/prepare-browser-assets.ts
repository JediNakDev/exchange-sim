import { cpSync, existsSync, mkdirSync, rmSync } from "node:fs";

// Browser invariants exercise the production standalone server, not next dev.
if (!existsSync(".next/standalone/server.js")) throw new Error("Run bun run build before browser tests.");
mkdirSync(".next/standalone/.next", { recursive: true });
rmSync(".next/standalone/.next/static", { recursive: true, force: true });
cpSync(".next/static", ".next/standalone/.next/static", { recursive: true });
if (existsSync("public")) {
  rmSync(".next/standalone/public", { recursive: true, force: true });
  cpSync("public", ".next/standalone/public", { recursive: true });
}
