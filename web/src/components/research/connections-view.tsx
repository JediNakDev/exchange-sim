"use client";

// Read-only protocol telemetry.

import Link from "next/link";
import { Badge } from "@/components/ui/badge";
import { ResearchSection } from "@/components/research/research-section";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { ApiFailureAlert } from "@/components/research/api-failure-alert";
import { PageHeading } from "@/components/research/page-heading";
import { useApiResource } from "@/hooks/use-api-resource";
import { getBrowserApiClient, unwrapApiResult, type ApiSchemas } from "@/lib/api/exsim-api-client";
import { formatIntegerText, formatWallClockTime } from "@/lib/exact-decimal-format";
import { humanizeEnumValue } from "@/lib/research-labels";

const CONNECTIONS_POLL_INTERVAL_MS = 2000;

const SESSION_STATE_DOT: Record<ApiSchemas["ProtocolSessionStatus"]["state"], string> = {
  logged_in: "bg-positive", connecting: "bg-warning", recovering: "bg-warning",
  logging_out: "bg-warning", disconnected: "bg-negative", failed: "bg-negative",
};

export function ConnectionsView() {
  const apiClient = getBrowserApiClient();
  const connections = useApiResource("connections", async () => unwrapApiResult(await apiClient.GET("/v1/connections")), CONNECTIONS_POLL_INTERVAL_MS);
  const status = connections.data;
  return (
    <>
      <PageHeading
        title="Connections and faults"
        description="Read-only gateway sessions, market data feeds and fault telemetry."
      />
      <div className="border-t pt-6"><h2 className="text-base font-medium">Reported gateway telemetry</h2><p className="mt-1 text-xs text-muted-foreground">{status ? `API report: ${humanizeEnumValue(status.serviceState)}, observed ${formatWallClockTime(status.observedAt)}. ${status.telemetryDroppedUpdateCount} telemetry updates dropped.` : "Loading the research API telemetry report."} This API does not automatically monitor a separately started protocol server.</p></div>
      {connections.failure ? <ApiFailureAlert failure={connections.failure} onRetry={connections.reload} /> : null}
      {status?.serviceState === "not_running" ? <section className="border bg-card p-4 text-sm" aria-label="Protocol mode unavailable">
        <h2 className="font-medium">No live gateway telemetry reported</h2>
        <p className="mt-1 text-muted-foreground">The research API reports no attached gateway. This does not establish whether a separately launched server is running. Use your native server and client logs to verify connectivity; browser launch, control and live gateway telemetry are not integrated.</p>
        <Link className="mt-3 inline-block underline" href="/experiments/new">Configure built-in strategy research</Link>
      </section> : null}
      {status ? (
        <div className="grid min-w-0 gap-8">
          <ResearchSection title="Order entry sessions">
            {status.sessions.length === 0 ? <p className="text-sm text-muted-foreground">No sessions supplied by this API. Use the native server logs to check its actual connection state.</p> : <div className="min-w-0 border bg-card">
              <Table className="text-xs" aria-label="Order entry sessions">
                <TableHeader>
                  <TableRow>
                    <TableHead>Session</TableHead>
                    <TableHead>Protocol</TableHead>
                    <TableHead>User</TableHead>
                    <TableHead>State</TableHead>
                    <TableHead>Remote</TableHead>
                    <TableHead>Connected</TableHead>
                    <TableHead>Last inbound</TableHead>
                    <TableHead className="text-right">In / out seq</TableHead>
                    <TableHead className="text-right">Heartbeat</TableHead>
                    <TableHead className="text-right">Missed</TableHead>
                    <TableHead className="text-right">Live orders</TableHead>
                    <TableHead>Cancel on disconnect</TableHead>
                  </TableRow>
                </TableHeader>
                <TableBody>
                  {status.sessions.map((session) => (
                    <TableRow key={session.sessionId}>
                      <TableCell className="font-mono">{session.sessionId}</TableCell>
                      <TableCell>{humanizeEnumValue(session.protocol)}</TableCell>
                      <TableCell className="font-mono">{session.username}</TableCell>
                      <TableCell>
                        <Badge variant="outline"><span aria-hidden="true" className={`size-1.5 shrink-0 rounded-full ${SESSION_STATE_DOT[session.state]}`} />{humanizeEnumValue(session.state)}</Badge>
                      </TableCell>
                      <TableCell className="font-mono">{session.remoteAddress ?? "n/a"}</TableCell>
                      <TableCell className="font-mono">{formatWallClockTime(session.connectedAt)}</TableCell>
                      <TableCell className="font-mono">{formatWallClockTime(session.lastInboundAt)}</TableCell>
                      <TableCell className="text-right font-mono tabular-nums">
                        {formatIntegerText(session.inboundSequenceNumber)} / {formatIntegerText(session.outboundSequenceNumber)}
                      </TableCell>
                      <TableCell className="text-right font-mono tabular-nums">{session.heartbeatIntervalSeconds} s</TableCell>
                      <TableCell className="text-right font-mono tabular-nums">{session.missedHeartbeatCount}</TableCell>
                      <TableCell className="text-right font-mono tabular-nums">{session.liveOrderCount}</TableCell>
                      <TableCell>{session.cancelOnDisconnect ? "yes" : "no"}</TableCell>
                    </TableRow>
                  ))}
                </TableBody>
              </Table>
            </div>}
          </ResearchSection>
          {status.feeds.length === 0 ? <ResearchSection title="Market data feeds" description="No feed telemetry supplied by this API. Check feed reception in your native client. Deterministic research book views remain available in run inspection." /> : null}
          {status.feeds.map((feed) => (
            <ResearchSection key={feed.feedId} title={<>Feed {feed.feedId} ({humanizeEnumValue(feed.protocol)})</>} actions={<Badge variant="outline"><span aria-hidden="true" className={`size-1.5 rounded-full ${feed.recentGaps.some((gap) => gap.recoveryState !== "recovered") ? "bg-warning" : "bg-positive"}`} />{feed.recentGaps.some((gap) => gap.recoveryState !== "recovered") ? "Gaps" : "Healthy"}</Badge>} description={<span className="font-mono wrap-anywhere">
                  {feed.multicastGroup}, session {feed.mold64Session}, last sequence {formatIntegerText(feed.lastPublishedSequenceNumber)}, {feed.retransmissionRequestCount} retransmission requests
                </span>}>
              <div className="min-w-0 border bg-card">
                <Table className="text-xs" aria-label={`Feed ${feed.feedId} gaps`}>
                  <TableHeader>
                    <TableRow>
                      <TableHead className="text-right">Gap</TableHead>
                      <TableHead>Detected</TableHead>
                      <TableHead>Recovery</TableHead>
                    </TableRow>
                  </TableHeader>
                  <TableBody>
                    {feed.recentGaps.map((feedGap) => (
                      <TableRow key={`${feedGap.fromSequenceNumber}-${feedGap.toSequenceNumber}`}>
                        <TableCell className="text-right font-mono tabular-nums">
                          {feedGap.fromSequenceNumber} to {feedGap.toSequenceNumber}
                        </TableCell>
                        <TableCell className="font-mono">{formatWallClockTime(feedGap.detectedAt)}</TableCell>
                        <TableCell>
                          <Badge variant="outline"><span aria-hidden="true" className={`size-1.5 shrink-0 rounded-full ${feedGap.recoveryState === "unrecoverable" ? "bg-negative" : feedGap.recoveryState === "recovered" ? "bg-positive" : "bg-warning"}`} />{humanizeEnumValue(feedGap.recoveryState)}</Badge>
                        </TableCell>
                      </TableRow>
                    ))}
                    {feed.recentGaps.length === 0 ? (
                      <TableRow>
                        <TableCell colSpan={3} className="text-muted-foreground">
                          No recent gaps.
                        </TableCell>
                      </TableRow>
                    ) : null}
                  </TableBody>
                </Table>
              </div>
            </ResearchSection>
          ))}
          <ResearchSection title="Fault scenarios" description="Fault injection belongs to the native server configuration and admin interface. This table displays only faults reported by the API.">
            {status.faults.length === 0 ? <p className="text-sm text-muted-foreground">No fault telemetry supplied. Delayed-data research experiments are available in the Latency section of built-in setup.</p> : <div className="min-w-0 border bg-card">
              <Table className="text-xs" aria-label="Fault scenarios">
                <TableHeader>
                  <TableRow>
                    <TableHead>Fault</TableHead>
                    <TableHead>State</TableHead>
                    <TableHead>Target</TableHead>
                    <TableHead>Description</TableHead>
                    <TableHead className="text-right">Seed</TableHead>
                    <TableHead>Active</TableHead>
                  </TableRow>
                </TableHeader>
                <TableBody>
                  {status.faults.map((fault) => (
                    <TableRow key={fault.faultId}>
                      <TableCell>{humanizeEnumValue(fault.faultKind)}</TableCell>
                      <TableCell>
                        <Badge variant="outline"><span aria-hidden="true" className={`size-1.5 shrink-0 rounded-full ${fault.state === "active" ? "bg-warning" : "bg-muted-foreground"}`} />{fault.state}</Badge>
                      </TableCell>
                      <TableCell className="font-mono">{fault.target}</TableCell>
                      <TableCell>{fault.description}</TableCell>
                      <TableCell className="text-right font-mono tabular-nums">{fault.reproducibilitySeed ?? "n/a"}</TableCell>
                      <TableCell className="font-mono">
                        {formatWallClockTime(fault.activatedAt)} to {fault.clearedAt ? formatWallClockTime(fault.clearedAt) : "now"}
                      </TableCell>
                    </TableRow>
                  ))}
                </TableBody>
              </Table>
            </div>}
          </ResearchSection>
        </div>
      ) : null}
    </>
  );
}
