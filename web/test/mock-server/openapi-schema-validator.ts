// Validates JSON values against component schemas of config/api/openapi.yaml.
// Used by the mock service for request validation and by tests to check that
// every mock response conforms to the contract.

import Ajv2020, { type ErrorObject, type ValidateFunction } from "ajv/dist/2020.js";
import addFormats from "ajv-formats";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { parse as parseYaml } from "yaml";

export const OPENAPI_DOCUMENT_PATH = join(dirname(fileURLToPath(import.meta.url)), "..", "..", "..", "config", "api", "openapi.yaml");
const OPENAPI_SCHEMA_ID = "exsim-openapi";

export interface SchemaValidationIssue {
  fieldPath: string;
  issue: string;
}

let sharedAjvInstance: Ajv2020 | null = null;
const compiledValidatorsBySchemaName = new Map<string, ValidateFunction>();

export function loadOpenApiDocument(): Record<string, unknown> {
  return parseYaml(readFileSync(OPENAPI_DOCUMENT_PATH, "utf8")) as Record<string, unknown>;
}

/** Ajv selects oneOf branches by the discriminator's const values and rejects the redundant OpenAPI `mapping`. */
function removeDiscriminatorMappings(node: unknown): Record<string, unknown> {
  if (Array.isArray(node)) return node.map(removeDiscriminatorMappings) as unknown as Record<string, unknown>;
  if (node === null || typeof node !== "object") return node as Record<string, unknown>;
  const copy: Record<string, unknown> = {};
  for (const [key, child] of Object.entries(node)) {
    if (key === "discriminator" && child && typeof child === "object") {
      copy[key] = { propertyName: (child as { propertyName: string }).propertyName };
    } else copy[key] = removeDiscriminatorMappings(child);
  }
  return copy;
}

function getAjvInstance(): Ajv2020 {
  if (!sharedAjvInstance) {
    sharedAjvInstance = new Ajv2020({ strict: false, allErrors: true, validateFormats: true, discriminator: true });
    addFormats(sharedAjvInstance);
    sharedAjvInstance.addSchema(removeDiscriminatorMappings(loadOpenApiDocument()), OPENAPI_SCHEMA_ID);
  }
  return sharedAjvInstance;
}

function describeIssue(error: ErrorObject): SchemaValidationIssue {
  const missingProperty = error.keyword === "required" ? `/${(error.params as { missingProperty: string }).missingProperty}` : "";
  const extraProperty = error.keyword === "additionalProperties" ? `/${(error.params as { additionalProperty: string }).additionalProperty}` : "";
  return { fieldPath: `${error.instancePath}${missingProperty}${extraProperty}` || "/", issue: error.message ?? error.keyword };
}

/** Returns an empty list when `value` matches `#/components/schemas/<componentSchemaName>`. */
export function validateAgainstComponentSchema(componentSchemaName: string, value: unknown): SchemaValidationIssue[] {
  let validator = compiledValidatorsBySchemaName.get(componentSchemaName);
  if (!validator) {
    validator = getAjvInstance().compile({ $ref: `${OPENAPI_SCHEMA_ID}#/components/schemas/${componentSchemaName}` });
    compiledValidatorsBySchemaName.set(componentSchemaName, validator);
  }
  if (validator(value)) return [];
  const issues = (validator.errors ?? []).map(describeIssue);
  // Keep the list short and unique.
  return [...new Map(issues.map((issue) => [`${issue.fieldPath}|${issue.issue}`, issue])).values()].slice(0, 20);
}

interface OpenApiResponseObject {
  $ref?: string;
  content?: Record<string, { schema?: { $ref?: string } }>;
}

let cachedOpenApiDocument: Record<string, unknown> | null = null;

/**
 * Validates a response body against the schema the contract declares for that
 * operation and status (falling back to `default`). Returns the component
 * schema name used so callers can assert which branch applied.
 */
export function validateOperationResponse(pathTemplate: string, httpMethod: "get" | "post", httpStatus: number, body: unknown): { componentSchemaName: string; issues: SchemaValidationIssue[] } {
  cachedOpenApiDocument ??= loadOpenApiDocument();
  const pathItem = (cachedOpenApiDocument.paths as Record<string, Record<string, { responses: Record<string, OpenApiResponseObject> }>>)[pathTemplate];
  const operation = pathItem?.[httpMethod];
  if (!operation) throw new Error(`the contract has no ${httpMethod.toUpperCase()} ${pathTemplate}`);
  let responseObject = operation.responses[String(httpStatus)] ?? operation.responses.default;
  if (responseObject?.$ref) {
    const sharedResponses = (cachedOpenApiDocument.components as { responses: Record<string, OpenApiResponseObject> }).responses;
    responseObject = sharedResponses[responseObject.$ref.replace("#/components/responses/", "")];
  }
  const schemaReference = responseObject?.content?.["application/json"]?.schema?.$ref;
  if (!schemaReference) throw new Error(`${httpMethod.toUpperCase()} ${pathTemplate} declares no JSON schema for status ${httpStatus}`);
  const componentSchemaName = schemaReference.replace("#/components/schemas/", "");
  return { componentSchemaName, issues: validateAgainstComponentSchema(componentSchemaName, body) };
}
