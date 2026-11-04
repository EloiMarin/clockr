// Protocol-level MCP tests: spawn `clockr-debug-mcp` over stdio with a mock
// debug client (main.ts's `--client-module` seam) and exercise the whole MCP
// layer — handshake, tools/list, tools/call dispatch and validation, resources,
// JSON-RPC error codes, notifications, both stdin framings, and stdout purity.
//
// This is the layer test. The e2e (e2e.test.ts) repeats the loop against the
// real `debugger_demo` + the real client; if this test passes and the e2e
// fails, the fault is in the transport/client wiring, not in MCP.
//
// Run: node test/protocol.test.ts

import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { McpClient } from "./support/rpc-client.ts";
import { callTool, expectRpcError } from "./support/tool-client.ts";
import { check, finish, ok, section } from "./support/harness.ts";

const HERE = dirname(fileURLToPath(import.meta.url));
const MOCK_MODULE = resolve(HERE, "support/mock-client.ts");

const EXPECTED_TOOLS = [
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
];

async function expectNoExtraMessage(client: McpClient): Promise<void> {
  try {
    const message = await client.nextMessage(150);

    ok("unknown notification is not answered", false, message);
  } catch {
    ok("unknown notification is not answered", true);
  }
}

const mcp = await McpClient.spawn({
  // A dead URL on purpose: the mock client ignores it, so a test that
  // accidentally used the real client would hang/fail instead of succeeding.
  url: "ws://127.0.0.1:1/ws",
  clientModule: MOCK_MODULE,
});

try {
  // --- handshake -----------------------------------------------------------

  section("initialize handshake");

  const initialized = (await mcp.request("initialize", {
    protocolVersion: "2025-06-18",
    capabilities: {},
    clientInfo: { name: "mcp-protocol-test", version: "0.0.0" },
  })) as {
    protocolVersion?: string;
    capabilities?: Record<string, unknown>;
    serverInfo?: { name?: string; version?: string };
    instructions?: string;
  };

  check("echoes a supported protocol revision", initialized.protocolVersion, "2025-06-18");
  check("serverInfo.name", initialized.serverInfo?.name, "clockr-debug-mcp");
  ok("serverInfo.version is a non-empty string", typeof initialized.serverInfo?.version === "string");
  ok("advertises tools capability", typeof initialized.capabilities?.tools === "object");
  ok("advertises resources capability", typeof initialized.capabilities?.resources === "object");
  ok(
    "instructions describe the workflow",
    typeof initialized.instructions === "string" && initialized.instructions.includes("graph_get"),
  );

  const negotiated = (await mcp.request("initialize", {
    protocolVersion: "1999-01-01",
    capabilities: {},
    clientInfo: { name: "mcp-protocol-test", version: "0.0.0" },
  })) as { protocolVersion?: string };

  check("unknown protocol revision negotiates down", negotiated.protocolVersion, "2025-06-18");

  mcp.notify("notifications/initialized");

  check("ping", await mcp.request("ping"), {});

  // Unknown notifications are silently dropped (never answered).
  mcp.notify("notifications/whatever", { ignored: true });
  check("requests work after an unknown notification", await mcp.request("ping"), {});
  await expectNoExtraMessage(mcp);

  // --- tools/list ----------------------------------------------------------

  section("tools/list");

  const toolsList = (await mcp.request("tools/list")) as {
    tools?: Array<{ name?: string; description?: string; inputSchema?: Record<string, unknown> }>;
  };
  const tools = toolsList.tools ?? [];

  check(
    "advertises exactly the ten debug tools",
    tools.map((tool) => tool.name),
    EXPECTED_TOOLS,
  );
  ok(
    "every tool has a description",
    tools.every((tool) => typeof tool.description === "string" && tool.description.length > 20),
  );
  ok(
    "every tool inputSchema is a JSON Schema object",
    tools.every(
      (tool) =>
        tool.inputSchema !== undefined &&
        tool.inputSchema.type === "object" &&
        typeof tool.inputSchema.properties === "object",
    ),
    tools.map((tool) => ({ name: tool.name, schema: tool.inputSchema })),
  );
  ok(
    "input_set requires port and value and explains the value mapping",
    (() => {
      const schema = tools.find((tool) => tool.name === "input_set")?.inputSchema as {
        required?: string[];
        properties?: Record<string, { description?: string }>;
      };

      return (
        Array.isArray(schema.required) &&
        schema.required.includes("port") &&
        schema.required.includes("value") &&
        typeof schema.properties?.value?.description === "string" &&
        schema.properties.value.description.includes("int64")
      );
    })(),
  );

  // --- JSON-RPC protocol errors --------------------------------------------

  section("JSON-RPC errors and malformed input");

  await expectRpcError("unknown method -> -32601", () => mcp.request("resources/subscribe", {}), -32601);
  await expectRpcError("unknown tool -> -32602", () => mcp.request("tools/call", { name: "nope" }), -32602);
  await expectRpcError(
    "missing required tool argument -> -32602",
    () => mcp.request("tools/call", { name: "input_set", arguments: {} }),
    -32602,
  );
  await expectRpcError(
    "unexpected tool argument -> -32602",
    () => mcp.request("tools/call", { name: "graph_get", arguments: { bogus: true } }),
    -32602,
  );
  await expectRpcError(
    "non-object arguments -> -32602",
    () => mcp.request("tools/call", { name: "graph_get", arguments: [1, 2] }),
    -32602,
  );
  await expectRpcError(
    "bad value mapping -> -32602",
    () => mcp.request("tools/call", { name: "input_set", arguments: { port: "bpm", value: { kind: "nope" } } }),
    -32602,
  );

  mcp.writeRaw("this is not json\n");

  const parseError = await mcp.nextMessage(2_000);

  check("malformed JSON line -> -32700", (parseError.error as { code?: number })?.code, -32700);
  check("parse error response id is null", parseError.id, null);

  // Content-Length framed input is accepted too (early-MCP/LSP clients).
  const pingBody = JSON.stringify({ jsonrpc: "2.0", id: "framed-ping", method: "ping" });
  const framed = `Content-Length: ${Buffer.byteLength(pingBody, "utf8")}\r\n\r\n${pingBody}`;

  mcp.writeRaw(Buffer.from(framed, "utf8"));

  const framedResponse = await mcp.nextMessage(2_000);

  check("Content-Length framed ping id", framedResponse.id, "framed-ping");
  check("Content-Length framed ping result", framedResponse.result, {});

  // --- tools/call against the mock ----------------------------------------

  section("tools/call: graph_get");
  const graph = await callTool(mcp, "graph_get");

  ok("graph_get is not an error", graph.isError === false);
  check("graph_get names the active example", (graph.payload as { example?: string }).example, "demo");
  check(
    "graph_get counts the mock graph's nodes",
    (graph.payload as { counts?: { nodes?: number } }).counts?.nodes,
    5,
  );
  check(
    "graph_get lists the boundary inputs",
    (graph.payload as { inputs?: Array<{ name?: string }> }).inputs?.map((port) => port.name),
    ["bpm", "gain"],
  );
  check(
    "graph_get lists the boundary outputs",
    (graph.payload as { outputs?: Array<{ name?: string }> }).outputs?.map((port) => port.name),
    ["tempo", "gain_out"],
  );
  ok(
    "graph_get serialises node ids as JSON numbers",
    typeof (graph.payload as { nodes?: Array<{ id?: unknown }> }).nodes?.[0]?.id === "number",
  );

  section("tools/call: events_tail / events_watch");

  const tail = await callTool(mcp, "events_tail", { n: 2 });
  const tailPayload = tail.payload as {
    count?: number;
    events?: Array<{ sequence?: unknown; clock?: string; kind?: string; value?: unknown }>;
    dropped?: { events?: Record<string, unknown>; commands?: unknown };
    cursors?: Record<string, unknown>;
    replay?: boolean;
  };

  ok("events_tail is not an error", tail.isError === false);
  check("events_tail honours n", tailPayload.count, 2);
  check("events_tail returns the two most recent events", tailPayload.events?.length, 2);
  check("event sequence is JSON-safe", typeof tailPayload.events?.[0]?.sequence, "number");
  check("event kind is the client's spelling", tailPayload.events?.[1]?.kind, "node_tick_end");
  check("events_tail surfaces trace drop counts", tailPayload.dropped?.events?.ui, 2);
  check("events_tail surfaces command drop counts", tailPayload.dropped?.commands, 0);
  ok(
    "events_tail returns per-clock cursors",
    typeof tailPayload.cursors?.audio === "number" && typeof tailPayload.cursors?.ui === "number",
    tailPayload.cursors,
  );

  const cursorBeforeInject = tailPayload.cursors as Record<string, number>;

  const injected = await callTool(mcp, "input_set", { port: "bpm", value: 123 });
  const injectedPayload = injected.payload as { port?: string; requestId?: unknown; appliedEpoch?: unknown };

  ok("input_set is not an error", injected.isError === false);
  check("input_set names the port", injectedPayload.port, "bpm");
  check("input_set returns the applied epoch", injectedPayload.appliedEpoch, 9);
  ok("input_set requestId is JSON-safe", typeof injectedPayload.requestId === "number");

  const watch = await callTool(mcp, "events_watch", { since: cursorBeforeInject, timeout: 1_000 });
  const watchPayload = watch.payload as {
    count?: number;
    events?: Array<{ value?: { kind?: string; value?: unknown }; epoch?: unknown }>;
    timedOut?: boolean;
    cursors?: Record<string, unknown>;
  };

  ok("events_watch is not an error", watch.isError === false);
  ok("events_watch returns the injected delta", (watchPayload.count ?? 0) >= 1, watchPayload);
  check("events_watch is not a timeout", watchPayload.timedOut, false);
  check(
    "events_watch carries the injected trace value (2 * 123)",
    watchPayload.events?.find((event) => event.value?.kind === "double")?.value?.value,
    246,
  );
  check("events_watch reports the applied epoch", watchPayload.events?.[0]?.epoch, 9);
  ok(
    "events_watch advances the cursor",
    typeof watchPayload.cursors?.audio === "number" &&
      (watchPayload.cursors.audio as number) > (cursorBeforeInject.audio as number),
    { before: cursorBeforeInject, after: watchPayload.cursors },
  );

  const readInput = await callTool(mcp, "input_get", { port: "bpm" });

  check("input_get returns the boundary input", (readInput.payload as { value?: { kind?: string } }).value?.kind, "double");
  check("input_get names the port", (readInput.payload as { port?: string }).port, "bpm");

  const readOutput = await callTool(mcp, "output_get", { port: "tempo" });

  check("output_get returns a double", (readOutput.payload as { value?: { kind?: string; value?: unknown } }).value?.value, 246);

  section("tools/call: pause / resume");

  const paused = await callTool(mcp, "pause");

  ok("pause is not an error", paused.isError === false);
  check("pause reports the capture gate", paused.payload, { paused: true });

  const resumed = await callTool(mcp, "resume");

  ok("resume is not an error", resumed.isError === false);
  check("resume reports the capture gate", resumed.payload, { paused: false });

  section("tools/call: example_list / example_select");

  const examples = await callTool(mcp, "example_list");

  check("example_list active", (examples.payload as { active?: string }).active, "demo");
  check("example_list names", (examples.payload as { names?: string[] }).names, ["demo", "daw"]);

  const selected = await callTool(mcp, "example_select", { name: "daw" });
  const selectedPayload = selected.payload as {
    selected?: string;
    active?: string;
    graph?: { inputs?: string[]; outputs?: string[] };
  };

  ok("example_select is not an error", selected.isError === false);
  check("example_select reports the requested example", selectedPayload.selected, "daw");
  check("example_select reports the active example", selectedPayload.active, "daw");
  check("example_select returns the new graph's inputs", selectedPayload.graph?.inputs, ["playing"]);

  const dawGraph = await callTool(mcp, "graph_get");

  check("graph_get follows example_select", (dawGraph.payload as { example?: string }).example, "daw");

  const unknownExample = await callTool(mcp, "example_select", { name: "nope" });

  ok("unknown example is a tool error, not a protocol error", unknownExample.isError === true);
  ok(
    "unknown example error carries the client's code",
    typeof (unknownExample.payload as { error?: string }).error === "string" &&
      (unknownExample.payload as { code?: string }).code === "unknown_example",
    unknownExample.payload,
  );

  // --- resources -----------------------------------------------------------

  section("resources");

  const resourceList = (await mcp.request("resources/list")) as {
    resources?: Array<{ uri?: string; name?: string; mimeType?: string }>;
  };

  check(
    "resources/list advertises the live graph resource",
    resourceList.resources?.map((resource) => resource.uri),
    ["graph://daw"],
  );
  check("graph resource is JSON", resourceList.resources?.[0]?.mimeType, "application/json");

  const read = (await mcp.request("resources/read", { uri: "graph://daw" })) as {
    contents?: Array<{ uri?: string; mimeType?: string; text?: string }>;
  };
  const resourceText = read.contents?.[0]?.text ?? "";
  const resourceGraph = JSON.parse(resourceText) as { example?: string; counts?: { nodes?: number } };

  check("resources/read echoes the uri", read.contents?.[0]?.uri, "graph://daw");
  check("resources/read marks JSON", read.contents?.[0]?.mimeType, "application/json");
  check("resources/read returns the live graph", resourceGraph.example, "daw");
  check("resources/read returns the graph's node count", resourceGraph.counts?.nodes, 3);

  await expectRpcError(
    "reading a non-active example is a resource error (-32002)",
    () => mcp.request("resources/read", { uri: "graph://demo" }),
    -32002,
  );
  await expectRpcError(
    "reading a non-graph URI is a resource error (-32002)",
    () => mcp.request("resources/read", { uri: "file:///etc/passwd" }),
    -32002,
  );

  // --- bounded event queries: caps, filters, summary, paging ---------------

  section("events: bounded tail shape, caps and nextSince paging (small mock)");

  const resultBytes = (result: { raw: unknown }): number => {
    const raw = result.raw as { content?: Array<{ text?: string }> };
    const text = raw.content?.[0]?.text ?? "";

    return Buffer.byteLength(text, "utf8");
  };

  interface EventPayload {
    count?: number;
    matched?: number;
    truncated?: boolean;
    timedOut?: boolean;
    replay?: boolean;
    events?: Array<{
      sequence?: number | string;
      clock?: string;
      nodeId?: number | string;
      kind?: string;
      epoch?: number | string;
    }>;
    nextSince?: Record<string, number | string>;
    cursors?: Record<string, number | string>;
    since?: Record<string, number | string>;
    autoPaged?: boolean;
    dropped?: { events?: Record<string, number | string>; commands?: number | string };
    summary?: {
      events?: number;
      byClock?: Record<string, { events?: number; byKind?: Record<string, number>; threw?: number }>;
      byKind?: Record<string, number>;
      nodes?: Array<{ nodeId?: unknown; count?: number }>;
      epochs?: { first?: unknown; last?: unknown };
      threw?: number;
    };
  }

  const tailDefault = await callTool(mcp, "events_tail", {});
  const tailDefaultPayload = tailDefault.payload as EventPayload;

  check("events_tail default returns the retained tail", tailDefaultPayload.count, 6);
  check("events_tail default matched", tailDefaultPayload.matched, 6);
  check("events_tail default is not truncated", tailDefaultPayload.truncated, false);
  check("events_tail default nextSince is the last kept sequence per clock", tailDefaultPayload.nextSince, {
    audio: 4,
    ui: 2,
  });
  ok("events_tail default stays well under the byte cap", resultBytes(tailDefault) <= 262_144);

  const tailFour = await callTool(mcp, "events_tail", { n: 4 });
  const tailFourPayload = tailFour.payload as EventPayload;

  check("events_tail {n:4} still returns 4 events", tailFourPayload.count, 4);
  check("events_tail {n:4} reports matched", tailFourPayload.matched, 6);
  check("events_tail {n:4} is truncated", tailFourPayload.truncated, true);
  check(
    "events_tail keeps the newest events (trim-from-front)",
    tailFourPayload.events?.map((event) => event.sequence),
    [3, 1, 2, 4],
  );
  check("events_tail {n:4} nextSince", tailFourPayload.nextSince, { audio: 4, ui: 2 });

  const afterTail = await callTool(mcp, "events_tail", { n: 4, since: tailFourPayload.nextSince });
  const afterTailPayload = afterTail.payload as EventPayload;

  check("paging past the tail yields nothing", afterTailPayload.count, 0);
  check("paging past the tail keeps nextSince unchanged", afterTailPayload.nextSince, tailFourPayload.nextSince);
  check("paging past the tail has no matched events", afterTailPayload.matched, 0);

  const cappedTail = await callTool(mcp, "events_tail", { n: 4, maxEvents: 2 });
  const cappedTailPayload = cappedTail.payload as EventPayload;

  check("events_tail maxEvents caps count", cappedTailPayload.count, 2);
  check("events_tail maxEvents keeps matched", cappedTailPayload.matched, 6);
  check("events_tail maxEvents flags truncation", cappedTailPayload.truncated, true);
  ok("events_tail maxEvents result honours the byte bound", resultBytes(cappedTail) <= 262_144);

  section("events: filters (clock / nodeId / kind)");

  const seededTail = await callTool(mcp, "events_tail", { n: 1, since: { audio: 1, ui: 1 } });
  const seededTailPayload = seededTail.payload as EventPayload;

  check("nextSince is seeded from since for clocks with no kept events", seededTailPayload.nextSince, {
    audio: 4,
    ui: 1,
  });
  check("seeded tail kept one event", seededTailPayload.count, 1);

  const audioOnly = await callTool(mcp, "events_tail", { clock: "audio" });
  const audioOnlyPayload = audioOnly.payload as EventPayload;

  check("clock filter keeps matching events", audioOnlyPayload.matched, 4);
  ok(
    "clock filter keeps only that clock",
    audioOnlyPayload.events?.every((event) => event.clock === "audio"),
    audioOnlyPayload.events,
  );

  const bothClocks = await callTool(mcp, "events_tail", { clock: ["audio", "ui"] });

  check("clock filter accepts an array", (bothClocks.payload as EventPayload).matched, 6);

  const nodeTwo = await callTool(mcp, "events_tail", { nodeId: "2" });

  check("nodeId filter accepts a decimal string", (nodeTwo.payload as EventPayload).matched, 1);

  const nodePair = await callTool(mcp, "events_tail", { nodeId: ["2", "4"] });

  check("nodeId filter accepts an array of mixed forms", (nodePair.payload as EventPayload).matched, 2);

  const tickEnds = await callTool(mcp, "events_tail", { kind: ["node_tick_end"] });

  check("kind filter accepts an array", (tickEnds.payload as EventPayload).matched, 5);

  const uiTick = await callTool(mcp, "events_tail", { clock: "ui", kind: "node_tick_end", nodeId: 4 });

  check("filters combine as AND", (uiTick.payload as EventPayload).matched, 1);

  await expectRpcError("unknown clock filter -> -32602", () => mcp.request("tools/call", { name: "events_tail", arguments: { clock: "worker" } }), -32602);
  await expectRpcError("unknown kind filter -> -32602", () => mcp.request("tools/call", { name: "events_tail", arguments: { kind: "nope" } }), -32602);
  await expectRpcError("negative nodeId filter -> -32602", () => mcp.request("tools/call", { name: "events_tail", arguments: { nodeId: -1 } }), -32602);
  await expectRpcError("fractional nodeId filter -> -32602", () => mcp.request("tools/call", { name: "events_tail", arguments: { nodeId: 1.5 } }), -32602);
  await expectRpcError("non-numeric nodeId string -> -32602", () => mcp.request("tools/call", { name: "events_tail", arguments: { nodeId: "abc" } }), -32602);
  await expectRpcError("empty filter array -> -32602", () => mcp.request("tools/call", { name: "events_tail", arguments: { clock: [] } }), -32602);
  await expectRpcError("maxEvents 0 -> -32602", () => mcp.request("tools/call", { name: "events_tail", arguments: { maxEvents: 0 } }), -32602);
  await expectRpcError("maxEvents 2001 -> -32602", () => mcp.request("tools/call", { name: "events_tail", arguments: { maxEvents: 2001 } }), -32602);
  await expectRpcError("maxBytes 1023 -> -32602", () => mcp.request("tools/call", { name: "events_tail", arguments: { maxBytes: 1023 } }), -32602);
  await expectRpcError("maxBytes 2097153 -> -32602", () => mcp.request("tools/call", { name: "events_tail", arguments: { maxBytes: 2_097_153 } }), -32602);
  await expectRpcError("non-boolean summary -> -32602", () => mcp.request("tools/call", { name: "events_tail", arguments: { summary: "yes" } }), -32602);
  await expectRpcError("unknown argument still rejected -> -32602", () => mcp.request("tools/call", { name: "events_tail", arguments: { bogus: 1 } }), -32602);

  section("events: summary mode");

  const summaryTail = await callTool(mcp, "events_tail", { summary: true, since: {} });
  const summaryTailPayload = summaryTail.payload as EventPayload;

  ok("summary mode has no events array", !("events" in (summaryTailPayload as object)));
  check("summary count is matched", summaryTailPayload.count, 6);
  check("summary matched", summaryTailPayload.matched, 6);
  check("summary.events", summaryTailPayload.summary?.events, 6);
  check("summary byClock audio events", summaryTailPayload.summary?.byClock?.audio?.events, 4);
  check("summary byClock ui events", summaryTailPayload.summary?.byClock?.ui?.events, 2);
  check("summary byKind node_tick_end", summaryTailPayload.summary?.byKind?.node_tick_end, 5);
  ok(
    "summary node histogram is a bounded top-N",
    Array.isArray(summaryTailPayload.summary?.nodes) &&
      summaryTailPayload.summary.nodes.length > 0 &&
      summaryTailPayload.summary.nodes.length <= 20,
    summaryTailPayload.summary?.nodes,
  );
  check("summary threw count", summaryTailPayload.summary?.threw, 0);
  ok("summary result stays under the byte cap", resultBytes(summaryTail) <= 262_144);

  const summaryFiltered = await callTool(mcp, "events_tail", { summary: true, clock: "ui" });

  check("summary respects filters", (summaryFiltered.payload as EventPayload).summary?.byClock?.ui?.events, 2);

  const fullTail = await callTool(mcp, "events_tail", { n: 2_000, maxEvents: 2_000, maxBytes: 2_097_152, since: {} });

  check("summary counts equal a non-summary scan", (fullTail.payload as EventPayload).count, summaryTailPayload.summary?.events);

  const summaryWatch = await callTool(mcp, "events_watch", { summary: true, since: {}, timeout: 800 });
  const summaryWatchPayload = summaryWatch.payload as EventPayload;

  ok("watch summary mode has no events array", !("events" in (summaryWatchPayload as object)));
  check("watch summary matched", summaryWatchPayload.matched, 6);
  check("watch summary did not time out", summaryWatchPayload.timedOut, false);

  section("events: watch caps, early abort and paging");

  const watchDefault = await callTool(mcp, "events_watch", { since: {}, timeout: 800 });
  const watchDefaultPayload = watchDefault.payload as EventPayload;

  check("fresh watch returns the retained delta", watchDefaultPayload.count, 6);
  check("fresh watch matched", watchDefaultPayload.matched, 6);
  check("fresh watch is not truncated", watchDefaultPayload.truncated, false);
  check("fresh watch nextSince", watchDefaultPayload.nextSince, { audio: 4, ui: 2 });

  const pageOne = await callTool(mcp, "events_watch", { since: {}, maxEvents: 2, timeout: 800 });
  const pageOnePayload = pageOne.payload as EventPayload;

  check("watch maxEvents caps count", pageOnePayload.count, 2);
  check("watch maxEvents flags truncation (early abort)", pageOnePayload.truncated, true);
  check("capped watch returns nextSince", pageOnePayload.nextSince, { audio: 2 });

  const pageTwo = await callTool(mcp, "events_watch", { since: pageOnePayload.nextSince, maxEvents: 2, timeout: 800 });
  const pageTwoPayload = pageTwo.payload as EventPayload;

  check("paged watch continues at nextSince", pageTwoPayload.nextSince, { audio: 3, ui: 1 });

  const pageThree = await callTool(mcp, "events_watch", { since: pageTwoPayload.nextSince, maxEvents: 2, timeout: 800 });
  const pageThreePayload = pageThree.payload as EventPayload;

  check("third watch page continues at nextSince", pageThreePayload.nextSince, { audio: 4, ui: 2 });
  check("third watch page hit the count cap exactly", pageThreePayload.count, 2);

  const pageFour = await callTool(mcp, "events_watch", { since: pageThreePayload.nextSince, maxEvents: 2, timeout: 500 });
  const pageFourPayload = pageFour.payload as EventPayload;

  check("last watch page is empty", pageFourPayload.count, 0);
  check("last watch page is not truncated", pageFourPayload.truncated, false);

  const pagedEvents = [
    ...(pageOnePayload.events ?? []),
    ...(pageTwoPayload.events ?? []),
    ...(pageThreePayload.events ?? []),
  ];
  const lastPerClock = new Map<string, number>();
  let pagingMonotonic = true;

  for (const event of pagedEvents) {
    const clock = event.clock ?? "";
    const sequence = Number(event.sequence);
    const last = lastPerClock.get(clock);

    if (last !== undefined && sequence <= last) pagingMonotonic = false;

    lastPerClock.set(clock, sequence);
  }

  ok("paged watch sequences strictly increase per clock", pagingMonotonic, pagedEvents);
  check("paged watch delivers every event exactly once", pagedEvents.length, 6);

  // --- auto-paging: continuation is the no-since default -------------------

  section("events: auto-paging (no-since continuation)");

  try {
    // Seed a truncated page explicitly, so the stored cursor is known and the
    // explicit-since path (autoPaged false) is pinned first.
    const seedPage = await callTool(mcp, "events_watch", { since: {}, maxEvents: 2, timeout: 800 });
    const seedPayload = seedPage.payload as EventPayload;

    check("explicit first page is not auto-paged", seedPayload.autoPaged, false);
    check("explicit since is echoed", seedPayload.since, {});
    check("explicit first page is truncated", seedPayload.truncated, true);
    check("explicit first page nextSince", seedPayload.nextSince, { audio: 2 });

    // No-since call #1 continues the page automatically.
    const autoOne = await callTool(mcp, "events_watch", { maxEvents: 2, timeout: 800 });
    const autoOnePayload = autoOne.payload as EventPayload;

    check("no-since watch after a truncated page continues automatically", autoOnePayload.autoPaged, true);
    check("auto page echoes the cursor it used", autoOnePayload.since, { audio: 2 });
    check(
      "auto page continues from the previous nextSince",
      autoOnePayload.events?.map((event) => event.sequence),
      [3, 1],
    );
    check("auto page nextSince", autoOnePayload.nextSince, { audio: 3, ui: 1 });
    check("auto page is still truncated", autoOnePayload.truncated, true);

    // No-since call #2.
    const autoTwo = await callTool(mcp, "events_watch", { maxEvents: 2, timeout: 800 });
    const autoTwoPayload = autoTwo.payload as EventPayload;

    check("second auto page continues", autoTwoPayload.autoPaged, true);
    check("second auto page echoes the cursor it used", autoTwoPayload.since, { audio: 3, ui: 1 });
    check(
      "second auto page events are the remainder",
      autoTwoPayload.events?.map((event) => event.sequence),
      [2, 4],
    );
    check("second auto page nextSince", autoTwoPayload.nextSince, { audio: 4, ui: 2 });
    console.log(
      `auto-paging: explicit page nextSince={${JSON.stringify(seedPayload.nextSince)}} -> ` +
        `no-since page autoPaged=${String(autoOnePayload.autoPaged)} since={${JSON.stringify(autoOnePayload.since)}} ` +
        `nextSince={${JSON.stringify(autoTwoPayload.nextSince)}} (events continue 1,2,3,4 + ui 1,2)`,
    );

    // No-since call #3: caught up -> the state is cleared.
    const autoThree = await callTool(mcp, "events_watch", { maxEvents: 2, timeout: 600 });
    const autoThreePayload = autoThree.payload as EventPayload;

    check("caught-up auto page is empty", autoThreePayload.count, 0);
    check("caught-up auto page is not truncated", autoThreePayload.truncated, false);
    check("caught-up auto page still echoes the cursor it used", autoThreePayload.since, { audio: 4, ui: 2 });

    // No-since call #4: now behaves like a normal live watch again.
    const autoFour = await callTool(mcp, "events_watch", { maxEvents: 2, timeout: 400 });
    const autoFourPayload = autoFour.payload as EventPayload;

    check("after catching up, a no-since watch is not auto-paged", autoFourPayload.autoPaged, false);
    check("after catching up, no cursor is echoed", autoFourPayload.since, {});

    // An explicit `since` always wins over a stored auto cursor (it also
    // re-bases the stored cursor to the explicit result).
    const override = await callTool(mcp, "events_watch", { since: {}, maxEvents: 2, timeout: 800 });
    const overridePayload = override.payload as EventPayload;

    check("explicit since overrides auto-paging", overridePayload.autoPaged, false);
    check("explicit since is echoed on the override", overridePayload.since, {});
    check(
      "explicit override starts from the beginning, not the continuation",
      overridePayload.events?.map((event) => event.sequence),
      [1, 2],
    );
    check("override result stores its own nextSince", overridePayload.nextSince, { audio: 2 });

    // Example switch clears the stored cursor: the next no-since watch must not
    // continue the demo page against the daw graph.
    const switched = await callTool(mcp, "example_select", { name: "daw" });

    ok("example switch succeeded", switched.isError === false);

    const afterSwitch = await callTool(mcp, "events_watch", { maxEvents: 2, timeout: 500 });
    const afterSwitchPayload = afterSwitch.payload as EventPayload;

    check("example switch clears auto-paging", afterSwitchPayload.autoPaged, false);
    check("example switch does not reuse the old cursor", afterSwitchPayload.since, {});

    // Switch back so later assertions in this suite see the same example as
    // before this section (the switch also clears the page cursor).
    await callTool(mcp, "example_select", { name: "demo" });
  } finally {
    // A failed assertion above must not leave a page cursor for later
    // sections; the switch back is the normal clear, this is the belt.
    await callTool(mcp, "events_watch", { since: {}, maxEvents: 2_000, maxBytes: 2_097_152, timeout: 500 });
  }

  // --- large verbose trace: hard byte/count caps ---------------------------

  section("events: hard caps on a large verbose trace (MOCK_LARGE)");

  const large = await McpClient.spawn({
    url: "ws://127.0.0.1:1/ws",
    clientModule: MOCK_MODULE,
    env: { MOCK_LARGE: "1" },
  });

  const largeResults: Array<{ name: string; result: { raw: unknown }; maxBytes: number }> = [];
  const tracked = (name: string, result: { raw: unknown }, maxBytes = 262_144): void => {
    largeResults.push({ name, result, maxBytes });
  };

  try {
    await large.request("initialize", { protocolVersion: "2025-06-18", capabilities: {}, clientInfo: { name: "caps", version: "0" } });
    large.notify("notifications/initialized");
    await callTool(large, "graph_get");

    const full = await callTool(large, "events_tail", {
      n: 2_000,
      maxEvents: 2_000,
      maxBytes: 2_097_152,
      since: {},
    });
    tracked("full tail", full, 2_097_152);

    const fullPayload = full.payload as EventPayload;
    const fullEvents = fullPayload.events ?? [];

    check("large trace total", fullPayload.matched, 800);
    check("large trace returned in full when bounds allow", fullPayload.count, 800);
    check("large trace full result is not truncated", fullPayload.truncated, false);
    ok("the large trace really is bigger than the default byte cap", resultBytes(full) > 262_144, resultBytes(full));
    console.log(
      `large trace full scan (maxBytes 2 MiB): ${fullPayload.count} events, ${resultBytes(full)} bytes — ` +
        `default caps would trim this`,
    );

    const fullSequences = fullEvents.map((event) => Number(event.sequence));

    const byteTrimmed = await callTool(large, "events_tail", {
      n: 2_000,
      maxEvents: 2_000,
      maxBytes: 4_096,
      since: {},
    });
    tracked("byte-trimmed tail", byteTrimmed, 4_096);

    const byteTrimmedPayload = byteTrimmed.payload as EventPayload;
    const byteTrimmedEvents = byteTrimmedPayload.events ?? [];

    ok("tail maxBytes is a hard bound", resultBytes(byteTrimmed) <= 4_096, resultBytes(byteTrimmed));
    ok("tail maxBytes dropped events", (byteTrimmedPayload.count ?? 0) > 0 && (byteTrimmedPayload.count ?? 0) < 800);
    check("tail byte trimming keeps the newest events (suffix of the full tail)", byteTrimmedEvents.map((event) => Number(event.sequence)), fullSequences.slice(-(byteTrimmedPayload.count ?? 0)));
    check("tail byte trimming flags truncation", byteTrimmedPayload.truncated, true);
    console.log(
      `large trace events_tail maxBytes=4096: count=${byteTrimmedPayload.count} of ${byteTrimmedPayload.matched}, ` +
        `bytes=${resultBytes(byteTrimmed)}`,
    );

    const capped = await callTool(large, "events_tail", { n: 50 });
    tracked("default tail", capped);

    check("default tail caps at min(n, 200)", (capped.payload as EventPayload).count, 50);
    check("default tail reports matched 800", (capped.payload as EventPayload).matched, 800);
    check("default tail is truncated", (capped.payload as EventPayload).truncated, true);

    const watchCapped = await callTool(large, "events_watch", { since: {}, timeout: 800 });
    tracked("fresh watch", watchCapped);

    const watchCappedPayload = watchCapped.payload as EventPayload;

    check("fresh watch is capped at 200 events", watchCappedPayload.matched, 200);
    check("fresh watch count is capped at 200 events", watchCappedPayload.count, 200);
    check("fresh watch is truncated", watchCappedPayload.truncated, true);
    ok("fresh watch respects the default byte cap", resultBytes(watchCapped) <= 262_144);
    check("fresh watch nextSince points at the last kept sequence", watchCappedPayload.nextSince, { audio: 200 });
    console.log(
      `large trace fresh events_watch (defaults): count=${watchCappedPayload.count} of ${watchCappedPayload.matched}, ` +
        `bytes=${resultBytes(watchCapped)}`,
    );

    const watchBytes = await callTool(large, "events_watch", {
      since: {},
      maxEvents: 2_000,
      maxBytes: 4_096,
      timeout: 800,
    });
    tracked("byte-watch", watchBytes, 4_096);

    const watchBytesPayload = watchBytes.payload as EventPayload;

    ok("watch maxBytes is a hard bound", resultBytes(watchBytes) <= 4_096, resultBytes(watchBytes));
    check("watch byte trimming keeps the oldest prefix", (watchBytesPayload.events ?? []).map((event) => Number(event.sequence)), fullSequences.slice(0, watchBytesPayload.count ?? 0));
    check("watch byte trimming flags truncation", watchBytesPayload.truncated, true);
    console.log(
      `large trace events_watch maxBytes=4096: count=${watchBytesPayload.count} of ${watchBytesPayload.matched}, ` +
        `bytes=${resultBytes(watchBytes)}`,
    );

    // Paging loop on the real mock delta: walk maxEvents at a time, following
    // nextSince; each page must continue exactly where the previous stopped.
    let pageSince: Record<string, number | string> = {};
    const pages: EventPayload[] = [];

    for (let page = 0; page < 12; page += 1) {
      const result = await callTool(large, "events_watch", {
        since: pageSince,
        maxEvents: 300,
        timeout: 800,
      });
      tracked(`page ${page}`, result);

      const payload = result.payload as EventPayload;

      pages.push(payload);

      if (payload.truncated !== true) break;

      pageSince = payload.nextSince ?? pageSince;
    }

    check("paging loop reached the end of the delta", pages.at(-1)?.truncated, false);
    console.log(
      `large trace paging: ${pages.length} pages of maxEvents=300, ${pages.reduce((sum, page) => sum + (page.events?.length ?? 0), 0)} events`,
    );

    const paged: EventPayload["events"] = [];
    const seen = new Set<string>();
    const pageLast = new Map<string, number>();
    let duplicates = 0;
    let backwards = 0;

    for (const page of pages) {
      for (const event of page.events ?? []) {
        const clock = event.clock ?? "";
        const sequence = Number(event.sequence);
        const key = `${clock}:${sequence}`;
        const last = pageLast.get(clock);

        if (seen.has(key)) duplicates += 1;
        if (last !== undefined && sequence <= last) backwards += 1;

        seen.add(key);
        pageLast.set(clock, sequence);
        paged.push(event);
      }
    }

    check("paging loop produces no duplicates", duplicates, 0);
    check("paging loop sequences strictly increase per clock", backwards, 0);
    check("paging loop delivers every event exactly once", paged.length, 800);
    check("paging loop audio total", pages.reduce((sum, page) => sum + (page.events ?? []).filter((event) => event.clock === "audio").length, 0), 500);
    check("paging loop ui total", pages.reduce((sum, page) => sum + (page.events ?? []).filter((event) => event.clock === "ui").length, 0), 300);

    section("events: filters and summary on the large trace");

    const throws = await callTool(large, "events_tail", { clock: "ui", kind: "node_tick_throw", n: 100 });
    tracked("filtered throws", throws);

    const throwsPayload = throws.payload as EventPayload;

    check("kind/clock filter matched the thrown ui ticks", throwsPayload.matched, 12);
    ok(
      "filtered events all match",
      throwsPayload.events?.every(
        (event) => event.clock === "ui" && event.kind === "node_tick_throw",
      ),
      throwsPayload.events,
    );

    const nodeFiltered = await callTool(large, "events_tail", {
      clock: "ui",
      nodeId: ["2", "5"],
      n: 2_000,
      maxEvents: 2_000,
      maxBytes: 2_097_152,
    });
    tracked("filtered nodes", nodeFiltered, 2_097_152);

    const nodeFilteredPayload = nodeFiltered.payload as EventPayload;

    check("nodeId array filter matched ui node 5", nodeFilteredPayload.matched, 150);
    ok(
      "nodeId array filter returns only matching nodes",
      nodeFilteredPayload.events?.every((event) => Number(event.nodeId) === 5 && event.clock === "ui"),
      nodeFilteredPayload.events?.slice(0, 3),
    );

    const summaryAll = await callTool(large, "events_tail", { summary: true, since: {} });
    tracked("summary all", summaryAll);

    const summaryAllPayload = summaryAll.payload as EventPayload;

    ok("large summary has no events array", !("events" in (summaryAllPayload as object)));
    check("large summary matched", summaryAllPayload.matched, 800);
    check("large summary count equals matched", summaryAllPayload.count, 800);
    check("large summary byClock audio", summaryAllPayload.summary?.byClock?.audio?.events, 500);
    check("large summary byClock ui", summaryAllPayload.summary?.byClock?.ui?.events, 300);
    check("large summary byKind total", Object.values(summaryAllPayload.summary?.byKind ?? {}).reduce((sum, count) => sum + Number(count), 0), 800);
    ok("large summary node histogram is bounded", (summaryAllPayload.summary?.nodes?.length ?? 0) <= 20);
    ok("large summary respects the byte cap", resultBytes(summaryAll) <= 262_144);
    check("large summary threw count", summaryAllPayload.summary?.threw, 22);

    const summaryThrows = await callTool(large, "events_tail", {
      summary: true,
      clock: "ui",
      kind: "node_tick_throw",
    });
    tracked("summary filtered", summaryThrows);

    check(
      "summary agrees with the filtered scan",
      (summaryThrows.payload as EventPayload).matched,
      throwsPayload.matched,
    );

    await expectRpcError("large graph unknown clock -> -32602", () => large.request("tools/call", { name: "events_tail", arguments: { clock: "nope" } }), -32602);

    for (const entry of largeResults) {
      ok(`result "${entry.name}" respects its maxBytes`, resultBytes(entry.result) <= entry.maxBytes, {
        bytes: resultBytes(entry.result),
        maxBytes: entry.maxBytes,
      });
    }
  } finally {
    await large.kill();
  }

  // --- shutdown / exit -----------------------------------------------------

  section("shutdown / exit");

  check("shutdown returns an empty result", await mcp.request("shutdown", {}), {});

  await expectRpcError(
    "requests after shutdown are refused (-32600)",
    () => mcp.request("ping", {}),
    -32600,
  );

  mcp.notify("exit");

  check("process exits 0 after exit notification", await mcp.waitExit(5_000), 0);
  ok("server wrote nothing unexpected to stderr", mcp.stderr.trim() === "", mcp.stderr);
} finally {
  await mcp.kill();
}

finish();
