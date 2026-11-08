import type { ApiSchemas } from "@/lib/api/exsim-api-client";
import { ResearchSection } from "@/components/research/research-section";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";

export function RequestDiagnosticsPanel({ diagnostics }: { diagnostics: ApiSchemas["RequestDiagnostic"][] | undefined }) {
  return <ResearchSection title="Outbound request diagnostics" description={<> Submitted counts are outbound requests by kind. Exchange rejections use that kind’s submitted count as denominator. Pre-trade host risk blocks occur before submission and are separate risk evidence. Replacements use the cancel/replace channel. Arrival transit is exchange arrival minus submit, in ns; samples cover observed arrivals, including rejected requests. </>}>
<div>
      {diagnostics === undefined ? <p className="text-sm text-muted-foreground">Request diagnostics unavailable in this data.</p> : <div className="min-w-0 overflow-x-auto border bg-card"><Table aria-label="Outbound request diagnostics" className="text-xs">
        <TableHeader><TableRow><TableHead>Request</TableHead><TableHead className="text-right">Submitted</TableHead><TableHead className="text-right">Exchange rejected / submitted</TableHead><TableHead>Reject reasons (count)</TableHead><TableHead className="text-right">Arrival samples</TableHead><TableHead className="text-right">Min (ns)</TableHead><TableHead className="text-right">Max (ns)</TableHead><TableHead className="text-right">Mean (ns)</TableHead></TableRow></TableHeader>
        <TableBody>{(["new_order", "cancel", "replace"] as const).map((kind) => {
          const diagnostic = diagnostics.find((row) => row.requestKind === kind);
          const transit = diagnostic?.arrivalTransit;
          return <TableRow key={kind}>
            <TableCell>{kind === "new_order" ? "New order" : kind === "cancel" ? "Cancel" : "Replace"}</TableCell>
            <TableCell className="text-right font-mono tabular-nums">{diagnostic?.submittedCount ?? "Unavailable"}</TableCell>
            <TableCell className="text-right font-mono tabular-nums">{diagnostic ? `${diagnostic.exchangeRejectedCount} / ${diagnostic.submittedCount}` : "Unavailable"}</TableCell>
            <TableCell>{diagnostic ? diagnostic.rejectReasons.length ? diagnostic.rejectReasons.map(({ reason, count }) => `${reason}: ${count}`).join("; ") : "None reported" : "Unavailable"}</TableCell>
            <TableCell className="text-right font-mono tabular-nums">{transit?.sampleCount ?? "Unavailable"}</TableCell>
            <TableCell className="text-right font-mono tabular-nums">{transit?.minNs ?? "Unavailable"}</TableCell>
            <TableCell className="text-right font-mono tabular-nums">{transit?.maxNs ?? "Unavailable"}</TableCell>
            <TableCell className="text-right font-mono tabular-nums">{transit?.meanNs ?? "Unavailable"}</TableCell>
          </TableRow>;
        })}</TableBody>
      </Table></div>}
    </div>
  </ResearchSection>;
}
