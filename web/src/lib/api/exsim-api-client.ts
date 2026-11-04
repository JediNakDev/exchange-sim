// Typed clients for docs/api/openapi.yaml. Types come from generated-schema.d.ts
// (run `bun run generate-api-types` after editing the contract).
//
// The browser talks to the same-origin proxy at /api/exsim, which forwards to
// EXSIM_API_BASE_URL. Server code and tests can build a client that points at the
// research service directly.

import createClient, { type Client } from "openapi-fetch";
import type { components, paths } from "./generated-schema";

export type ApiSchemas = components["schemas"];
export type ExsimApiClient = Client<paths>;

export const EXSIM_API_SCHEMA_VERSION = "exsim.api/1";
export const EXSIM_BROWSER_PROXY_BASE_PATH = "/api/exsim";
export const DEFAULT_EXSIM_API_BASE_URL = "http://127.0.0.1:7600";

export function readExsimApiBaseUrl(): string {
  return process.env.EXSIM_API_BASE_URL ?? DEFAULT_EXSIM_API_BASE_URL;
}

export function createExsimApiClient(baseUrl: string, customFetch?: (request: Request) => Promise<Response>): ExsimApiClient {
  return createClient<paths>({
    baseUrl,
    headers: { "Exsim-Schema-Version": EXSIM_API_SCHEMA_VERSION },
    // The contract's array query parameters are comma separated (style form, explode false).
    querySerializer: { array: { style: "form", explode: false } },
    ...(customFetch ? { fetch: customFetch } : {}),
  });
}

let browserApiClient: ExsimApiClient | null = null;

/** Client used by client components; relative URLs resolve against the page origin. */
export function getBrowserApiClient(): ExsimApiClient {
  browserApiClient ??= createExsimApiClient(EXSIM_BROWSER_PROXY_BASE_PATH);
  return browserApiClient;
}

export class ExsimApiRequestError extends Error {
  constructor(
    readonly httpStatus: number,
    readonly apiError: ApiSchemas["ApiError"],
  ) {
    super(`${apiError.code}: ${apiError.message}`);
  }
}

function isErrorResponseBody(value: unknown): value is ApiSchemas["ErrorResponseBody"] {
  return typeof value === "object" && value !== null && "error" in value && typeof (value as { error: unknown }).error === "object";
}

/** Returns `data` or throws ExsimApiRequestError carrying the contract's ApiError. */
export function unwrapApiResult<ResponseData>(result: { data?: ResponseData; error?: unknown; response: Response }): ResponseData {
  if (result.data !== undefined && result.response.ok) return result.data;
  if (isErrorResponseBody(result.error)) throw new ExsimApiRequestError(result.response.status, result.error.error);
  throw new ExsimApiRequestError(result.response.status, {
    code: result.response.status === 503 ? "service_unavailable" : "internal_error",
    message: `unexpected response with HTTP status ${result.response.status}`,
    requestId: "unknown",
    retryable: result.response.status >= 500,
    fieldIssues: [],
  });
}

export function createIdempotencyKey(purpose: string): string {
  return `${purpose}-${crypto.randomUUID().replaceAll("-", "")}`.slice(0, 128);
}

export function describeApiFailure(caught: unknown): ApiSchemas["ApiError"] {
  if (caught instanceof ExsimApiRequestError) return caught.apiError;
  return {
    code: "service_unavailable",
    message: caught instanceof Error ? caught.message : "request failed",
    requestId: "client",
    retryable: true,
    fieldIssues: [],
  };
}
