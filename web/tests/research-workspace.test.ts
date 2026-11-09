import { expect, test } from "bun:test";
import type { ApiSchemas } from "../src/lib/api/exsim-api-client";
import { configurationChanges, researchRoot, researchStudies } from "../src/lib/research-workspace";
import { MockResearchService } from "../mock-server/mock-research-service";

async function example() {
  const service = new MockResearchService();
  const page = await (await service.handleRequest(new Request("http://fixture/v1/experiments"))).json();
  return page.items[0] as ApiSchemas["Experiment"];
}

test("nested variants stay in one study; missing parents remain accessible", async () => {
  const fixture = await example();
  const root = { ...fixture, experimentId: "root", parentExperimentId: null };
  const child = { ...fixture, experimentId: "child", parentExperimentId: "root" };
  const grandchild = { ...fixture, experimentId: "grandchild", parentExperimentId: "child" };
  expect(researchRoot(grandchild, [root, child, grandchild])).toBe("root");
  expect(researchStudies([grandchild, child, root])).toHaveLength(1);
  expect(researchStudies([grandchild, child, root])[0].configurations).toHaveLength(3);
  expect(researchRoot(child, [child])).toBe("child");
});

test("corrupt cyclic lineage terminates and groups descendants consistently", async () => {
  const fixture = await example();
  const a = { ...fixture, experimentId: "a", parentExperimentId: "b" };
  const b = { ...fixture, experimentId: "b", parentExperimentId: "a" };
  const descendant = { ...fixture, experimentId: "0-descendant", parentExperimentId: "b" };
  const all = [descendant, b, a];
  expect(researchRoot(descendant, all)).toBe(researchRoot(a, all));
  expect(researchStudies(all)).toHaveLength(1);
});

test("configuration changes retain exact integers, arrays and added strategy fields", () => {
  const before = { strategy: { start: "9007199254740992", kind: "fixed" }, seeds: ["1", "2"] };
  const after = { strategy: { start: "9007199254740993", kind: "skew", skew: "1" }, seeds: ["1", "3"] };
  const changes = configurationChanges(before, after);
  expect(changes.find((c) => c.path === "strategy.start")).toEqual({ path: "strategy.start", before: "9007199254740992", after: "9007199254740993" });
  expect(changes.find((c) => c.path === "strategy.skew")?.before).toBe("Not applicable");
  expect(changes.find((c) => c.path === "seeds")?.after).toBe('["1","3"]');
  expect(configurationChanges(after, structuredClone(after))).toEqual([]);
});
