import { expect, test } from "bun:test";
import { renderToStaticMarkup } from "react-dom/server";
import { WorkspaceBreadcrumb } from "../../src/components/research/app-navigation";

test("inspection ancestors navigate back to the same run and pinned cohort", () => {
  const html = renderToStaticMarkup(<WorkspaceBreadcrumb currentPathname="/runs/run_123/inspect" contextQuery="?baseline=exp_a&candidate=exp_b&runSelection=opaque" />);
  expect(html).toContain('href="/research"');
  expect(html).toContain('href="/runs"');
  expect(html).toContain('href="/runs/run_123?baseline=exp_a&amp;candidate=exp_b&amp;runSelection=opaque"');
  expect(html).toContain('aria-current="page"');
  expect(html).toContain('aria-disabled="true"');
  expect(html).not.toContain('href="/runs/run_123/inspect"');
});

test("variant breadcrumb links its immutable source and root has no self link", () => {
  const variant = renderToStaticMarkup(<WorkspaceBreadcrumb currentPathname="/experiments/new" sourceId="exp_source" />);
  expect(variant).toContain('href="/experiments/exp_source"');
  expect(variant).toContain("New variant");
  expect(renderToStaticMarkup(<WorkspaceBreadcrumb currentPathname="/research" />)).not.toContain("href=");
});
