import { cpSync, existsSync, mkdirSync, rmSync } from "node:fs";

// Browser invariants exercise the production standalone server, not next dev.
// Native audit checks also execute the independent CLI artifact verifier.
for (const binary of ["exsim_api", "exsim_research"]) {
  if (!existsSync(`../build/release/${binary}`)) {
    throw new Error("Build the native browser-test tools first: make -C .. build/release/exsim_api build/release/exsim_research");
  }
}
if (!existsSync(".next/standalone/server.js")) throw new Error("Run bun run build before browser tests.");
mkdirSync(".next/standalone/.next", { recursive: true });
rmSync(".next/standalone/.next/static", { recursive: true, force: true });
cpSync(".next/static", ".next/standalone/.next/static", { recursive: true });
if (existsSync("public")) {
  rmSync(".next/standalone/public", { recursive: true, force: true });
  cpSync("public", ".next/standalone/public", { recursive: true });
}
