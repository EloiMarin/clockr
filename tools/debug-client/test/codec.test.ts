// Codec unit tests: the client's hand-rolled protobuf decode/encode path.
//
// These are pure wire-format checks (no socket, no server). They pin:
//   - the shared v1 server messages, decoded from hand-built frames,
//   - the frozen v1 injection additions (field numbers + encodings),
//   - 64-bit values as bigint (sequence/epoch/request_id/int_value),
//   - byte-exact client frames (so the C++ server's ParseFromArray sees what
//     proto/clockr_debug.proto promises).
//
// Run: node test/codec.test.ts

import { Reader, asString, decodeMessage } from "../src/protobuf.ts";
import {
  CLIENT_V1_ADDITIONS,
  WIRE_SCHEMA,
  decodeFrame,
  decodeValue,
  encodePause,
  encodeReadInput,
  encodeReadOutput,
  encodeResume,
  encodeSelectExample,
  encodeSetInput,
  encodeValue,
} from "../src/schema.ts";

import { bigintToJson, toValue, valueToJson } from "../src/value.ts";
import { tag } from "../src/wire.ts";

import type { Value } from "../src/types.ts";

import {
  EVENT_KIND,
  ackFrame,
  batchFrame,
  commandStatsFrame,
  examplesFrame,
  graphFrame,
  portValueFrame,
} from "./support/frames.ts";
import { check, finish, ok, rejects, section } from "./support/harness.ts";

// --- helpers -----------------------------------------------------------------

function hex(bytes: Uint8Array | number[]): string {
  return [...bytes].map((byte) => byte.toString(16).padStart(2, "0")).join(" ");
}

function doubleBytes(value: number): number[] {
  const bytes = new Uint8Array(8);

  new DataView(bytes.buffer).setFloat64(0, value, true);

  return [...bytes];
}

// --- shared server messages --------------------------------------------------

section("server frames: graph");

const graphBytes = graphFrame({
  name: "demo",
  nodes: [
    { id: 0, kind: "input", clock: "ui", label: "input", port: "bpm" },
    { id: 1, kind: "internal", clock: "audio", label: "lift", fanIn: [0, 2] },
    { id: 2, kind: "output", clock: "audio", label: "output", port: "tempo" },
  ],
  crossings: [{ from: 0, to: 1, policy: "latest" }],
  inputs: [{ nodeId: 0, name: "bpm" }],
  outputs: [{ nodeId: 2, name: "tempo" }],
});

{
  const frame = decodeFrame(graphBytes);

  ok("graph frame detected", "graph" in frame);

  if (!("graph" in frame)) throw new Error("unreachable");

  const graph = frame.graph;

  check("graph name", graph.name, "demo");
  check("node count", graph.nodes.length, 3);
  check("node ids are bigint", graph.nodes.map((node) => [node.id, typeof node.id]), [
    [0n, "bigint"],
    [1n, "bigint"],
    [2n, "bigint"],
  ]);
  check("node 0 port", graph.nodes[0]?.port, "bpm");
  check("node 1 fan-in is bigint[] (packed)", graph.nodes[1]?.fanIn, [0n, 2n]);
  check("crossing from/to are bigint", [graph.crossings[0]?.from, graph.crossings[0]?.to], [0n, 1n]);
  check("crossing policy", graph.crossings[0]?.policy, "latest");
  check("input node id bigint", graph.inputs[0]?.nodeId, 0n);
  check("output name", graph.outputs[0]?.name, "tempo");
  check("graph types default empty", graph.types, []);
}

// An unpacked repeated fan-in (what a hand-rolled encoder might emit) must
// still decode: proto3 says packed, but robust readers accept both.
{
  const unpacked = graphFrame({ nodes: [{ id: 1, fanIn: [] }] });
  const raw = decodeMessage(new Reader(unpacked));
  check("unpacked graph fixture is a frame", typeof raw, "object");
}

section("server frames: event batch");

const batch = batchFrame({
  clock: "audio",
  dropped: 2n,
  events: [
    {
      sequence: 7n,
      stampNs: 1000n,
      kind: EVENT_KIND.NODE_EVAL_END,
      nodeId: 1n,
      clock: "audio",
      epoch: 4n,
      value: { kind: "double", value: 2.5 },
      durationNs: 123n,
    },
    {
      sequence: 8n,
      stampNs: 2000n,
      kind: EVENT_KIND.REGION_EVAL_END,
      clock: "audio",
      epoch: 9n,
      threw: true,
      durationNs: 1_500_000n,
    },
  ],
});

{
  const frame = decodeFrame(batch);

  ok("batch frame detected", "batch" in frame);

  if (!("batch" in frame)) throw new Error("unreachable");

  const eventBatch = frame.batch;

  check("batch clock", eventBatch.clock, "audio");
  check("batch dropped is bigint", eventBatch.dropped, 2n);
  check("event kinds", eventBatch.events.map((event) => event.kind), ["node_tick_end", "region_step_end"]);
  check("event sequence bigint", eventBatch.events[0]?.sequence, 7n);
  check("event epoch bigint", eventBatch.events[0]?.epoch, 4n);
  check("event node bigint", eventBatch.events[0]?.nodeId, 1n);
  check("event value double", eventBatch.events[0]?.value, { kind: "double", value: 2.5 });
  check("event duration bigint", eventBatch.events[0]?.durationNs, 123n);
  check("event threw", eventBatch.events[1]?.threw, true);
  check("absent value is null", eventBatch.events[1]?.value, null);
}

section("server frames: kernel Value variants");

function batchWithValue(value: Value): ReturnType<typeof decodeFrame> {
  const bytes = batchFrame({
    clock: "ui",
    events: [{ sequence: 1n, kind: EVENT_KIND.NODE_EVAL_END, clock: "ui", value }],
  });

  return decodeFrame(bytes);
}

function decodedValue(value: Value): Value | null {
  const frame = batchWithValue(value);

  return "batch" in frame ? (frame.batch.events[0]?.value ?? null) : null;
}

check("value unit", decodedValue({ kind: "unit" }), { kind: "unit" });
check("value bool true", decodedValue({ kind: "bool", value: true }), { kind: "bool", value: true });
check("value bool false (presence in oneof)", decodedValue({ kind: "bool", value: false }), {
  kind: "bool",
  value: false,
});
check("value int negative bigint", decodedValue({ kind: "int", value: -42n }), { kind: "int", value: -42n });
check(
  "value int beyond Number.MAX_SAFE_INTEGER",
  decodedValue({ kind: "int", value: 9_007_199_254_740_993n }),
  { kind: "int", value: 9_007_199_254_740_993n },
);
check("value string", decodedValue({ kind: "string", value: "saw" }), { kind: "string", value: "saw" });
check(
  "value struct",
  decodedValue({
    kind: "struct",
    value: {
      typeId: 7,
      fields: [
        { kind: "double", value: 1 },
        { kind: "int", value: 4n },
      ],
    },
  }),
  {
    kind: "struct",
    value: {
      typeId: 7,
      fields: [
        { kind: "double", value: 1 },
        { kind: "int", value: 4n },
      ],
    },
  },
);
check("anonymous struct", decodedValue({ kind: "struct", value: { typeId: 0, fields: [] } }), {
  kind: "struct",
  value: { typeId: 0, fields: [] },
});

// An empty Value message (oneof unset) renders as absent.
{
  const frame = decodeFrame(batchFrame({
    clock: "ui",
    events: [{ sequence: 1n, kind: EVENT_KIND.NODE_EVAL_END, clock: "ui", value: { kind: "absent" } }],
  }));

  ok("empty-value frame is a batch", "batch" in frame);
  check("empty Value decodes as absent", "batch" in frame ? frame.batch.events[0]?.value : null, { kind: "absent" });
}

section("server frames: examples / ack / port value / command stats");

{
  const frame = decodeFrame(examplesFrame(["demo", "daw"], "daw"));

  check("examples frame", "examples" in frame ? frame.examples : null, { names: ["demo", "daw"], active: "daw" });
}

{
  const frame = decodeFrame(ackFrame({ requestId: 42n, appliedEpoch: 7n }));

  check("ack frame", "ack" in frame ? frame.ack : null, { requestId: 42n, error: "", appliedEpoch: 7n });
}

{
  const frame = decodeFrame(ackFrame({ requestId: 1n, error: "unknown port \"nope\"" }));

  check("error ack frame", "ack" in frame ? frame.ack : null, {
    requestId: 1n,
    error: 'unknown port "nope"',
    appliedEpoch: 0n,
  });
}

{
  const frame = decodeFrame(portValueFrame({ requestId: 3n, port: "bpm", value: { kind: "double", value: 120 } }));

  check("port value frame", "portValue" in frame ? frame.portValue : null, {
    requestId: 3n,
    port: "bpm",
    value: { kind: "double", value: 120 },
    error: "",
  });
}

{
  const frame = decodeFrame(portValueFrame({ requestId: 4n, port: "gain", error: "no such port" }));

  ok("port value error frame", "portValue" in frame && frame.portValue.error === "no such port");
}

{
  const frame = decodeFrame(commandStatsFrame(5n));

  check("command stats frame", "commandStats" in frame ? frame.commandStats : null, { dropped: 5n });
}

// A body number this client does not know (a future additive message) is
// ignored, not a decode failure.
{
  const unknown = new Uint8Array([...tag(99, 2), 0x00]);
  const frame = decodeFrame(unknown);

  check("unknown server body is ignored", frame, { unknown: true });
}

await rejects(
  "truncated frame throws",
  Promise.resolve().then(() => decodeFrame(new Uint8Array([0x0a]))),
  "Error",
);

// --- client message encoders -------------------------------------------------

section("client frames: pause / resume / select_example (byte parity with web/schema.js)");

check("pause bytes", [...encodePause()], [0x0a, 0x00]);
check("resume bytes", [...encodeResume()], [0x12, 0x00]);
check("select_example('daw') bytes", [...encodeSelectExample("daw")], [0x1a, 0x05, 0x0a, 0x03, 0x64, 0x61, 0x77]);
check("select_example('yang-b') bytes", [...encodeSelectExample("yang-b")], [
  0x1a,
  0x08,
  0x0a,
  0x06,
  ...Buffer.from("yang-b", "utf8"),
]);

section("client frames: set_input");

{
  const frame = encodeSetInput({ requestId: 1n, port: "bpm", value: { kind: "double", value: 123.0 } });

  // ClientMessage.set_input = 4: [0x22, len]
  //   SetInput { request_id = 1, port = 2, value = 3 }
  //     request_id: 0x08 0x01
  //     port:       0x12 0x03 "bpm"
  //     value:      0x1a 0x09 [Value.double_value = 4: 0x21 + 8 bytes]
  check(
    "set_input double bytes",
    hex(frame),
    hex([0x22, 0x12, 0x08, 0x01, 0x12, 0x03, 0x62, 0x70, 0x6d, 0x1a, 0x09, 0x21, ...doubleBytes(123.0)]),
  );
}

{
  const withExample = encodeSetInput({
    requestId: 2n,
    port: "gain",
    value: { kind: "bool", value: false },
    example: "daw",
  });
  const raw = decodeMessage(new Reader(withExample));
  const slot = raw[4] as { __sub: Reader };

  ok("set_input frame nests under field 4", slot !== undefined && slot.__sub !== undefined);

  const body = decodeMessage(slot.__sub);
  const exampleSlot = body[4] as { __sub: Reader };

  ok("optional example travels when present", exampleSlot !== undefined);
}

{
  const noExample = encodeSetInput({ requestId: 3n, port: "gain", value: { kind: "unit" } });
  const body = decodeMessage((decodeMessage(new Reader(noExample))[4] as { __sub: Reader }).__sub);

  ok("optional example is absent by default", body[4] === undefined);
}

section("client frames: read_input / read_output");

{
  const input = encodeReadInput({ requestId: 9n, port: "bpm" });
  const output = encodeReadOutput({ requestId: 10n, port: "tempo" });

  check("read_input oneof field is 5", input[0], 0x2a);
  check("read_output oneof field is 6", output[0], 0x32);

  const body = decodeMessage((decodeMessage(new Reader(input))[5] as { __sub: Reader }).__sub);

  check("read_input request id", body[1], 9n);
  check("read_input port decodes", asString(body[2] as never), "bpm");
}

section("client frames: Value encodings");

check("encode unit", encodeValue({ kind: "unit" }), [0x08, 0x01]);
check("encode bool true", encodeValue({ kind: "bool", value: true }), [0x10, 0x01]);
check("encode bool false", encodeValue({ kind: "bool", value: false }), [0x10, 0x00]);
check("encode int -1 (zigzag)", encodeValue({ kind: "int", value: -1n }), [0x18, 0x01]);
check("encode int 1 (zigzag)", encodeValue({ kind: "int", value: 1n }), [0x18, 0x02]);
check("encode double 2.5", encodeValue({ kind: "double", value: 2.5 }), [0x21, ...doubleBytes(2.5)]);
check(
  "encode struct",
  hex(encodeValue({ kind: "struct", value: { typeId: 7, fields: [{ kind: "double", value: 1 }] } })),
  // StructValue { type_id = 1 (uint32), fields = 2 (repeated Value) }
  hex([0x2a, 0x0d, 0x08, 0x07, 0x12, 0x09, 0x21, ...doubleBytes(1)]),
);

// encodeValue -> wire -> decodeValue round-trip, including the empty/absent case.
for (const value of [
  { kind: "unit" },
  { kind: "bool", value: true },
  { kind: "bool", value: false },
  { kind: "int", value: -(2n ** 63n) },
  { kind: "int", value: 2n ** 63n - 1n },
  { kind: "double", value: Number.MIN_VALUE },
  { kind: "string", value: "" },
] as Value[]) {
  const bytes = new Uint8Array(encodeValue(value));
  const decoded = decodeValue(decodeMessage(new Reader(bytes)));

  check(`Value round-trip ${value.kind}${value.kind === "int" ? ` ${String(value.value)}` : ""}`, decoded, value);
}

// --- frozen v1 field numbers (also pinned against the .proto in sync.test.ts) --

section("frozen v1 additions");

check("SetInput fields", WIRE_SCHEMA.messages.SetInput, { request_id: 1, port: 2, value: 3, example: 4 });
check("ReadInput fields", WIRE_SCHEMA.messages.ReadInput, { request_id: 1, port: 2, example: 3 });
check("ReadOutput fields", WIRE_SCHEMA.messages.ReadOutput, { request_id: 1, port: 2, example: 3 });
check("Ack fields", WIRE_SCHEMA.messages.Ack, { request_id: 1, error: 2, applied_epoch: 3 });
check("PortValue fields", WIRE_SCHEMA.messages.PortValue, { request_id: 1, port: 2, value: 3, error: 4 });
check("CommandStats fields", WIRE_SCHEMA.messages.CommandStats, { dropped: 1 });
check("ClientMessage oneof", WIRE_SCHEMA.messages.ClientMessage, {
  pause: 1,
  resume: 2,
  select_example: 3,
  set_input: 4,
  read_input: 5,
  read_output: 6,
});
check("ServerMessage oneof", WIRE_SCHEMA.messages.ServerMessage, {
  graph: 1,
  batch: 2,
  examples: 3,
  ack: 4,
  port_value: 5,
  command_stats: 6,
});
check("client v1 additions list", [...CLIENT_V1_ADDITIONS], [
  "SetInput",
  "ReadInput",
  "ReadOutput",
  "Ack",
  "PortValue",
  "CommandStats",
]);

// --- value sugar -------------------------------------------------------------

section("Value sugar and JSON conversion");

check("number sugar is a double", toValue(120.0), { kind: "double", value: 120 });
check("bigint sugar is an int", toValue(7n), { kind: "int", value: 7n });
check("boolean sugar", toValue(true), { kind: "bool", value: true });
check("string sugar", toValue("midi"), { kind: "string", value: "midi" });
check("null sugar is unit", toValue(null), { kind: "unit" });
check("int from a safe integer in a mapping", toValue({ kind: "int", value: 42 }), { kind: "int", value: 42n });
check("valueToJson keeps struct shape", valueToJson({
  kind: "struct",
  value: { typeId: 7, fields: [{ kind: "int", value: 3n }] },
}), { kind: "struct", value: { typeId: 7, fields: [{ kind: "int", value: 3 }] } });
check("bigintToJson lossless number", bigintToJson(42n), 42);
check("bigintToJson unsafe becomes string", bigintToJson(9_007_199_254_740_993n), "9007199254740993");

{
  let threw = false;

  try {
    toValue({ kind: "nope" } as unknown as Value);
  } catch (error) {
    threw = (error as Error).name === "TypeError";
  }

  ok("invalid Value kind throws TypeError", threw);
}

finish("codec.test.ts");
