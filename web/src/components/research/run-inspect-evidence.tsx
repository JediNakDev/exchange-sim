import type { ApiSchemas } from "@/lib/api/exsim-api-client";
import { Button } from "@/components/ui/button";
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { humanizeEnumValue } from "@/lib/research-labels";

export function LifecycleRepairEvidence({ lifecycle, onSelectEventPosition }: {
  lifecycle: ApiSchemas["OrderLifecycle"];
  onSelectEventPosition: (position: string) => void;
}) {
  const jump = (position: string, label: string) => <Button size="xs" variant="outline" onClick={() => onSelectEventPosition(position)}>{label} #{position}</Button>;
  return <>
    <Card size="sm">
      <CardHeader><CardTitle>Replacement transit</CardTitle><CardDescription>Cancel/replace channel, exact submit-to-arrival elapsed ns. A fill race is labeled only when a related fill occurred between those events. Unknown-order rejection alone does not establish a fill race.</CardDescription></CardHeader>
      <CardContent>{lifecycle.replaceTransits === undefined ? <p className="text-sm text-muted-foreground">Replacement transit evidence unavailable in this data.</p> : lifecycle.replaceTransits.length === 0 ? <p className="text-sm">No replacement transits observed.</p> : <Table aria-label="Replacement transits" className="text-xs">
        <TableHeader><TableRow><TableHead>Submit</TableHead><TableHead>Arrival</TableHead><TableHead>Transit (ns)</TableHead><TableHead>Fill before arrival</TableHead></TableRow></TableHeader>
        <TableBody>{lifecycle.replaceTransits.map((transit) => <TableRow key={transit.submitEventPosition}>
          <TableCell>{jump(transit.submitEventPosition, "Replace submit")}</TableCell>
          <TableCell>{jump(transit.arrivalEventPosition, "Replace arrival")}</TableCell>
          <TableCell className="font-mono">{transit.transitNs}</TableCell>
          <TableCell>{transit.fillBeforeArrivalEventPosition === null ? "No related fill observed between submit and arrival" : <>{jump(transit.fillBeforeArrivalEventPosition, "Fill before replace arrival")}<span> Observed causal fill race</span></>}</TableCell>
        </TableRow>)}</TableBody>
      </Table>}</CardContent>
    </Card>
    <Card size="sm">
      <CardHeader><CardTitle>FIFO queue ahead</CardTitle><CardDescription>Shares resting at the same price with earlier time priority. Exact simulated truth unless marked estimated. At a priority-losing replacement, queue position is unavailable until subsequent rest; the new queue can have nonzero shares ahead.</CardDescription></CardHeader>
      <CardContent>
        <p className="mb-2 text-xs text-muted-foreground">{lifecycle.fifoEvidenceTruncated === true ? "FIFO evidence truncated at 500 rows; later transitions are omitted." : lifecycle.fifoEvidenceTruncated === false ? "FIFO evidence is not truncated." : "FIFO truncation status unavailable in this data."}</p>
        <Table aria-label="FIFO queue evidence" className="text-xs">
          <TableHeader><TableRow><TableHead>Event</TableHead><TableHead>Reason</TableHead><TableHead>Exchange order</TableHead><TableHead>Old exchange order</TableHead><TableHead>Price</TableHead><TableHead>Priority</TableHead><TableHead>Queue availability</TableHead><TableHead>Shares ahead</TableHead><TableHead>Orders ahead</TableHead><TableHead>Level total shares</TableHead><TableHead>Source</TableHead></TableRow></TableHeader>
          <TableBody>{lifecycle.fifoAheadEvidence.map((evidence) => <TableRow key={`${evidence.eventPosition}-${evidence.reason}`}>
            <TableCell>{jump(evidence.eventPosition, "FIFO event")}</TableCell>
            <TableCell>{humanizeEnumValue(evidence.reason)}</TableCell>
            <TableCell className="font-mono">{evidence.orderId ?? "Unavailable"}</TableCell>
            <TableCell className="font-mono">{evidence.replacedOldOrderId ?? (evidence.replacedOldOrderId === null ? "None" : "Unavailable")}</TableCell>
            <TableCell className="font-mono">{evidence.price}</TableCell>
            <TableCell>{evidence.priorityKept === true ? "Kept" : evidence.priorityKept === false ? "Lost" : "Unavailable / not a replacement"}</TableCell>
            <TableCell>{evidence.queuePositionAvailable === false ? "Unavailable at replacement; see subsequent rest" : evidence.queuePositionAvailable === true ? "Available" : "Availability flag unavailable in this data"}</TableCell>
            <TableCell className="font-mono">{evidence.queuePositionAvailable === false ? "Unavailable" : evidence.sharesAhead ?? "Unavailable"}</TableCell>
            <TableCell className="font-mono">{evidence.queuePositionAvailable === false ? "Unavailable" : evidence.ordersAhead ?? "Unavailable"}</TableCell>
            <TableCell className="font-mono">{evidence.queuePositionAvailable === false ? "Unavailable" : evidence.levelTotalShares ?? "Unavailable"}</TableCell>
            <TableCell>{evidence.source === "exact_simulated_truth" ? "exact" : "estimated"}</TableCell>
          </TableRow>)}{lifecycle.fifoAheadEvidence.length === 0 ? <TableRow><TableCell colSpan={11}>No FIFO queue evidence recorded.</TableCell></TableRow> : null}</TableBody>
        </Table>
      </CardContent>
    </Card>
  </>;
}
