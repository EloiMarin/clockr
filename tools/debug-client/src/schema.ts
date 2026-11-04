// The debug wire schema, decoded from the protobuf wire format.
//
// STAGE-COPIED from tools/web-debugger/web/schema.js, which stays the source of
// truth for the *shared* v1 messages (the page and this client must agree on
// every field number below). Two deliberate, documented divergences for the
// agent client:
//
//   1. 64-bit fields decode as `bigint` (the page decodes everything as
//      number; an agent must not silently truncate sequence numbers, epochs or
//      request ids). See `test/sync.test.ts`, which pins the shared messages
//      against both the .proto file and the web decoder.
//   2. field 2 of GraphDescription is named `crossings` here (the web page
//      calls it `sharingPoints`; the proto calls it `crossings`).
//
// The client-side *additions* (the v1 injection messages, and the encoder) are
// below the shared decoder, under a marked line; keep them in sync with
// proto/clockr_debug.proto. `WIRE_SCHEMA` pins every field number as data so
// the sync test can compare it with the .proto source mechanically.
//
// Dedupe follow-up: see the header of protobuf.ts.

import {
  Reader,
  decodeMessage,
  asSubReader,
  asString,
  toNumber,
} from "./protobuf.ts";

import {
  concatBytes,
  doubleField,
  messageField,
  sint64Field,
  stringField,
  varintField,
} from "./wire.ts";

import type {
  Ack,
  CommandStats,
  EventBatch,
  ExampleList,
  GraphDescription,
  PortValue,
  ServerFrame,
  StructValue,
  TraceEvent,
  TypeSchema,
  Value,
} from "./types.ts";

// ---------------------------------------------------------------------------
// Shared v1 decoder (stage-copied from web/schema.js)
// ---------------------------------------------------------------------------

// value.proto: message Value { oneof kind { unit=1; bool_value=2;
//                                            int_value=3; double_value=4;
//                                            struct_value=5; string_value=6 } }
//                message StructValue { uint32 type_id = 1;
//                                      repeated Value fields = 2 }
function decodeStructValue(root: MessageMap): StructValue {
  return {
    typeId: num(root[1]),
    fields: (root[2] !== undefined ? ([] as unknown[]).concat(root[2]) : []).map((s) =>
      decodeValue(decodeMessage(asSubReader(s as Slot))),
    ),
  };
}

export function decodeValue(root: MessageMap): Value {
  const kind = root[1] !== undefined ? "unit"
    : root[2] !== undefined ? "bool"
    : root[3] !== undefined ? "int"
    : root[4] !== undefined ? "double"
    : root[5] !== undefined ? "struct"
    : root[6] !== undefined ? "string"
    : "absent";

  if (kind === "unit") return { kind };
  if (kind === "bool") return { kind, value: Boolean(toNumber(root[2])) };
  // client divergence: int64 stays a bigint (the page returns a number).
  if (kind === "int") return { kind, value: zigzag64Big(root[3]) };
  if (kind === "double") {
    const slot = root[4];
    if (!slot || !slot.__fixed64) return { kind: "absent" };
    return { kind, value: slot.__fixed64.getFloat64(0, true) };
  }
  if (kind === "struct") return { kind, value: decodeStructValue(decodeMessage(asSubReader(root[5]))) };
  if (kind === "string") return { kind, value: asString(root[6]) };
  return { kind: "absent" };
}

// proto3 omits fields whose value is the type's default (0, "", false), so
// every scalar read must supply that default when the field is absent.
function num(value: unknown): number {
  return value === undefined ? 0 : Number(toNumber(value as bigint | number));
}

// client addition: uint64 fields decode as bigint (see header divergence 1).
function num64(value: bigint | number | undefined): bigint {
  return value === undefined ? 0n : BigInt(value);
}

// client addition: bigint-preserving zigzag64 (protobuf.ts's returns a number
// for the web page; Value.int_value is a signed 64-bit and must not truncate).
// The encoded value is unsigned; do NOT two's-complement it first, or every
// int64 outside [-2^63, 2^63) decodes wrong at the extremes.
function zigzag64Big(value: bigint | number): bigint {
  const n = BigInt(value);

  return (n >> 1n) ^ -(n & 1n);
}

// "repeated uint64 fan_in = 6" is encoded packed (one wire-2 field holding
// varint after varint) by proto3 serializers, but accept the unpacked form
// too so hand-built or non-conforming encoders still decode.
// client divergence: the packed run decodes straight to bigint.
function decodeFanIn(slot: Slot | undefined): bigint[] {
  if (slot === undefined) return [];

  const entries = ([] as Slot[]).concat(slot);
  const out: bigint[] = [];

  for (const entry of entries) {
    if (entry && entry.__sub) {
      const sub = entry.__sub;
      while (!sub.done) out.push(sub.varint());
    } else {
      out.push(BigInt(entry as bigint | number));
    }
  }

  return out;
}

function decodeNode(root: MessageMap) {
  return {
    id: num64(root[1]),
    kind: root[2] !== undefined ? asString(root[2]) : "",
    clock: root[3] !== undefined ? asString(root[3]) : "",
    label: root[4] !== undefined ? asString(root[4]) : "",
    port: root[5] !== undefined ? asString(root[5]) : "",
    fanIn: decodeFanIn(root[6]),
  };
}

function decodeCrossing(root: MessageMap) {
  return {
    from: num64(root[1]),
    to: num64(root[2]),
    policy: root[3] !== undefined ? asString(root[3]) : "",
  };
}

function decodePort(root: MessageMap) {
  return {
    nodeId: num64(root[1]),
    name: root[2] !== undefined ? asString(root[2]) : "",
  };
}

// FieldSchema { string name = 1; string kind = 2; uint32 type_id = 3 }
// TypeSchema { uint32 id = 1; string name = 2; repeated FieldSchema fields = 3 }
function decodeFieldSchema(root: MessageMap) {
  return {
    name: root[1] !== undefined ? asString(root[1]) : "",
    kind: root[2] !== undefined ? asString(root[2]) : "",
    typeId: num(root[3]),
  };
}

function decodeTypeSchema(root: MessageMap): TypeSchema {
  return {
    id: num(root[1]),
    name: root[2] !== undefined ? asString(root[2]) : "",
    fields: (root[3] !== undefined ? ([] as unknown[]).concat(root[3]) : []).map((s) =>
      decodeFieldSchema(decodeMessage(asSubReader(s as Slot)))),
  };
}

export function decodeGraphDescription(root: MessageMap): GraphDescription {
  return {
    nodes: (root[1] !== undefined ? ([] as unknown[]).concat(root[1]) : []).map((s) =>
      decodeNode(decodeMessage(asSubReader(s as Slot)))),
    // client divergence 2: the proto's name (`crossings`), not the page's.
    crossings: (root[2] !== undefined ? ([] as unknown[]).concat(root[2]) : []).map((s) =>
      decodeCrossing(decodeMessage(asSubReader(s as Slot)))),
    inputs: (root[3] !== undefined ? ([] as unknown[]).concat(root[3]) : []).map((s) =>
      decodePort(decodeMessage(asSubReader(s as Slot)))),
    outputs: (root[4] !== undefined ? ([] as unknown[]).concat(root[4]) : []).map((s) =>
      decodePort(decodeMessage(asSubReader(s as Slot)))),
    types: (root[5] !== undefined ? ([] as unknown[]).concat(root[5]) : []).map((s) =>
      decodeTypeSchema(decodeMessage(asSubReader(s as Slot)))),
    name: root[6] !== undefined ? asString(root[6]) : "",
  };
}

const EVENT_KINDS = [
  "region_step_begin",
  "region_step_end",
  "node_tick_begin",
  "node_tick_end",
  "node_tick_throw",
];

function decodeTraceEvent(root: MessageMap): TraceEvent {
  return {
    sequence: num64(root[1]),
    stampNs: num64(root[2]),
    kind: (EVENT_KINDS[num(root[3])] ?? "unknown") as TraceEvent["kind"],
    nodeId: num64(root[4]),
    clock: root[5] !== undefined ? asString(root[5]) : "",
    epoch: num64(root[6]),
    value: root[7] !== undefined ? decodeValue(decodeMessage(asSubReader(root[7]))) : null,
    threw: root[8] !== undefined ? Boolean(toNumber(root[8])) : false,
    durationNs: num64(root[9]),
  };
}

export function decodeEventBatch(root: MessageMap): EventBatch {
  return {
    clock: root[1] !== undefined ? asString(root[1]) : "",
    dropped: num64(root[2]),
    events: (root[3] !== undefined ? ([] as unknown[]).concat(root[3]) : []).map((s) =>
      decodeTraceEvent(decodeMessage(asSubReader(s as Slot)))),
  };
}

// ExampleList { repeated string names = 1; string active = 2 } — the set of
// live examples and which one is selected; the page rebuilds its selector.
export function decodeExampleList(root: MessageMap): ExampleList {
  return {
    names: (root[1] !== undefined ? ([] as unknown[]).concat(root[1]) : []).map((s) => asString(s as Slot)),
    active: root[2] !== undefined ? asString(root[2]) : "",
  };
}

// ---------------------------------------------------------------------------
// Client v1 additions (not in web/schema.js): injection messages + encoder.
// Field numbers are frozen in the task contract:
//
//   SetInput    { uint64 request_id = 1; string port = 2; Value value = 3;
//                 optional string example = 4; }
//   ReadInput   { uint64 request_id = 1; string port = 2; optional string example = 3; }
//   ReadOutput  { uint64 request_id = 1; string port = 2; optional string example = 3; }
//   Ack         { uint64 request_id = 1; string error = 2; uint64 applied_epoch = 3; }
//   PortValue   { uint64 request_id = 1; string port = 2; Value value = 3; string error = 4; }
//   CommandStats{ uint64 dropped = 1; }
//   ClientMessage oneof: pause=1, resume=2, select_example=3,
//                       set_input=4, read_input=5, read_output=6
//   ServerMessage oneof: graph=1, batch=2, examples=3,
//                       ack=4, port_value=5, command_stats=6
// ---------------------------------------------------------------------------

/** Ack { request_id = 1, error = 2, applied_epoch = 3 } */
export function decodeAck(root: MessageMap): Ack {
  return {
    requestId: num64(root[1]),
    error: root[2] !== undefined ? asString(root[2]) : "",
    appliedEpoch: num64(root[3]),
  };
}

/** PortValue { request_id = 1, port = 2, value = 3, error = 4 } */
export function decodePortValue(root: MessageMap): PortValue {
  return {
    requestId: num64(root[1]),
    port: root[2] !== undefined ? asString(root[2]) : "",
    value: root[3] !== undefined ? decodeValue(decodeMessage(asSubReader(root[3]))) : { kind: "absent" },
    error: root[4] !== undefined ? asString(root[4]) : "",
  };
}

/** CommandStats { dropped = 1 } */
export function decodeCommandStats(root: MessageMap): CommandStats {
  return { dropped: num64(root[1]) };
}

// The entry point: decode one binary WebSocket frame into one of
// { graph } / { batch } / { examples } / { ack } / { portValue } /
// { commandStats } / { unknown }. Unknown bodies are ignored by the client, so
// a newer server never breaks an older client (the schema grows additively).
export function decodeFrame(bytes: Uint8Array): ServerFrame {
  const root = decodeMessage(new Reader(bytes));

  if (root[1] !== undefined) return { graph: decodeGraphDescription(decodeMessage(asSubReader(root[1]))) };
  if (root[2] !== undefined) return { batch: decodeEventBatch(decodeMessage(asSubReader(root[2]))) };
  if (root[3] !== undefined) return { examples: decodeExampleList(decodeMessage(asSubReader(root[3]))) };
  if (root[4] !== undefined) return { ack: decodeAck(decodeMessage(asSubReader(root[4]))) };
  if (root[5] !== undefined) return { portValue: decodePortValue(decodeMessage(asSubReader(root[5]))) };
  if (root[6] !== undefined) return { commandStats: decodeCommandStats(decodeMessage(asSubReader(root[6]))) };

  return { unknown: true };
}

// ClientMessage { oneof body { pause = 1; resume = 2; select_example = 3;
//                              set_input = 4; read_input = 5; read_output = 6 } }
//
// Encoding side. Every oneof member travels as a length-delimited field; names
// and ports are short, so lengths stay one or two bytes. The wire bytes for
// pause/resume/select_example are identical to the web page's encoders (pinned
// by web/schema_smoke_test.js and by test/codec.test.ts here).

export function encodePause(): Uint8Array {
  return new Uint8Array([0x0a, 0x00]); // field 1 (Pause), empty message
}

export function encodeResume(): Uint8Array {
  return new Uint8Array([0x12, 0x00]); // field 2 (Resume), empty message
}

export function encodeSelectExample(name: string): Uint8Array {
  const bytes = new TextEncoder().encode(name);
  const inner = [0x0a, bytes.length, ...bytes]; // field 1 (name), string
  return new Uint8Array([0x1a, inner.length, ...inner]); // field 3 (SelectExample)
}

/** Encode one client-side `Value` (a oneof member is always emitted). */
export function encodeValue(value: Value): number[] {
  switch (value.kind) {
    case "unit":
      return varintField(1, true);
    case "bool":
      return varintField(2, value.value);
    case "int":
      return sint64Field(3, value.value);
    case "double":
      return doubleField(4, value.value);
    case "struct": {
      const body = [
        ...varintField(1, value.value.typeId, { omitDefault: true }),
        ...value.value.fields.flatMap((field) => messageField(2, encodeValue(field))),
      ];

      return messageField(5, body);
    }
    case "string":
      return stringField(6, value.value);
    case "absent":
      return []; // an empty Value: the oneof is unset
    default:
      throw new TypeError(`encodeValue: unsupported Value kind ${JSON.stringify((value as { kind: string }).kind)}`);
  }
}

export interface SetInputMessage {
  requestId: bigint;
  port: string;
  value: Value;
  /** Absent => the server's active example. */
  example?: string;
}

export interface ReadMessage {
  requestId: bigint;
  port: string;
  /** Absent => the server's active example. */
  example?: string;
}

function portField(port: string): number[] {
  return port === "" ? [] : stringField(2, port);
}

function exampleField(example: string | undefined): number[] {
  return example === undefined || example === "" ? [] : stringField(3, example);
}

/** Encode one full `ClientMessage` frame. */
export function encodeSetInput(message: SetInputMessage): Uint8Array {
  const body = [
    ...varintField(1, message.requestId, { omitDefault: true }),
    ...portField(message.port),
    ...messageField(3, encodeValue(message.value)),
    ...(message.example === undefined || message.example === "" ? [] : stringField(4, message.example)),
  ];

  return concatBytes([messageField(4, body)]); // ClientMessage.set_input = 4
}

/** Encode one full `ClientMessage` frame. */
export function encodeReadInput(message: ReadMessage): Uint8Array {
  return concatBytes([messageField(5, readBody(message))]); // ClientMessage.read_input = 5
}

/** Encode one full `ClientMessage` frame. */
export function encodeReadOutput(message: ReadMessage): Uint8Array {
  return concatBytes([messageField(6, readBody(message))]); // ClientMessage.read_output = 6
}

// ReadInput/ReadOutput share a shape, including the optional example field.
function readBody(message: ReadMessage): number[] {
  return [
    ...varintField(1, message.requestId, { omitDefault: true }),
    ...portField(message.port),
    ...exampleField(message.example),
  ];
}

// ---------------------------------------------------------------------------
// The wire contract as data: every field number the codecs above use, pinned
// by test/sync.test.ts against proto/clockr_debug.proto and the web decoder.
// Add a message here when the proto grows one (the test fails on an unpinned
// message, which is the point).
// ---------------------------------------------------------------------------

export const WIRE_SCHEMA = {
  messages: {
    Value: { unit: 1, bool_value: 2, int_value: 3, double_value: 4, struct_value: 5, string_value: 6 },
    StructValue: { type_id: 1, fields: 2 },
    FieldSchema: { name: 1, kind: 2, type_id: 3 },
    TypeSchema: { id: 1, name: 2, fields: 3 },
    Node: { id: 1, kind: 2, clock: 3, label: 4, port: 5, fan_in: 6 },
    Crossing: { from: 1, to: 2, policy: 3 },
    Port: { node_id: 1, name: 2 },
    ExampleList: { names: 1, active: 2 },
    GraphDescription: { nodes: 1, crossings: 2, inputs: 3, outputs: 4, types: 5, name: 6 },
    TraceEvent: {
      sequence: 1,
      stamp_ns: 2,
      kind: 3,
      node_id: 4,
      clock: 5,
      epoch: 6,
      value: 7,
      threw: 8,
      duration_ns: 9,
    },
    EventBatch: { clock: 1, dropped: 2, events: 3 },
    ServerMessage: { graph: 1, batch: 2, examples: 3, ack: 4, port_value: 5, command_stats: 6 },
    ClientMessage: { pause: 1, resume: 2, select_example: 3, set_input: 4, read_input: 5, read_output: 6 },
    Pause: {},
    Resume: {},
    SelectExample: { name: 1 },
    SetInput: { request_id: 1, port: 2, value: 3, example: 4 },
    ReadInput: { request_id: 1, port: 2, example: 3 },
    ReadOutput: { request_id: 1, port: 2, example: 3 },
    Ack: { request_id: 1, error: 2, applied_epoch: 3 },
    PortValue: { request_id: 1, port: 2, value: 3, error: 4 },
    CommandStats: { dropped: 1 },
  },
  enums: {
    EventKind: {
      REGION_EVAL_BEGIN: 0,
      REGION_EVAL_END: 1,
      NODE_EVAL_BEGIN: 2,
      NODE_EVAL_END: 3,
      NODE_EVAL_THROW: 4,
    },
  },
} as const;

/** The v1 additions this client is the first consumer of (landed). */
export const CLIENT_V1_ADDITIONS = [
  "SetInput",
  "ReadInput",
  "ReadOutput",
  "Ack",
  "PortValue",
  "CommandStats",
] as const;

// Internal wire-tag plumbing: decodeMessage yields { fieldNumber: slot } where
// a length-delimited slot is { __sub: Reader } and a varint slot is bigint.
type Slot = { __sub?: Reader } & Record<string, unknown>;
type MessageMap = Record<number, Slot | bigint | number | undefined>;
