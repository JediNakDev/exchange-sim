"use client";

// Read-only status of the real-time protocol mode: OUCH/FIX sessions, ITCH feed
// gaps and injected faults. Nothing here can change the service.

import Link from "next/link";
import { Badge } from "@/components/ui/badge";
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { ApiFailureAlert } from "@/components/research/api-failure-alert";
import { PageHeading } from "@/components/research/page-heading";
import { useApiResource } from "@/hooks/use-api-resource";
import { getBrowserApiClient, unwrapApiResult, type ApiSchemas } from "@/lib/api/exsim-api-client";
import { formatIntegerText, formatWallClockTime } from "@/lib/exact-decimal-format";
import { humanizeEnumValue } from "@/lib/research-labels";

const CONNECTIONS_POLL_INTERVAL_MS = 2000;

const SESSION_STATE_VARIANT: Record<ApiSchemas["ProtocolSessionStatus"]["state"], "default" | "secondary" | "destructive" | "outline"> = {
  logged_in: "default",
  connecting: "outline",
  recovering: "secondary",
  logging_out: "outline",
  disconnected: "outline",
  failed: "destructive",
};

export function ConnectionsView() {
  const apiClient = getBrowserApiClient();
  const connections = useApiResource("connections", async () => unwrapApiResult(await apiClient.GET("/v1/connections")), CONNECTIONS_POLL_INTERVAL_MS);
  const status = connections.data;
  return (
    <>
      <PageHeading
        title="Connections and faults"
        description={
          status ? (
            <span>
              {humanizeEnumValue(status.mode)} service {status.serviceState}, observed {formatWallClockTime(status.observedAt)}, {status.telemetryDroppedUpdateCount} telemetry updates dropped. Read only.
            </span>
          ) : (
            "Read only."
          )
        }
      />
      {connections.failure ? <ApiFailureAlert failure={connections.failure} onRetry={connections.reload} /> : null}
      {status?.serviceState === "not_running" ? <section className="rounded-md border bg-card p-4 text-sm" aria-label="Protocol mode unavailable">
        <h2 className="font-medium">Real-time protocol service is not running here</h2>
        <p className="mt-1 text-muted-foreground">Deterministic research runs do not create OUCH/FIX sessions or ITCH feed connections. This read-only page cannot start gateways or configure faults. Start a separate protocol service using the server setup instructions when testing an external client.</p>
        <Link className="mt-3 inline-block underline" href="/experiments">Return to research experiments</Link>
      </section> : null}
      {status ? (
        <div className="grid gap-4 xl:grid-cols-2">
          <Card size="sm" className="xl:col-span-2">
            <CardHeader>
              <CardTitle>Order entry sessions</CardTitle>
            </CardHeader>
            <CardContent className="overflow-x-auto">
              <Table className="text-xs">
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
                  {status.sessions.length === 0 ? <TableRow><TableCell colSpan={12} className="text-muted-foreground">No order-entry sessions reported. {status.serviceState === "not_running" ? "Start the separate protocol service before connecting clients." : "Connect an external client to the running protocol service to observe sessions here."}</TableCell></TableRow> : null}
                  {status.sessions.map((session) => (
                    <TableRow key={session.sessionId}>
                      <TableCell className="font-mono">{session.sessionId}</TableCell>
                      <TableCell>{humanizeEnumValue(session.protocol)}</TableCell>
                      <TableCell className="font-mono">{session.username}</TableCell>
                      <TableCell>
                        <Badge variant={SESSION_STATE_VARIANT[session.state]}>{humanizeEnumValue(session.state)}</Badge>
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
            </CardContent>
          </Card>
          {status.feeds.length === 0 ? <Card size="sm"><CardHeader><CardTitle>Market data feeds</CardTitle><CardDescription>No feeds reported. Deterministic research book views are available from each run’s event inspector; protocol feeds require the separate real-time service.</CardDescription></CardHeader></Card> : null}
          {status.feeds.map((feed) => (
            <Card size="sm" key={feed.feedId}>
              <CardHeader>
                <CardTitle>
                  Feed {feed.feedId} ({humanizeEnumValue(feed.protocol)})
                </CardTitle>
                <CardDescription className="font-mono">
                  {feed.multicastGroup}, session {feed.mold64Session}, last sequence {formatIntegerText(feed.lastPublishedSequenceNumber)}, {feed.retransmissionRequestCount} retransmission requests
                </CardDescription>
              </CardHeader>
              <CardContent>
                <Table className="text-xs">
                  <TableHeader>
                    <TableRow>
                      <TableHead>Gap</TableHead>
                      <TableHead>Detected</TableHead>
                      <TableHead>Recovery</TableHead>
                    </TableRow>
                  </TableHeader>
                  <TableBody>
                    {feed.recentGaps.map((feedGap) => (
                      <TableRow key={`${feedGap.fromSequenceNumber}-${feedGap.toSequenceNumber}`}>
                        <TableCell className="font-mono tabular-nums">
                          {feedGap.fromSequenceNumber} to {feedGap.toSequenceNumber}
                        </TableCell>
                        <TableCell className="font-mono">{formatWallClockTime(feedGap.detectedAt)}</TableCell>
                        <TableCell>
                          <Badge variant={feedGap.recoveryState === "unrecoverable" ? "destructive" : feedGap.recoveryState === "recovered" ? "secondary" : "outline"}>{humanizeEnumValue(feedGap.recoveryState)}</Badge>
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
              </CardContent>
            </Card>
          ))}
          <Card size="sm">
            <CardHeader>
              <CardTitle>Fault scenarios</CardTitle>
              <CardDescription>Faults are configured outside the UI; this view only reports them.</CardDescription>
            </CardHeader>
            <CardContent>
              <Table className="text-xs">
                <TableHeader>
                  <TableRow>
                    <TableHead>Fault</TableHead>
                    <TableHead>State</TableHead>
                    <TableHead>Target</TableHead>
                    <TableHead>Description</TableHead>
                    <TableHead>Seed</TableHead>
                    <TableHead>Active</TableHead>
                  </TableRow>
                </TableHeader>
                <TableBody>
                  {status.faults.length === 0 ? <TableRow><TableCell colSpan={6} className="text-muted-foreground">No protocol fault scenarios reported. Configure faults in the separate protocol service; this page reports their state. Delayed-data research experiments are available through the Latency section in experiment setup.</TableCell></TableRow> : null}
                  {status.faults.map((fault) => (
                    <TableRow key={fault.faultId}>
                      <TableCell>{humanizeEnumValue(fault.faultKind)}</TableCell>
                      <TableCell>
                        <Badge variant={fault.state === "active" ? "destructive" : "outline"}>{fault.state}</Badge>
                      </TableCell>
                      <TableCell className="font-mono">{fault.target}</TableCell>
                      <TableCell>{fault.description}</TableCell>
                      <TableCell className="font-mono">{fault.reproducibilitySeed ?? "n/a"}</TableCell>
                      <TableCell className="font-mono">
                        {formatWallClockTime(fault.activatedAt)} to {fault.clearedAt ? formatWallClockTime(fault.clearedAt) : "now"}
                      </TableCell>
                    </TableRow>
                  ))}
                </TableBody>
              </Table>
            </CardContent>
          </Card>
        </div>
      ) : null}
    </>
  );
}
