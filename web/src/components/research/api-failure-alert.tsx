import { CircleAlertIcon } from "lucide-react";
import { Alert, AlertDescription, AlertTitle } from "@/components/ui/alert";
import type { ApiSchemas } from "@/lib/api/exsim-api-client";

export function ApiFailureAlert({ failure, title }: { failure: ApiSchemas["ApiError"]; title?: string }) {
  return (
    <Alert variant="destructive">
      <CircleAlertIcon />
      <AlertTitle>{title ?? "Request failed"}</AlertTitle>
      <AlertDescription>
        <p>
          <code className="font-mono">{failure.code}</code>: {failure.message}
          {failure.retryable ? " (retryable)" : ""}
        </p>
        {failure.fieldIssues.length > 0 ? (
          <ul className="list-disc pl-4">
            {failure.fieldIssues.map((fieldIssue) => (
              <li key={`${fieldIssue.fieldPath}:${fieldIssue.issue}`}>
                <code className="font-mono">{fieldIssue.fieldPath}</code> {fieldIssue.issue}
              </li>
            ))}
          </ul>
        ) : null}
        <p className="text-xs opacity-70">request {failure.requestId}</p>
      </AlertDescription>
    </Alert>
  );
}
