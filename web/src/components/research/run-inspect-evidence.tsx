import type { ApiSchemas } from "@/lib/api/exsim-api-client";
import { Button } from "@/components/ui/button";
import { ResearchSection } from "@/components/research/research-section";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { humanizeEnumValue } from "@/lib/research-labels";

export function LifecycleRepairEvidence({ lifecycle, onSelectEventPosition }: {
  lifecycle: ApiSchemas["OrderLifecycle"];
  onSelectEventPosition: (position: string) => void;
}) {
  const jump = (position: string, label: string) => <Button size="xs" variant="outline" onClick={() => onSelectEventPosition(position)}>{label} #{position}</Button>;
  return <>
    <ResearchSection title="Replacement transit" description="Cancel/replace channel, exact submit-to-arrival elapsed ns. A fill race is labeled only when a related fill occurred between those events. Unknown-order rejection alone does not establish a fill race.">
      <div className="min-w-0">{lifecycle.replaceTransits === undefined ? <p className="text-sm text-muted-foreground">Replacement transit evidence unavailable in this data.</p> : lifecycle.replaceTransits.length === 0 ? <p className="text-sm">No replacement transits observed.</p> : <div className="min-w-0 border bg-card [&>[data-slot=table-container]]:max-h-96 [&>[data-slot=table-container]]:overflow-auto"><Table aria-label="Replacement transits" className="text-xs">
        <TableHeader className="sticky top-0 z-1 bg-muted [&_th]:text-muted-foreground"><TableRow><TableHead>Submit</TableHead><TableHead>Arrival</TableHead><TableHead className="text-right">Transit (ns)</TableHead><TableHead>Fill before arrival</TableHead></TableRow></TableHeader>
        <TableBody>{lifecycle.replaceTransits.map((transit) => <TableRow key={transit.submitEventPosition}>
          <TableCell>{jump(transit.submitEventPosition, "Replace submit")}</TableCell>
          <TableCell>{jump(transit.arrivalEventPosition, "Replace arrival")}</TableCell>
          <TableCell className="text-right font-mono tabular-nums">{transit.transitNs}</TableCell>
          <TableCell>{transit.fillBeforeArrivalEventPosition === null ? "No related fill observed between submit and arrival" : <>{jump(transit.fillBeforeArrivalEventPosition, "Fill before replace arrival")}<span> Observed causal fill race</span></>}</TableCell>
        </TableRow>)}</TableBody>
      </Table></div>}</div>
    </ResearchSection>
    <ResearchSection title="FIFO queue ahead" description="Shares resting at the same price with earlier time priority. Exact simulated truth unless marked estimated. At a priority-losing replacement, queue position is unavailable until subsequent rest; the new queue can have nonzero shares ahead.">
      <div className="min-w-0">
        <p className="mb-2 text-xs text-muted-foreground">{lifecycle.fifoEvidenceTruncated === true ? "FIFO evidence truncated at 500 rows; later transitions are omitted." : lifecycle.fifoEvidenceTruncated === false ? "FIFO evidence is not truncated." : "FIFO truncation status unavailable in this data."}</p>
        <div className="min-w-0 border bg-card [&>[data-slot=table-container]]:max-h-96 [&>[data-slot=table-container]]:overflow-auto"><Table aria-label="FIFO queue evidence" className="text-xs">
          <TableHeader className="sticky top-0 z-1 bg-muted [&_th]:text-muted-foreground"><TableRow><TableHead>Event</TableHead><TableHead>Reason</TableHead><TableHead className="text-right">Exchange order</TableHead><TableHead className="text-right">Old exchange order</TableHead><TableHead className="text-right">Price</TableHead><TableHead>Priority</TableHead><TableHead>Queue availability</TableHead><TableHead className="text-right">Shares ahead</TableHead><TableHead className="text-right">Orders ahead</TableHead><TableHead className="text-right">Level total shares</TableHead><TableHead>Source</TableHead></TableRow></TableHeader>
          <TableBody>{lifecycle.fifoAheadEvidence.map((evidence) => <TableRow key={`${evidence.eventPosition}-${evidence.reason}`}>
            <TableCell>{jump(evidence.eventPosition, "FIFO event")}</TableCell>
            <TableCell>{humanizeEnumValue(evidence.reason)}</TableCell>
            <TableCell className="text-right font-mono tabular-nums">{evidence.orderId ?? "Unavailable"}</TableCell>
            <TableCell className="text-right font-mono tabular-nums">{evidence.replacedOldOrderId ?? (evidence.replacedOldOrderId === null ? "None" : "Unavailable")}</TableCell>
            <TableCell className="text-right font-mono tabular-nums">{evidence.price}</TableCell>
            <TableCell>{evidence.priorityKept === true ? "Kept" : evidence.priorityKept === false ? "Lost" : "Unavailable / not a replacement"}</TableCell>
            <TableCell>{evidence.queuePositionAvailable === false ? "Unavailable at replacement; see subsequent rest" : evidence.queuePositionAvailable === true ? "Available" : "Availability flag unavailable in this data"}</TableCell>
            <TableCell className="text-right font-mono tabular-nums">{evidence.queuePositionAvailable === false ? "Unavailable" : evidence.sharesAhead ?? "Unavailable"}</TableCell>
            <TableCell className="text-right font-mono tabular-nums">{evidence.queuePositionAvailable === false ? "Unavailable" : evidence.ordersAhead ?? "Unavailable"}</TableCell>
            <TableCell className="text-right font-mono tabular-nums">{evidence.queuePositionAvailable === false ? "Unavailable" : evidence.levelTotalShares ?? "Unavailable"}</TableCell>
            <TableCell>{evidence.source === "exact_simulated_truth" ? "exact" : "estimated"}</TableCell>
          </TableRow>)}{lifecycle.fifoAheadEvidence.length === 0 ? <TableRow><TableCell colSpan={11}>No FIFO queue evidence recorded.</TableCell></TableRow> : null}</TableBody>
        </Table></div>
      </div>
    </ResearchSection>
  </>;
}
