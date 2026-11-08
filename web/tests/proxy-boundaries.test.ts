import { afterEach, expect, test } from "bun:test";
import { GET, POST } from "../src/app/api/exsim/[...pathSegments]/route";

const originalFetch = globalThis.fetch;
afterEach(() => { globalThis.fetch = originalFetch; });
const context = { params: Promise.resolve({ pathSegments: ["v1", "runs"] }) };
const post = (body = "{}", headers: Record<string, string> = {}) => new Request("http://localhost:3762/api/exsim/v1/runs", {
  method: "POST", body, headers: { "content-type": "application/json", ...headers },
});
function stubFetch(implementation: (input: string | URL | Request, init?: RequestInit) => Promise<Response>) {
  globalThis.fetch = implementation as typeof fetch;
}

test("cross-site form and opaque-origin mutations never reach the service", async () => {
  stubFetch(async () => { throw new Error("upstream must not be called"); });
  const cases: Record<string, string>[] = [{ origin: "https://other.test" }, { origin: "null" }, { "sec-fetch-site": "cross-site" }];
  for (const headers of cases) {
    const response = await POST(post("{}", headers), context);
    expect(response.status).toBe(403);
    expect((await response.json()).error.retryable).toBe(false);
  }
  expect((await POST(post("reason=cancel", { "content-type": "application/x-www-form-urlencoded" }), context)).status).toBe(415);
});

test("bounded JSON forwards exact bodies and idempotency headers behind TLS termination", async () => {
  let count = 0;
  const body = '{"masterSeed":"18446744073709551615"}';
  stubFetch(async (_input, init) => {
    count++;
    expect(init?.body).toBe(body);
    expect(new Headers(init?.headers).get("idempotency-key")).toBe("retry-same-key");
    expect(init?.signal).toBeInstanceOf(AbortSignal);
    return Response.json({ ok: true }, { status: 201, headers: { "idempotent-replayed": "true" } });
  });
  const response = await POST(post(body, { host: "research.example.ts.net:10000", origin: "https://research.example.ts.net:10000", "idempotency-key": "retry-same-key" }), context);
  expect(count).toBe(1);
  expect(response.status).toBe(201);
  expect(response.headers.get("idempotent-replayed")).toBe("true");
  expect(response.headers.get("cache-control")).toBe("no-store");
});

test("body limit applies to actual streamed bytes without trusting Content-Length", async () => {
  let called = false;
  stubFetch(async () => { called = true; return Response.json({}); });
  const response = await POST(post("é".repeat(524289)), context);
  expect(response.status).toBe(413);
  expect(called).toBe(false);
  expect(response.headers.get("exsim-schema-version")).toBe("exsim.api/1");
});

test("a broken upstream body returns structured failure instead of a Next error page", async () => {
  stubFetch(async () => new Response(new ReadableStream({ start(controller) { controller.error(new Error("private-path-and-address")); } })));
  const response = await GET(new Request("http://localhost/api/exsim/v1/runs"), context);
  expect(response.status).toBe(503);
  const body = await response.json();
  expect(body.error).toMatchObject({ code: "service_unavailable", retryable: true, fieldIssues: [] });
  expect(body.error.message).not.toContain("private-path-and-address");
});

test("non-v1 paths are rejected before network access", async () => {
  const response = await GET(new Request("http://localhost/api/exsim/admin"), { params: Promise.resolve({ pathSegments: ["admin"] }) });
  expect(response.status).toBe(404);
});
