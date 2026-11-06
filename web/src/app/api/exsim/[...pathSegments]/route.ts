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

function errorResponse(status: number, code: ApiSchemas["ErrorCode"], message: string): Response {
  const body: ApiSchemas["ErrorResponseBody"] = {
    error: { code, message, requestId: "proxy", retryable: status >= 500, fieldIssues: [] },
  };
  return Response.json(body, { status, headers: { "cache-control": "no-store", "exsim-schema-version": "exsim.api/1" } });
}

async function forwardToResearchService(request: Request, context: ProxyRouteContext): Promise<Response> {
  if (request.method === "POST") {
    // JSON-only mutations cannot be issued by cross-site HTML forms. The origin
    // uses Host because a TLS-terminating private proxy may change request.url.
    const origin = request.headers.get("origin");
    const host = request.headers.get("host") ?? new URL(request.url).host;
    if (request.headers.get("sec-fetch-site") === "cross-site" || (origin !== null && !sameOriginHost(origin, host))) {
      return errorResponse(403, "invalid_request", "Run changes must be made from this research workspace.");
    }
    if (request.headers.get("content-type")?.split(";")[0].trim().toLowerCase() !== "application/json") {
      return errorResponse(415, "invalid_request", "Send requests as application/json.");
    }
  }
  const { pathSegments } = await context.params;
  if (pathSegments[0] !== "v1" || pathSegments.some((segment) => segment === ".." || segment === "")) {
    return errorResponse(404, "not_found", "Only /v1 paths are proxied.");
  }
  const incomingUrl = new URL(request.url);
  const upstreamUrl = `${readExsimApiBaseUrl().replace(/\/$/, "")}/${pathSegments.map(encodeURIComponent).join("/")}${incomingUrl.search}`;
  const upstreamHeaders = new Headers();
  for (const headerName of FORWARDED_REQUEST_HEADERS) {
    const headerValue = request.headers.get(headerName);
    if (headerValue !== null) upstreamHeaders.set(headerName, headerValue);
  }
  try {
    let body: string | undefined;
    if (request.method === "POST") {
      const reader = request.body?.getReader();
      const decoder = new TextDecoder();
      let bytesRead = 0;
      body = "";
      if (reader) {
        try {
          for (;;) {
            const chunk = await reader.read();
            if (chunk.done) break;
            bytesRead += chunk.value.byteLength;
            if (bytesRead > 1024 * 1024) {
              await reader.cancel();
              return errorResponse(413, "invalid_request", "The request exceeds the 1 MiB limit.");
            }
            body += decoder.decode(chunk.value, { stream: true });
          }
          body += decoder.decode();
        } finally {
          reader.releaseLock();
        }
      }
    }
    const upstreamResponse = await fetch(upstreamUrl, {
      method: request.method,
      headers: upstreamHeaders,
      body,
      cache: "no-store",
      signal: AbortSignal.any([request.signal, AbortSignal.timeout(120_000)]),
    });
    const responseHeaders = new Headers({ "cache-control": "no-store" });
    for (const headerName of FORWARDED_RESPONSE_HEADERS) {
      const headerValue = upstreamResponse.headers.get(headerName);
      if (headerValue !== null) responseHeaders.set(headerName, headerValue);
    }
    // Body reads can fail too, even after a successful response header.
    return new Response(await upstreamResponse.text(), { status: upstreamResponse.status, headers: responseHeaders });
  } catch {
    return errorResponse(503, "service_unavailable", "The research service did not respond. Retry the same action to check its outcome.");
  }
}

function sameOriginHost(origin: string, host: string): boolean {
  try {
    const parsed = new URL(origin);
    return ["http:", "https:"].includes(parsed.protocol) && parsed.host === host;
  } catch {
    return false;
  }
}

export async function GET(request: Request, context: ProxyRouteContext): Promise<Response> {
  return forwardToResearchService(request, context);
}

export async function POST(request: Request, context: ProxyRouteContext): Promise<Response> {
  return forwardToResearchService(request, context);
}
