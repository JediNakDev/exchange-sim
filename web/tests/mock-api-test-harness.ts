// Drives the mock service in-process through the same openapi-fetch client the
// UI uses, with a controllable wall clock, and checks every response body
// against the schema the contract declares for that operation and status.

import { expect } from "bun:test";
import { MockResearchService } from "../mock-server/mock-research-service";
import { validateOperationResponse } from "../mock-server/openapi-schema-validator";
import { createExsimApiClient, type ApiSchemas, type ExsimApiClient } from "../src/lib/api/exsim-api-client";

export interface MockApiHarness {
  apiClient: ExsimApiClient;
  advanceWallClockMs: (milliseconds: number) => void;
  /** Every (method, path template, status) that responded, for coverage checks. */
  observedOperations: Set<string>;
}

export function createMockApiHarness(): MockApiHarness {
  let wallClockMs = Date.parse("2026-11-03T12:00:00.000Z");
  const service = new MockResearchService(() => wallClockMs);
  const observedOperations = new Set<string>();
  const apiClient = createExsimApiClient("http://mock.exsim.test", (request) => service.handleRequest(request));
  return {
    apiClient,
    advanceWallClockMs: (milliseconds) => {
      wallClockMs += milliseconds;
    },
    observedOperations,
  };
}

/** Asserts the response matches the contract and returns the parsed body. */
export function expectContractResponse<ResponseBody>(
  harness: MockApiHarness,
  result: { data?: ResponseBody; error?: unknown; response: Response },
  httpMethod: "get" | "post",
  pathTemplate: string,
  expectedStatus: number,
): ResponseBody {
  const body = result.data ?? result.error;
  expect({ status: result.response.status, body }).toMatchObject({ status: expectedStatus });
  expect(result.response.headers.get("Exsim-Schema-Version")).toBe("exsim.api/1");
  const { issues } = validateOperationResponse(pathTemplate, httpMethod, result.response.status, body);
  expect(issues).toEqual([]);
  harness.observedOperations.add(`${httpMethod} ${pathTemplate}`);
  return body as ResponseBody;
}

/** Asserts an error response matches the contract and returns its `error` object. */
export function expectContractError(
  harness: MockApiHarness,
  result: { error?: unknown; response: Response },
  httpMethod: "get" | "post",
  pathTemplate: string,
  expectedStatus: number,
): ApiSchemas["ApiError"] {
  expect(expectedStatus).toBeGreaterThanOrEqual(400);
  return expectContractResponse<ApiSchemas["ErrorResponseBody"]>(harness, { error: result.error, response: result.response }, httpMethod, pathTemplate, expectedStatus).error;
}
