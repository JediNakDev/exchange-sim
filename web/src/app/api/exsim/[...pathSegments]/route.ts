// Same-origin proxy from the browser to the research service at
// EXSIM_API_BASE_URL. It forwards only /v1 GET and POST requests and the headers
// the contract defines; it never interprets or rewrites response bodies.

import type { ApiSchemas } from "@/lib/api/exsim-api-client";
import { readExsimApiBaseUrl } from "@/lib/api/exsim-api-client";

const FORWARDED_REQUEST_HEADERS = ["content-type", "idempotency-key", "exsim-schema-version", "accept"];
const FORWARDED_RESPONSE_HEADERS = ["content-type", "exsim-schema-version", "idempotent-replayed"];

interface ProxyRouteContext {
  params: Promise<{ pathSegments: string[] }>;
}

function serviceUnavailableResponse(message: string): Response {
  const body: ApiSchemas["ErrorResponseBody"] = {
    error: { code: "service_unavailable", message, requestId: "proxy", retryable: true, fieldIssues: [] },
  };
  return Response.json(body, { status: 503 });
}

async function forwardToResearchService(request: Request, context: ProxyRouteContext): Promise<Response> {
  const { pathSegments } = await context.params;
  if (pathSegments[0] !== "v1" || pathSegments.some((segment) => segment === ".." || segment === "")) {
    const body: ApiSchemas["ErrorResponseBody"] = {
      error: { code: "not_found", message: "only /v1 paths are proxied", requestId: "proxy", retryable: false, fieldIssues: [] },
    };
    return Response.json(body, { status: 404 });
  }
  const incomingUrl = new URL(request.url);
  const upstreamUrl = `${readExsimApiBaseUrl().replace(/\/$/, "")}/${pathSegments.map(encodeURIComponent).join("/")}${incomingUrl.search}`;
  const upstreamHeaders = new Headers();
  for (const headerName of FORWARDED_REQUEST_HEADERS) {
    const headerValue = request.headers.get(headerName);
    if (headerValue !== null) upstreamHeaders.set(headerName, headerValue);
  }
  let upstreamResponse: Response;
  try {
    upstreamResponse = await fetch(upstreamUrl, {
      method: request.method,
      headers: upstreamHeaders,
      body: request.method === "POST" ? await request.text() : undefined,
      cache: "no-store",
    });
  } catch (caught) {
    return serviceUnavailableResponse(`research service at ${readExsimApiBaseUrl()} is unreachable: ${caught instanceof Error ? caught.message : "unknown error"}`);
  }
  const responseHeaders = new Headers({ "cache-control": "no-store" });
  for (const headerName of FORWARDED_RESPONSE_HEADERS) {
    const headerValue = upstreamResponse.headers.get(headerName);
    if (headerValue !== null) responseHeaders.set(headerName, headerValue);
  }
  return new Response(await upstreamResponse.text(), { status: upstreamResponse.status, headers: responseHeaders });
}

export async function GET(request: Request, context: ProxyRouteContext): Promise<Response> {
  return forwardToResearchService(request, context);
}

export async function POST(request: Request, context: ProxyRouteContext): Promise<Response> {
  return forwardToResearchService(request, context);
}
