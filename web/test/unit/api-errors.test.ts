import { expect, test } from "bun:test";
import { describeApiFailure, unwrapApiResult, type ApiSchemas } from "../../src/lib/api/exsim-api-client";

test("malformed upstream errors cannot crash the failure view", () => {
  for (const error of [{ error: null }, { error: [] }, { error: { code: "bad", message: "bad", requestId: "bad", retryable: true, fieldIssues: [null] } }, "<html>gateway failure</html>"]) {
    let caught: unknown;
    try { unwrapApiResult({ response: new Response("", { status: 503 }), error }); } catch (failure) { caught = failure; }
    const failure = describeApiFailure(caught);
    expect(failure.code).toBe("service_unavailable");
    expect(failure.fieldIssues).toEqual([]);
    expect(failure.retryable).toBe(true);
  }
});

test("valid structured error context survives", () => {
  const error: ApiSchemas["ApiError"] = { code: "validation_failed", message: "Quantity must be positive", requestId: "request-12", retryable: false, fieldIssues: [{ fieldPath: "/quantity", issue: "positive required" }] };
  let caught: unknown;
  try { unwrapApiResult({ response: new Response("", { status: 422 }), error: { error } }); } catch (failure) { caught = failure; }
  expect(describeApiFailure(caught)).toEqual(error);
});
