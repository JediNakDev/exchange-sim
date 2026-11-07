import type { ApiSchemas } from "@/lib/api/exsim-api-client";
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";

export function RequestDiagnosticsPanel({ diagnostics }: { diagnostics: ApiSchemas["RequestDiagnostic"][] | undefined }) {
  return <Card size="sm">
    <CardHeader>
      <CardTitle>Outbound request diagnostics</CardTitle>
      <CardDescription>Submitted counts are outbound requests by kind. Exchange rejections use that kind’s submitted count as denominator. Pre-trade host risk blocks occur before submission and are separate risk evidence. Replacements use the cancel/replace channel. Arrival transit is exchange arrival minus submit, in ns; samples cover observed arrivals, including rejected requests.</CardDescription>
    </CardHeader>
    <CardContent>
      {diagnostics === undefined ? <p className="text-sm text-muted-foreground">Request diagnostics unavailable in this data.</p> : <Table aria-label="Outbound request diagnostics" className="text-xs">
        <TableHeader><TableRow><TableHead>Request</TableHead><TableHead>Submitted</TableHead><TableHead>Exchange rejected / submitted</TableHead><TableHead>Reject reasons (count)</TableHead><TableHead>Arrival samples</TableHead><TableHead>Min (ns)</TableHead><TableHead>Max (ns)</TableHead><TableHead>Mean (ns)</TableHead></TableRow></TableHeader>
        <TableBody>{(["new_order", "cancel", "replace"] as const).map((kind) => {
          const diagnostic = diagnostics.find((row) => row.requestKind === kind);
          const transit = diagnostic?.arrivalTransit;
          return <TableRow key={kind}>
            <TableCell>{kind === "new_order" ? "New order" : kind === "cancel" ? "Cancel" : "Replace"}</TableCell>
            <TableCell className="font-mono">{diagnostic?.submittedCount ?? "Unavailable"}</TableCell>
            <TableCell className="font-mono">{diagnostic ? `${diagnostic.exchangeRejectedCount} / ${diagnostic.submittedCount}` : "Unavailable"}</TableCell>
            <TableCell>{diagnostic ? diagnostic.rejectReasons.length ? diagnostic.rejectReasons.map(({ reason, count }) => `${reason}: ${count}`).join("; ") : "None reported" : "Unavailable"}</TableCell>
            <TableCell className="font-mono">{transit?.sampleCount ?? "Unavailable"}</TableCell>
            <TableCell className="font-mono">{transit?.minNs ?? "Unavailable"}</TableCell>
            <TableCell className="font-mono">{transit?.maxNs ?? "Unavailable"}</TableCell>
            <TableCell className="font-mono">{transit?.meanNs ?? "Unavailable"}</TableCell>
          </TableRow>;
        })}</TableBody>
      </Table>}
    </CardContent>
  </Card>;
}
