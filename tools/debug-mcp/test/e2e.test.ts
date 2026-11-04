// End-to-end MCP harness: spawn the real `debugger_demo` (the debug server),
// spawn `clockr-debug-mcp` over stdio against it, and drive the whole
// debug-agent loop through MCP tools exactly as a coding agent would.
//
// Asserted:
//   initialize -> graph_get (the demo graph) -> events_tail (live trace)
//   -> events_watch (wait for a delta) -> input_set("bpm", 123) Ack with a
//   positive applied epoch -> the traced stream shows bpm 123 and tempo 246
//   (2 * bpm) -> input_get / output_get read back -> pause gates capture,
//   resume restores it -> example_list -> resources/list + resources/read
//   follow the active example -> shutdown / exit leaves a clean exit code.
//
// This test repeats the *real* path: no mock client, no fake frames. The port
// is picked free in 19000-19999, readiness is a TCP connect (the demo's piped
// stdout is block-buffered), and the demo is SIGKILLed afterwards.
//
// Run:
//   node test/e2e.test.ts
//   DEBUGGER_DEMO=/path/to/debugger_demo node test/e2e.test.ts
//
// Without a `debugger_demo` binary the harness prints SKIP and exits 0 so the
// protocol suite can run on a machine without a C++ build; when the binary is
// present every check above runs for real.

import { dirname } from "node:path";
import { fileURLToPath } from "node:url";

import { check, finish, ok, section, sleep, waitFor } from "./support/harness.ts";
import { hasDebuggerDemo } from "./support/processes.ts";
import { pickPort, startDebuggerDemo } from "./support/processes.ts";
import { McpClient } from "./support/rpc-client.ts";
import { callTool, waitForTool } from "./support/tool-client.ts";

const HERE = dirname(fileURLToPath(import.meta.url));

if (!hasDebuggerDemo()) {
  console.log("SKIP e2e: no debugger_demo binary");
  console.log("      build a preset, or set DEBUGGER_DEMO=/path/to/debugger_demo");
  process.exit(0);
}

interface TraceEventJson {
  sequence?: number | string;
  clock?: string;
  nodeId?: number | string;
  epoch?: number | string;
  kind?: string;
  value?: { kind?: string; value?: unknown } | null;
}

interface ToolPayload {
  count?: number;
  matched?: number;
  truncated?: boolean;
  events?: TraceEventJson[];
  nextSince?: Record<string, number | string>;
  since?: Record<string, number | string>;
  autoPaged?: boolean;
  summary?: {
    events?: number;
    byClock?: Record<string, { events?: number; threw?: number }>;
    byKind?: Record<string, number>;
    nodes?: Array<{ nodeId?: unknown; count?: number }>;
    threw?: number;
  };
  cursors?: Record<string, number | string>;
  dropped?: { events?: Record<string, number | string>; commands?: number | string };
  timedOut?: boolean;
  example?: string;
  counts?: { nodes?: number };
  inputs?: Array<{ nodeId?: number | string; name?: string }>;
  outputs?: Array<{ nodeId?: number | string; name?: string }>;
  active?: string;
  names?: string[];
  selected?: string;
  paused?: boolean;
  requestId?: number | string;
  port?: string;
  appliedEpoch?: number | string;
  value?: { kind?: string; value?: unknown };
}

const port = await pickPort();
const demo = await startDebuggerDemo({ port });
const url = `ws://127.0.0.1:${port}/ws`;
let mcp: McpClient | null = null;

try {
  // --- connect an MCP session to the live demo -----------------------------

  section(`e2e: MCP server on stdio against debugger_demo :${port}`);

  mcp = await McpClient.spawn({ url });

  const initialized = (await mcp.request("initialize", {
    protocolVersion: "2025-06-18",
    capabilities: {},
    clientInfo: { name: "clockr-debug-mcp-e2e", version: "0.0.0" },
  })) as { protocolVersion?: string; serverInfo?: { name?: string } };

  check("initialize handshake", initialized.protocolVersion, "2025-06-18");
  check("serverInfo", initialized.serverInfo?.name, "clockr-debug-mcp");

  mcp.notify("notifications/initialized");

  const tools = (await mcp.request("tools/list")) as { tools?: Array<{ name?: string }> };

  check(
    "tools/list exposes the debug tools",
    tools.tools?.map((tool) => tool.name),
    [
      "graph_get",
      "events_tail",
      "events_watch",
      "example_list",
      "example_select",
      "input_set",
      "input_get",
      "output_get",
      "pause",
      "resume",
    ],
  );

  // --- graph ---------------------------------------------------------------

  section("e2e: graph_get");

  const graph = await waitForTool(
    mcp,
    "graph_get",
    {},
    (payload) => (payload as ToolPayload).example === "demo" && ((payload as ToolPayload).counts?.nodes ?? 0) > 0,
  );
  const graphPayload = graph.payload as ToolPayload;
  const nodeCount = graphPayload.counts?.nodes ?? 0;
  const inputNames = (graphPayload.inputs ?? []).map((entry) => entry.name);
  const outputNames = (graphPayload.outputs ?? []).map((entry) => entry.name);

  ok("graph_get is not an error", graph.isError === false);
  check("graph_get names the active example", graphPayload.example, "demo");
  ok("graph_get returns the demo graph", nodeCount >= 10, { nodeCount });
  check("graph has the demo boundary inputs", [...(inputNames as string[])].sort(), ["bpm", "gain", "loop", "trigger"]);
  check(
    "graph has the demo boundary outputs",
    [...(outputNames as string[])].sort(),
    ["gain_out", "loop_len", "tempo", "ticks", "triggers"],
  );

  const bpmNodeId = graphPayload.inputs?.find((entry) => entry.name === "bpm")?.nodeId;
  const tempoNodeId = graphPayload.outputs?.find((entry) => entry.name === "tempo")?.nodeId;

  ok("bpm input node resolved", bpmNodeId !== undefined);
  ok("tempo output node resolved", tempoNodeId !== undefined);

  // --- trace ---------------------------------------------------------------

  section("e2e: events_tail");

  const tail = await waitForTool(
    mcp,
    "events_tail",
    { n: 200 },
    (payload) => ((payload as ToolPayload).count ?? 0) > 0,
  );
  const tailPayload = tail.payload as ToolPayload;
  const clocks = new Set((tailPayload.events ?? []).map((event) => event.clock));

  ok("events_tail is not an error", tail.isError === false);
  ok("events_tail returned a live trace tail", (tailPayload.count ?? 0) > 0, { count: tailPayload.count });
  ok("events_tail returned events from at least one clock", clocks.size >= 1, [...clocks]);
  ok(
    "events_tail serialises bigint fields as JSON",
    typeof tailPayload.events?.[0]?.sequence === "number" &&
      typeof tailPayload.events?.[0]?.epoch === "number" &&
      typeof tailPayload.events?.[0]?.nodeId === "number",
    tailPayload.events?.[0],
  );

  const cursorsBeforeWatch = tailPayload.cursors ?? {};

  section("e2e: events_watch");

  const watch = await waitForTool(
    mcp,
    "events_watch",
    { since: cursorsBeforeWatch, timeout: 3_000 },
    (payload) => ((payload as ToolPayload).count ?? 0) > 0,
  );
  const watchPayload = watch.payload as ToolPayload;

  ok("events_watch is not an error", watch.isError === false);
  ok("events_watch returned a delta", (watchPayload.count ?? 0) > 0, { count: watchPayload.count });
  check("events_watch did not time out", watchPayload.timedOut, false);
  ok(
    "events_watch advanced the audio cursor",
    ((watchPayload.cursors?.audio as number) ?? 0) > ((cursorsBeforeWatch.audio as number) ?? 0),
    { before: cursorsBeforeWatch.audio, after: watchPayload.cursors?.audio },
  );

  // --- injection at a tick boundary ---------------------------------------

  section("e2e: input_set -> traced effect");

  const beforeInject = await callTool(mcp, "events_tail", { n: 5 });
  const cursorBeforeInject = (beforeInject.payload as ToolPayload).cursors ?? {};

  const injected = await callTool(mcp, "input_set", { port: "bpm", value: 123 });
  const injectedPayload = injected.payload as ToolPayload;
  const appliedEpoch = Number(injectedPayload.appliedEpoch);

  ok("input_set is not an error", injected.isError === false);
  check("input_set names the port", injectedPayload.port, "bpm");
  ok("input_set acked with a positive applied epoch", appliedEpoch > 0, { appliedEpoch });
  ok("input_set requestId is JSON-safe", typeof injectedPayload.requestId === "number");

  // Read the delta until the injected tick's values appear. The demo's ui
  // driver overwrites bpm every 33 ms, so the host applies 123 for exactly one
  // ui tick: that tick's bpm input value (123) and the tempo it publishes to
  // audio (2 * 123 = 246) are the evidence the write landed at a boundary.
  const collected: TraceEventJson[] = [];
  let bpmEvent: TraceEventJson | undefined;
  let tempoEvent: TraceEventJson | undefined;
  let since: Record<string, number | string> = cursorBeforeInject;

  const found = await waitFor(
    "traced effect of the injected bpm reaches the stream",
    async () => {
      const delta = await callTool(mcp as McpClient, "events_tail", {
        n: 2_000,
        since,
        nodeId: [bpmNodeId, tempoNodeId].filter((nodeId) => nodeId !== undefined) as Array<number | string>,
      });

      if (!delta.isError) {
        const payload = delta.payload as ToolPayload;

        for (const event of payload.events ?? []) {
          collected.push(event);

          const sameNode = (candidate: number | string | undefined, nodeId: number | string | undefined) =>
            candidate !== undefined && nodeId !== undefined && String(candidate) === String(nodeId);

          if (bpmEvent === undefined && sameNode(event.nodeId, bpmNodeId) && event.value?.kind === "double" && event.value.value === 123) {
            bpmEvent = event;
          }

          if (
            tempoEvent === undefined &&
            sameNode(event.nodeId, tempoNodeId) &&
            event.value?.kind === "double" &&
            event.value.value === 246
          ) {
            tempoEvent = event;
          }
        }

        since = payload.cursors ?? since;
      }

      return bpmEvent !== undefined && tempoEvent !== undefined;
    },
    10_000,
    100,
  );

  ok("the injected bpm value appears in the input node's trace", bpmEvent !== undefined, { appliedEpoch });
  ok("the traced tempo output reaches 2 * bpm (246)", tempoEvent !== undefined, {
    appliedEpoch,
    lastEvents: collected.slice(-4),
  });

  if (bpmEvent !== undefined) {
    console.log(
      `traced effect: ${bpmEvent.clock} node ${String(bpmEvent.nodeId)} = 123 (bpm) at epoch ${String(bpmEvent.epoch)}`,
    );
  }
  if (tempoEvent !== undefined) {
    console.log(
      `traced effect: ${tempoEvent.clock} node ${String(tempoEvent.nodeId)} = 246 (2 * 123) at epoch ${String(tempoEvent.epoch)}`,
    );
  }
  ok("the search found the effect before timing out", found);

  // --- read-back -----------------------------------------------------------

  section("e2e: input_get / output_get");

  const readInput = await callTool(mcp, "input_get", { port: "bpm" });
  const readInputPayload = readInput.payload as ToolPayload;

  ok("input_get is not an error", readInput.isError === false);
  check("input_get names the port", readInputPayload.port, "bpm");
  check("input_get returns a double", readInputPayload.value?.kind, "double");

  const readOutput = await callTool(mcp, "output_get", { port: "tempo" });
  const readOutputPayload = readOutput.payload as ToolPayload;

  ok("output_get is not an error", readOutput.isError === false);
  check("output_get names the port", readOutputPayload.port, "tempo");
  check("output_get returns a double", readOutputPayload.value?.kind, "double");
  console.log(`getOutput("tempo") = ${JSON.stringify(readOutputPayload.value)}`);

  const unknownPort = await callTool(mcp, "input_set", { port: "no_such_port", value: 1 });
  ok("unknown port -> isError tool result", unknownPort.isError === true);
  ok(
    "unknown port error carries the server's ack text",
    typeof (unknownPort.payload as { error?: string }).error === "string" &&
      (unknownPort.payload as { code?: string }).code === "ack",
    unknownPort.payload,
  );

  // --- bounded event queries ----------------------------------------------

  section("e2e: bounded event queries (caps, filters, summary, paging)");

  const textOf = (result: { raw: unknown }): string => {
    const raw = result.raw as { content?: Array<{ text?: string }> };

    return raw.content?.[0]?.text ?? "";
  };
  const bytesOf = (result: { raw: unknown }): number => Buffer.byteLength(textOf(result), "utf8");
  const withinBudget = (name: string, result: { raw: unknown }, maxBytes = 262_144): void => {
    const bytes = bytesOf(result);

    ok(`${name} respects its maxBytes`, bytes <= maxBytes, { bytes, maxBytes });
  };

  // (a) A fresh watch (explicit since:{} so the retained history is replayed)
  //     must be bounded by the defaults even though the trace is much bigger.
  const freshWatch = await callTool(mcp, "events_watch", { since: {}, timeout: 1_500 });
  const freshWatchPayload = freshWatch.payload as ToolPayload;

  withinBudget("fresh events_watch", freshWatch);
  ok("fresh events_watch count is capped at maxEvents (200)", (freshWatchPayload.count ?? 0) <= 200, {
    count: freshWatchPayload.count,
  });
  ok(
    "fresh events_watch reports more matched than returned",
    (freshWatchPayload.matched ?? 0) >= (freshWatchPayload.count ?? 0),
    { count: freshWatchPayload.count, matched: freshWatchPayload.matched },
  );
  check("fresh events_watch is truncated by a cap", freshWatchPayload.truncated, true);
  ok(
    "fresh events_watch returns nextSince",
    freshWatchPayload.nextSince !== undefined && Object.keys(freshWatchPayload.nextSince).length > 0,
    freshWatchPayload.nextSince,
  );
  console.log(
    `fresh events_watch (since:{}): count=${freshWatchPayload.count} matched=${freshWatchPayload.matched} ` +
      `truncated=${String(freshWatchPayload.truncated)} bytes=${bytesOf(freshWatch)} (default maxBytes 262144)`,
  );

  // (b) Paging loop: watch a cheap filter (one worker event per tick), cap each
  //     page at 5, and walk forward with nextSince until the delta drains.
  await sleep(600);

  const pagedEvents: TraceEventJson[] = [];
  const pagedSeen = new Set<string>();
  const pagedLast = new Map<string, number>();
  let pagedDuplicates = 0;
  let pagedBackwards = 0;
  let pagedPages = 0;
  let pageSince: Record<string, number | string> = {};
  let drained = false;
  let maxPageBytes = 0;

  while (pagedPages < 30) {
    const page = await callTool(mcp, "events_watch", {
      since: pageSince,
      clock: "worker",
      kind: "region_step_begin",
      maxEvents: 5,
      timeout: 1_500,
    });

    withinBudget(`paging page ${pagedPages}`, page);
    maxPageBytes = Math.max(maxPageBytes, bytesOf(page));

    const payload = page.payload as ToolPayload;

    pagedPages += 1;

    for (const event of payload.events ?? []) {
      const clock = event.clock ?? "";
      const sequence = Number(event.sequence);
      const key = `${clock}:${sequence}`;
      const last = pagedLast.get(clock);

      if (pagedSeen.has(key)) pagedDuplicates += 1;
      if (last !== undefined && sequence <= last) pagedBackwards += 1;

      pagedSeen.add(key);
      pagedLast.set(clock, sequence);
      pagedEvents.push(event);
    }

    pageSince = payload.nextSince ?? pageSince;

    if (payload.truncated !== true) {
      drained = true;
      break;
    }
  }

  console.log(`paging loop: ${pagedPages} pages, ${pagedEvents.length} events, largest page ${maxPageBytes} bytes`);
  ok("paging loop drained the worker delta", drained, { pages: pagedPages });
  ok("paging loop returned events", pagedEvents.length > 0, { count: pagedEvents.length });
  check("paging loop produces no duplicate events", pagedDuplicates, 0);
  check("paging loop sequences strictly increase per clock", pagedBackwards, 0);

  // The paged sequence must be a *contiguous prefix* of the same filtered
  // history: no gaps, nothing skipped between pages.
  const workerHistory = await callTool(mcp, "events_tail", {
    since: {},
    clock: "worker",
    kind: "region_step_begin",
    n: 2_000,
    maxEvents: 2_000,
    maxBytes: 2_097_152,
  });

  withinBudget("worker history scan", workerHistory, 2_097_152);

  const workerHistorySequences = ((workerHistory.payload as ToolPayload).events ?? []).map((event) => Number(event.sequence));
  const pagedSequences = pagedEvents.map((event) => Number(event.sequence));

  check(
    "paged events are a contiguous prefix of the filtered history",
    pagedSequences,
    workerHistorySequences.slice(0, pagedSequences.length),
  );
  ok("filtered history still covers at least the paged prefix", workerHistorySequences.length >= pagedSequences.length);

  // (c) Filters on the real graph: one node's ticks on one clock are exact.
  const filtered = await callTool(mcp, "events_tail", {
    since: {},
    clock: "ui",
    kind: "node_tick_end",
    nodeId: String(bpmNodeId),
    n: 2_000,
    maxEvents: 2_000,
    maxBytes: 2_097_152,
  });
  const filteredPayload = filtered.payload as ToolPayload;
  const filteredEvents = filteredPayload.events ?? [];

  withinBudget("filtered tail", filtered, 2_097_152);
  ok("filtered tail returns bpm ui ticks", filteredEvents.length > 0, { count: filteredEvents.length });
  ok(
    "filtered tail returns only matching events",
    filteredEvents.every(
      (event) =>
        event.clock === "ui" &&
        event.kind === "node_tick_end" &&
        String(event.nodeId) === String(bpmNodeId),
    ),
    filteredEvents.slice(0, 3),
  );
  check("filtered tail is not truncated with generous caps", filteredPayload.truncated, false);

  const unfiltered = await callTool(mcp, "events_tail", {
    since: {},
    n: 2_000,
    maxEvents: 2_000,
    maxBytes: 2_097_152,
  });

  withinBudget("unfiltered tail", unfiltered, 2_097_152);
  ok(
    "a filtered query is a much smaller payload than the full trace",
    bytesOf(filtered) < bytesOf(unfiltered),
    { filteredBytes: bytesOf(filtered), unfilteredBytes: bytesOf(unfiltered) },
  );
  console.log(
    `filtered bpm ui node_tick_end: count=${filteredEvents.length} bytes=${bytesOf(filtered)}; ` +
      `unfiltered tail (same generous caps): count=${(unfiltered.payload as ToolPayload).count} bytes=${bytesOf(unfiltered)}`,
  );

  // (d) Summary mode aggregates the same filtered scan without any events.
  //     Capture is paused first: a live trace grows between two calls, so an
  //     exact summary-vs-scan comparison needs a frozen history.
  await callTool(mcp, "pause");
  await sleep(300);

  const filteredSummary = await callTool(mcp, "events_tail", {
    summary: true,
    since: {},
    clock: "ui",
    kind: "node_tick_end",
    nodeId: String(bpmNodeId),
  });
  const filteredSummaryPayload = filteredSummary.payload as ToolPayload;

  const pausedScan = await callTool(mcp, "events_tail", {
    since: {},
    clock: "ui",
    kind: "node_tick_end",
    nodeId: String(bpmNodeId),
    n: 2_000,
    maxEvents: 2_000,
    maxBytes: 2_097_152,
  });
  const pausedScanPayload = pausedScan.payload as ToolPayload;

  withinBudget("filtered summary", filteredSummary);
  withinBudget("paused filtered scan", pausedScan, 2_097_152);
  ok("summary mode has no events array", !("events" in (filteredSummaryPayload as object)));
  check(
    "summary matched equals the filtered scan's matched count",
    filteredSummaryPayload.matched,
    pausedScanPayload.matched,
  );
  check(
    "summary byClock ui count equals the filtered scan",
    filteredSummaryPayload.summary?.byClock?.ui?.events,
    pausedScanPayload.count,
  );
  check("paused filtered scan is complete", pausedScanPayload.truncated, false);

  const wholeSummary = await callTool(mcp, "events_tail", { summary: true, since: {} });
  const wholeSummaryPayload = wholeSummary.payload as ToolPayload;

  withinBudget("whole summary", wholeSummary);
  ok(
    "summary covers more than one clock on the real graph",
    Object.keys(wholeSummaryPayload.summary?.byClock ?? {}).length >= 2,
    Object.keys(wholeSummaryPayload.summary?.byClock ?? {}),
  );
  ok(
    "summary node histogram is bounded",
    (wholeSummaryPayload.summary?.nodes?.length ?? 0) <= 20,
    wholeSummaryPayload.summary?.nodes?.length,
  );

  await callTool(mcp, "resume");

  console.log(
    `summary (paused, filtered bpm ui ticks): matched=${filteredSummaryPayload.matched} bytes=${bytesOf(filteredSummary)}; ` +
      `whole summary: matched=${wholeSummaryPayload.matched} bytes=${bytesOf(wholeSummary)} (no events array)`,
  );

  // (e) The canonical small tail stays small.
  const smallTail = await callTool(mcp, "events_tail", { n: 4 });
  const smallTailPayload = smallTail.payload as ToolPayload;

  withinBudget("tail n:4", smallTail);
  ok("tail n:4 returns at most 4 events", (smallTailPayload.count ?? 0) <= 4, {
    count: smallTailPayload.count,
  });
  ok("tail n:4 stays tiny", bytesOf(smallTail) < 16_384, { bytes: bytesOf(smallTail) });
  check("tail n:4 reports truncation", smallTailPayload.truncated, true);
  console.log(
    `events_tail {n:4}: count=${smallTailPayload.count} matched=${smallTailPayload.matched} ` +
      `truncated=${String(smallTailPayload.truncated)} bytes=${bytesOf(smallTail)}`,
  );

  // --- auto-paging: continuation is the no-since default -------------------

  section("e2e: auto-paging continuation (no-since after truncation)");

  await sleep(2_000); // accumulate worker region ticks so paging needs several pages

  const autoPageEvents: TraceEventJson[] = [];
  const autoSeen = new Set<string>();
  const autoLast = new Map<string, number>();
  let autoDuplicates = 0;
  let autoBackwards = 0;
  let autoPages = 0;

  const firstAuto = await callTool(mcp, "events_watch", {
    since: {},
    clock: "worker",
    kind: "region_step_begin",
    maxEvents: 10,
    timeout: 1_500,
  });
  const firstAutoPayload = firstAuto.payload as ToolPayload;

  withinBudget("auto-paging first page", firstAuto);
  check("explicit first page is not auto-paged", firstAutoPayload.autoPaged, false);
  check("explicit first page echoes its since", firstAutoPayload.since, {});
  check("auto-paging first page is truncated at maxEvents:10", firstAutoPayload.truncated, true);

  const collectAutoPage = (payload: ToolPayload): void => {
    autoPages += 1;

    for (const event of payload.events ?? []) {
      const clock = event.clock ?? "";
      const sequence = Number(event.sequence);
      const key = `${clock}:${sequence}`;
      const last = autoLast.get(clock);

      if (autoSeen.has(key)) autoDuplicates += 1;
      if (last !== undefined && sequence <= last) autoBackwards += 1;

      autoSeen.add(key);
      autoLast.set(clock, sequence);
      autoPageEvents.push(event);
    }
  };

  collectAutoPage(firstAutoPayload);

  let previousAuto = firstAutoPayload;
  let autoDrained = false;

  // These calls intentionally omit `since`: the server must continue the page.
  for (let page = 0; page < 6; page += 1) {
    const auto = await callTool(mcp, "events_watch", {
      clock: "worker",
      kind: "region_step_begin",
      maxEvents: 10,
      timeout: 1_500,
    });
    const payload = auto.payload as ToolPayload;

    withinBudget(`auto page ${page}`, auto);
    check(`auto page ${page} is auto-paged`, payload.autoPaged, true);
    check(
      `auto page ${page} echoes the continuation cursor`,
      payload.since,
      previousAuto.nextSince,
    );

    collectAutoPage(payload);
    previousAuto = payload;

    if (payload.truncated !== true) {
      autoDrained = true;
      break;
    }
  }

  console.log(
    `auto-paging: ${autoPages} page(s) (1 explicit + ${autoPages - 1} no-since), ${autoPageEvents.length} events, ` +
      `drained=${String(autoDrained)}`,
  );
  ok("auto-paging drained the worker delta", autoDrained, { pages: autoPages });
  ok("auto-paging exercised multiple no-since pages", autoPages >= 3, { pages: autoPages });
  check("auto-paging produces no duplicate events", autoDuplicates, 0);
  check("auto-paging sequences strictly increase per clock", autoBackwards, 0);

  const autoHistory = await callTool(mcp, "events_tail", {
    since: {},
    clock: "worker",
    kind: "region_step_begin",
    n: 2_000,
    maxEvents: 2_000,
    maxBytes: 2_097_152,
  });

  withinBudget("auto-paging history scan", autoHistory, 2_097_152);

  const autoHistorySequences = ((autoHistory.payload as ToolPayload).events ?? []).map((event) => Number(event.sequence));

  check(
    "auto-paged events are a contiguous prefix of the filtered history",
    autoPageEvents.map((event) => Number(event.sequence)),
    autoHistorySequences.slice(0, autoPageEvents.length),
  );

  // After catching up, a no-since watch is a normal live watch again.
  const afterCaughtUp = await callTool(mcp, "events_watch", { maxEvents: 5, timeout: 500 });
  const afterCaughtUpPayload = afterCaughtUp.payload as ToolPayload;

  check("after catching up, a no-since watch is not auto-paged", afterCaughtUpPayload.autoPaged, false);
  check("after catching up, no cursor is echoed", afterCaughtUpPayload.since, {});

  // An explicit since overrides; the result re-bases the page cursor.
  const autoOverride = await callTool(mcp, "events_watch", {
    since: {},
    clock: "worker",
    kind: "region_step_begin",
    maxEvents: 10,
    timeout: 1_500,
  });
  const autoOverridePayload = autoOverride.payload as ToolPayload;

  withinBudget("auto-paging explicit override", autoOverride);
  check("explicit since overrides auto-paging", autoOverridePayload.autoPaged, false);
  check("explicit override echoes its own since", autoOverridePayload.since, {});
  check("explicit override is truncated", autoOverridePayload.truncated, true);

  // --- pause / resume ------------------------------------------------------

  section("e2e: pause / resume");

  const paused = await callTool(mcp, "pause");

  ok("pause is not an error", paused.isError === false);
  check("pause reports the capture gate", (paused.payload as ToolPayload).paused, true);

  await sleep(700); // let the server drain already-recorded batches

  const settled = await callTool(mcp, "events_tail", { n: 5 });
  const quietCursors = (settled.payload as ToolPayload).cursors ?? {};

  await sleep(500); // while paused, no new trace events may be recorded

  const quiet = await callTool(mcp, "events_tail", { n: 25, since: quietCursors });
  const quietPayload = quiet.payload as ToolPayload;

  check("capture is gated: no events arrive while paused", quietPayload.count, 0);

  const resumed = await callTool(mcp, "resume");

  ok("resume is not an error", resumed.isError === false);
  check("resume reports the capture gate", (resumed.payload as ToolPayload).paused, false);

  const afterResume = await waitForTool(
    mcp,
    "events_watch",
    { since: quietPayload.cursors ?? quietCursors, timeout: 3_000 },
    (payload) => ((payload as ToolPayload).count ?? 0) > 0,
    6_000,
  );

  ok("events flow again after resume", ((afterResume.payload as ToolPayload).count ?? 0) > 0);

  // --- examples + resources ------------------------------------------------

  section("e2e: example_list / resources");

  const examples = await callTool(mcp, "example_list");

  check("example_list active", (examples.payload as ToolPayload).active, "demo");
  check("example_list names", (examples.payload as ToolPayload).names, ["demo"]);

  const resources = (await mcp.request("resources/list")) as {
    resources?: Array<{ uri?: string; mimeType?: string }>;
  };

  check(
    "resources/list advertises the live graph",
    resources.resources?.map((resource) => resource.uri),
    ["graph://demo"],
  );
  check("graph resource is JSON", resources.resources?.[0]?.mimeType, "application/json");

  const demoRead = (await mcp.request("resources/read", { uri: "graph://demo" })) as {
    contents?: Array<{ uri?: string; text?: string }>;
  };
  const demoGraph = JSON.parse(demoRead.contents?.[0]?.text ?? "null") as ToolPayload;

  check("resources/read echoes the uri", demoRead.contents?.[0]?.uri, "graph://demo");
  check("resources/read returns the demo graph", demoGraph.example, "demo");
  check("resources/read returns the demo root node count", demoGraph.counts?.nodes, nodeCount);

  // --- shutdown ------------------------------------------------------------

  section("e2e: shutdown / exit");

  check("shutdown returns an empty result", await mcp.request("shutdown", {}), {});

  mcp.notify("exit");

  check("server process exits 0", await mcp.waitExit(5_000), 0);
  ok("server wrote nothing unexpected to stderr", mcp.stderr.trim() === "", mcp.stderr);
} finally {
  await mcp?.kill();
  await demo.stop();
}

finish();
