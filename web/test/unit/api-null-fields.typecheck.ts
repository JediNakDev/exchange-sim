// Compile-time integration regression: openapi-fetch's Readable mapper must keep
// required null-only evidence fields. NonNullable<null> is never, not writeOnly.
import type { ApiSchemas, ExsimApiClient } from "../../src/lib/api/exsim-api-client";

export async function checkNullOnlyResponseField(client: ExsimApiClient, runId: string) {
  const response = await client.GET("/v1/runs/{runId}/summary", { params: { path: { runId } } });
  if (response.data) {
    const summary: ApiSchemas["RunSummary"] = response.data;
    const opportunityCost: null | undefined = summary.execution?.unexecutedOpportunityCost;
    return opportunityCost;
  }
}
