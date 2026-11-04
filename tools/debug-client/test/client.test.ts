// Client unit tests — the socket-level behaviour, driven by an in-process fake
// WebSocket (no server, no C++). Covers what the e2e harness cannot easily
// provoke deterministically:
//
//   - connect()/graph/examples plumbing and the frozen request/ack routing,
//   - Ack / PortValue success, error and timeout paths (never silently unacked),
//   - events(): history replay, per-clock sinceSequence, live subscription,
//     duplicate suppression, default read cursor, live:false tail mode,
//   - reconnect: automatic re-open, connect-time replay dedupe, pending
//     requests surviving the gap, server-restart detection by stamp_ns,
//   - drop accounting from EventBatch.dropped and CommandStats.dropped,
//   - selectExample / pause / resume encodings.
//
// Run: node test/client.test.ts

import {
  AckError,
  ClientClosedError,
  DebugClientError,
  RequestTimeoutError,
  createClient,
  type DebugClient,
  type EventBatchView,
} from "../src/index.ts";

import {
  FakeWebSocket,
  FakeWebSocketFactory,
} from "./support/fake-websocket.ts";

import {
  EVENT_KIND,
  ackFrame,
  batchFrame,
  commandStatsFrame,
  examplesFrame,
  graphFrame,
  portValueFrame,
  requestIdOf,
} from "./support/frames.ts";

import { check, finish, ok, rejects, section, sleep, waitFor } from "./support/harness.ts";

// --- helpers -----------------------------------------------------------------

async function connectedGraph(
  options: Parameters<typeof createClient>[1] = {},
): Promise<{ client: DebugClient; factory: FakeWebSocketFactory; socket: FakeWebSocket }> {
  const factory = new FakeWebSocketFactory();
  const client = createClient("ws://fake.test/ws", {
    webSocketFactory: factory.factory,
    requestTimeoutMs: 200,
    connectTimeoutMs: 1000,
    ...options,
  });

  const connecting = client.connect();
  const socket = factory.last;

  socket.emitOpen();
  socket.emitMessage(graphFrame({ name: "demo", inputs: [{ nodeId: 0, name: "bpm" }] }));

  await connecting;

  return { client, factory, socket };
}

interface Pumped {
  batches: EventBatchView[];
  done: Promise<void>;
  /** Abort the iterator's signal and wait for the generator to finish. */
  stop: () => Promise<void>;
}

/** Drain `client.events(options)` in the background; stop by aborting. */
function pumpEvents(client: DebugClient, options: Record<string, unknown> = {}): Pumped {
  const controller = new AbortController();
  const batches: EventBatchView[] = [];
  const done = (async () => {
    try {
      for await (const batch of client.events({ ...options, signal: controller.signal })) batches.push(batch);
    } catch (error) {
      if ((error as Error).name !== "AbortError") throw error;
    }
  })();

  return { batches, done, stop: async () => {
    controller.abort();
    await done;
  } };
}

function sequencesOf(batches: EventBatchView[]): bigint[] {
  return batches.flatMap((batch) => batch.events.map((event) => event.sequence));
}

// --- connect + graph + examples ---------------------------------------------

section("connect / graph / examples");

{
  const factory = new FakeWebSocketFactory();
  const client = createClient("ws://fake.test/ws", { webSocketFactory: factory.factory, reconnect: false });

  check("idle before connect", client.state, "idle");
  check("no graph before connect", client.graph, null);

  const connecting = client.connect();
  const socket = factory.last;

  check("socket is opened by connect()", factory.sockets.length, 1);
  check("state is connecting while the socket opens", client.state, "connecting");

  socket.emitOpen();
  socket.emitMessage(examplesFrame(["demo", "daw"], "demo"));
  socket.emitMessage(graphFrame({ name: "demo", nodes: [{ id: 1, kind: "internal", clock: "audio" }] }));

  const graph = await connecting;

  check("connect resolves the graph", graph.name, "demo");
  check("state is open", client.state, "open");
  check("examples are cached", await client.examples(), { names: ["demo", "daw"], active: "demo" });
  check("current example", client.currentExample, "demo");

  let graphNotices = 0;
  const unsubscribe = client.onGraph(() => {
    graphNotices += 1;
  });

  socket.emitMessage(graphFrame({ name: "demo" }));
  await waitFor("onGraph fires for later descriptions", () => graphNotices === 1);
  unsubscribe();

  check("second connect returns the cached graph", (await client.connect()).name, "demo");
  client.close();
  check("state is closed after close()", client.state, "closed");
}

await rejects(
  "connect() times out without a graph",
  (async () => {
    const factory = new FakeWebSocketFactory();
    const client = createClient("ws://fake.test/ws", {
      webSocketFactory: factory.factory,
      connectTimeoutMs: 30,
      reconnect: false,
    });
    const connecting = client.connect();

    factory.last.emitOpen();

    try {
      await connecting;
    } finally {
      client.close();
    }
  })(),
  "ConnectError",
);

// --- events: replay + live + cursor -----------------------------------------

section("events(): history replay, live subscription, read cursor");

{
  const { client, socket } = await connectedGraph();

  socket.emitMessage(batchFrame({
    clock: "audio",
    events: [
      { sequence: 1n, stampNs: 1000n, kind: EVENT_KIND.NODE_EVAL_END, clock: "audio", value: { kind: "double", value: 1 } },
      { sequence: 2n, stampNs: 2000n, kind: EVENT_KIND.NODE_EVAL_END, clock: "audio", value: { kind: "double", value: 2 } },
    ],
  }));

  const first = client.events({ sinceSequence: { audio: 0n } });
  const firstBatch = await first.next();

  ok("history replay yields a batch", firstBatch.done !== true);
  check("history replay contains retained events", firstBatch.done ? [] : sequencesOf([firstBatch.value]), [1n, 2n]);

  // Ending the first iterator advances the client read cursor; the next
  // default iterator continues from there instead of replaying again.
  await first.return(undefined);

  socket.emitMessage(batchFrame({
    clock: "audio",
    events: [{ sequence: 3n, stampNs: 3000n, kind: EVENT_KIND.NODE_EVAL_END, clock: "audio" }],
  }));

  const stream = pumpEvents(client);

  await waitFor("default sinceSequence replays from the read cursor only", () => stream.batches.length === 1, {
    timeoutMs: 500,
  });
  check("second iterator does not replay consumed events", sequencesOf(stream.batches), [3n]);

  socket.emitMessage(batchFrame({
    clock: "audio",
    events: [{ sequence: 4n, stampNs: 4000n, kind: EVENT_KIND.NODE_EVAL_END, clock: "audio" }],
  }));

  await waitFor("live batch reaches the active iterator", () => stream.batches.length === 2);
  check("live batch sequence", sequencesOf(stream.batches), [3n, 4n]);

  // A duplicate frame (e.g. the server's reconnect replay) must not be
  // re-delivered: the sequence frontier filters it.
  socket.emitMessage(batchFrame({
    clock: "audio",
    events: [{ sequence: 3n, stampNs: 3000n, kind: EVENT_KIND.NODE_EVAL_END, clock: "audio" }],
  }));

  await sleep(20);
  check("duplicate replayed sequences are suppressed", sequencesOf(stream.batches), [3n, 4n]);

  await stream.stop();
  client.close();
}

section("events({live: false}) waits for the connect-time replay to drain");

{
  const { client, socket } = await connectedGraph({ replaySettleMs: 20 });

  const collected: EventBatchView[] = [];
  const finished = (async () => {
    for await (const batch of client.events({ live: false, sinceSequence: { audio: 0n } })) collected.push(batch);
  })();

  // The replay frames arrive after the iterator started: the wait path must
  // yield them and then end once the replay quiet period expires.
  socket.emitMessage(batchFrame({
    clock: "",
    events: [
      { sequence: 1n, stampNs: 1000n, kind: EVENT_KIND.REGION_EVAL_BEGIN, clock: "audio" },
      { sequence: 2n, stampNs: 2000n, kind: EVENT_KIND.REGION_EVAL_END, clock: "audio" },
    ],
  }));

  await waitFor("live:false iterator finishes after the replay", async () => {
    const settled = await Promise.race([finished.then(() => true), sleep(1).then(() => false)]);

    return settled;
  }, { timeoutMs: 1000 });

  check("live:false yields the replay tail", sequencesOf(collected), [1n, 2n]);
  ok("live:false marks batches replay", collected.every((batch) => batch.replay));

  client.close();
}

// --- requests: ack / port value / timeout -----------------------------------

section("setInput: ack correlation");

{
  const { client, socket } = await connectedGraph();

  const pending = client.setInput("bpm", 123.0);

  check("one frame was sent", socket.sent.length, 1);

  const requestId = requestIdOf(socket.sent[0]!, 4);

  check("request ids start at 1 and are bigint", requestId, 1n);
  check("set_input travels under ClientMessage field 4", socket.sent[0]![0], 0x22);

  socket.emitMessage(ackFrame({ requestId, appliedEpoch: 7n }));

  check("setInput resolves the applied epoch", await pending, { requestId, appliedEpoch: 7n });
  check("pending request map drains", client.pendingRequestCount, 0);
  check("no orphan acks", client.orphanAcks, 0);

  // Error ack: the wire text is surfaced on a structured AckError.
  const failing = client.setInput("nope", 1.0);
  const failingId = requestIdOf(socket.sent[1]!, 4);

  socket.emitMessage(ackFrame({ requestId: failingId, error: 'unknown input port "nope"' }));

  const error = await rejects("setInput rejects an error ack", failing, "AckError", (typed) =>
    (typed as AckError).ackError === 'unknown input port "nope"',
  );

  ok("AckError names the failed port", error instanceof AckError && error.port === "nope", error?.message);

  client.close();
}

section("requests: timeout and not-connected");

{
  const factory = new FakeWebSocketFactory();
  const client = createClient("ws://fake.test/ws", {
    webSocketFactory: factory.factory,
    reconnect: false,
    requestTimeoutMs: 40,
  });

  await rejects(
    "requests reject while not connected",
    client.setInput("bpm", 1.0),
    "NotConnectedError",
  );

  client.close();

  const configured = new FakeWebSocketFactory();
  const timeoutClient = createClient("ws://fake.test/ws", {
    webSocketFactory: configured.factory,
    reconnect: false,
    requestTimeoutMs: 40,
  });
  const connecting = timeoutClient.connect();

  configured.last.emitOpen();
  configured.last.emitMessage(graphFrame({ name: "demo" }));
  await connecting;

  await rejects(
    "a request with no reply rejects with RequestTimeoutError",
    timeoutClient.setInput("bpm", 1.0),
    "RequestTimeoutError",
    (typed) => (typed as RequestTimeoutError).requestId > 0n,
  );
  check("timed-out request is removed from the pending map", timeoutClient.pendingRequestCount, 0);

  timeoutClient.close();
  await rejects("requests after close reject with ClientClosedError", timeoutClient.getInput("bpm"), "ClientClosedError");
}

// --- reads -------------------------------------------------------------------

section("getOutput: PortValue correlation");

{
  const { client, socket } = await connectedGraph();

  const reading = client.getOutput("tempo");
  const requestId = requestIdOf(socket.sent[0]!, 6);

  socket.emitMessage(portValueFrame({ requestId, port: "tempo", value: { kind: "double", value: 2.5 } }));

  check("getOutput resolves the PortValue", await reading, {
    requestId,
    port: "tempo",
    value: { kind: "double", value: 2.5 },
  });

  const failing = client.getInput("bpm");
  const failingId = requestIdOf(socket.sent[1]!, 5);

  socket.emitMessage(portValueFrame({ requestId: failingId, port: "bpm", error: "no snapshot" }));

  await rejects("getInput rejects a PortValue error", failing, "AckError", (typed) =>
    (typed as AckError).ackError === "no snapshot",
  );

  client.close();
}

// --- drop accounting ---------------------------------------------------------

section("dropped accounting: EventBatch + CommandStats");

{
  const { client, socket } = await connectedGraph();

  const stats: Array<Record<string, bigint>> = [];

  client.onStats((entry) => stats.push(entry.events));

  socket.emitMessage(batchFrame({
    clock: "audio",
    dropped: 3n,
    events: [{ sequence: 1n, stampNs: 1000n, kind: EVENT_KIND.REGION_EVAL_END, clock: "audio" }],
  }));
  socket.emitMessage(batchFrame({ clock: "audio", dropped: 2n }));
  socket.emitMessage(commandStatsFrame(4n));
  socket.emitMessage(commandStatsFrame(9n));

  await waitFor("drop accounting frames are ingested", () => client.dropped.commands === 9n);

  check("event drops are summed per clock", client.dropped.events, { audio: 5n });
  check("command drops are the server's cumulative count (not summed)", client.dropped.commands, 9n);
  ok("onStats fires for dropped batches", stats.length >= 2, stats);
  ok("drop totals are bigint", Object.values(client.dropped.events).every((count) => typeof count === "bigint"));

  client.close();
}

// --- reconnect ---------------------------------------------------------------

section("reconnect: replay dedupe, pending request survival, sequence frontier");

{
  const factory = new FakeWebSocketFactory();
  const client = createClient("ws://fake.test/ws", {
    webSocketFactory: factory.factory,
    requestTimeoutMs: 500,
    reconnectBaseDelayMs: 10,
    reconnectMaxDelayMs: 20,
    replaySettleMs: 20,
  });
  const connectionStates: string[] = [];

  client.onConnection((info) => connectionStates.push(info.state));

  const connecting = client.connect();
  const first = factory.last;

  first.emitOpen();
  first.emitMessage(graphFrame({ name: "demo" }));
  await connecting;

  const stream = pumpEvents(client, { sinceSequence: { audio: 0n } });
  const batches = stream.batches;

  first.emitMessage(batchFrame({
    clock: "audio",
    events: [{ sequence: 1n, stampNs: 1000n, kind: EVENT_KIND.NODE_EVAL_END, clock: "audio" }],
  }));

  await waitFor("first live event arrives", () => batches.length === 1);

  // Issue a request, then lose the socket before its ack arrives.
  const pending = client.setInput("bpm", 5.0);
  const requestId = requestIdOf(first.sent[0]!, 4);

  first.emitClose(1006, "gone");

  await waitFor("client opens a second socket after an unexpected close", () => factory.sockets.length === 2, {
    timeoutMs: 1000,
  });

  const second = factory.last;

  second.emitOpen();
  second.emitMessage(graphFrame({ name: "demo" }));
  second.emitMessage(batchFrame({
    clock: "",
    events: [{ sequence: 1n, stampNs: 1000n, kind: EVENT_KIND.NODE_EVAL_END, clock: "audio" }], // replay duplicate
  }));
  second.emitMessage(batchFrame({
    clock: "",
    events: [{ sequence: 2n, stampNs: 2000n, kind: EVENT_KIND.NODE_EVAL_END, clock: "audio" }], // missed while away
  }));

  await waitFor("missed replay event arrives exactly once", () => batches.length === 2, { timeoutMs: 1000 });

  check("reconnect replays the missed tail without duplicating the old one", sequencesOf(batches), [1n, 2n]);
  check(
    "connection states reported",
    connectionStates,
    ["open", "reconnecting", "open"],
  );

  // The command was enqueued before the socket died; its ack can arrive on the
  // new connection and must resolve the original promise.
  second.emitMessage(ackFrame({ requestId, appliedEpoch: 3n }));
  check("pending request survives reconnect", await pending, { requestId, appliedEpoch: 3n });

  await stream.stop();
  client.close();

  // No further reconnect timers after an explicit close.
  await sleep(40);
  check("close() stops reconnecting", factory.sockets.length, 2);
}

section("reconnect: a restarted server resets the sequence frontier");

{
  const factory = new FakeWebSocketFactory();
  const client = createClient("ws://fake.test/ws", {
    webSocketFactory: factory.factory,
    reconnectBaseDelayMs: 10,
    reconnectMaxDelayMs: 20,
    replaySettleMs: 20,
    requestTimeoutMs: 200,
  });

  const connecting = client.connect();
  const first = factory.last;

  first.emitOpen();
  first.emitMessage(graphFrame({ name: "demo" }));
  await connecting;

  const stream = pumpEvents(client, { sinceSequence: { audio: 0n } });
  const batches = stream.batches;

  first.emitMessage(batchFrame({
    clock: "audio",
    events: [{ sequence: 1n, stampNs: 1000n, kind: EVENT_KIND.NODE_EVAL_END, clock: "audio" }],
  }));
  await waitFor("event before restart", () => batches.length === 1);

  first.emitClose(1006, "server restarting");
  await waitFor("reconnects", () => factory.sockets.length === 2, { timeoutMs: 1000 });

  const second = factory.last;

  second.emitOpen();
  second.emitMessage(graphFrame({ name: "demo" }));
  // Same example name, same sequence numbers, but a fresh session: the
  // stamp_ns differs, which is how the client tells a replay from a restart.
  second.emitMessage(batchFrame({
    clock: "audio",
    events: [{ sequence: 1n, stampNs: 50_000n, kind: EVENT_KIND.NODE_EVAL_END, clock: "audio" }],
  }));
  second.emitMessage(batchFrame({
    clock: "audio",
    events: [{ sequence: 2n, stampNs: 51_000n, kind: EVENT_KIND.NODE_EVAL_END, clock: "audio" }],
  }));

  await waitFor("restarted-server events are delivered", () => batches.length >= 3, { timeoutMs: 1000 });

  check("restart detection resets the frontier instead of filtering the new stream", sequencesOf(batches), [1n, 1n, 2n]);

  await stream.stop();
  client.close();
}

// --- examples / gate ---------------------------------------------------------

section("selectExample / pause / resume");

{
  const { client, socket } = await connectedGraph();

  const switching = client.selectExample("daw");

  check("select_example travels under ClientMessage field 3", socket.sent[0]![0], 0x1a);

  socket.emitMessage(graphFrame({ name: "daw" }));
  await switching;
  check("selectExample resolves on the new graph", client.graph?.name, "daw");
  check("selecting the current example is a no-op", client.currentExample, "daw");

  // A live iterator survives an example switch: its sequence cursor is reset
  // per engine, so the new example's events must not be filtered away.
  const stream = pumpEvents(client);

  socket.emitMessage(graphFrame({ name: "demo" }));
  socket.emitMessage(batchFrame({
    clock: "audio",
    events: [{ sequence: 1n, stampNs: 1000n, kind: EVENT_KIND.NODE_EVAL_END, clock: "audio" }],
  }));

  await waitFor("iterator survives example switches", () => stream.batches.length >= 1, { timeoutMs: 1000 });

  await stream.stop();

  await rejects(
    "selectExample rejects an unknown name from the cached list",
    connectedGraphForUnknown().then(async ({ client: unknownClient, socket: unknownSocket }) => {
      unknownSocket.emitMessage(examplesFrame(["demo", "daw"], "demo"));
      await waitFor("unknown client caches the example list", async () => (await unknownClient.examples()).names.length === 2, {
        timeoutMs: 500,
      });

      return unknownClient.selectExample("nope").finally(() => unknownClient.close());
    }),
    "DebugClientError",
    (typed) => (typed as DebugClientError).code === "unknown_example",
  );

  await client.pause();
  check("pause writes ClientMessage field 1", socket.sent[socket.sent.length - 1]![0], 0x0a);

  await client.resume();
  check("resume writes ClientMessage field 2", socket.sent[socket.sent.length - 1]![0], 0x12);

  client.close();
}

// A connected client whose examples frame the caller emits itself.
async function connectedGraphForUnknown(): Promise<{ client: DebugClient; socket: FakeWebSocket }> {
  const factory = new FakeWebSocketFactory();
  const client = createClient("ws://fake.test/ws", { webSocketFactory: factory.factory, reconnect: false });
  const connecting = client.connect();
  const socket = factory.last;

  socket.emitOpen();
  socket.emitMessage(graphFrame({ name: "demo" }));
  await connecting;

  return { client, socket };
}

// --- close -------------------------------------------------------------------

section("close()");

{
  const { client, socket } = await connectedGraph();

  const pending = client.setInput("bpm", 1.0);

  await sleep(1); // let the frame reach the fake socket
  client.close();

  await rejects("close rejects pending requests", pending, "ClientClosedError");

  const second = client.setInput("bpm", 2.0);

  await rejects("requests after close reject immediately", second, "ClientClosedError");
  check("socket is asked to close", socket.closed, true);
  check("close is idempotent", (client.close(), client.state), "closed");
}

finish("client.test.ts");
