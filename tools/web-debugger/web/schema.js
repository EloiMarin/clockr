// The debug wire schema, decoded from the protobuf wire format.
//
// Field numbers here mirror proto/clockr_debug.proto exactly; keep them in
// sync when the schema changes. This is the JS side of the same contract the
// C++ codec (server/event_message.cpp, server/graph_message.cpp) implements.

import {
  Reader,
  decodeMessage,
  asSubReader,
  asString,
  zigzag64,
  toNumber,
  decodePackedVarints,
} from "./protobuf.js";

// value.proto: message Value { oneof kind { unit=1; bool_value=2;
//                                            int_value=3; double_value=4;
//                                            struct_value=5; string_value=6 } }
//                message StructValue { uint32 type_id = 1;
//                                      repeated Value fields = 2 }
function decodeStructValue(root) {
  return {
    typeId: num(root[1]),
    fields: (root[2] !== undefined ? [].concat(root[2]) : []).map((s) =>
      decodeValue(decodeMessage(asSubReader(s)))),
  };
}

function decodeValue(root) {
  const kind = root[1] !== undefined ? "unit"
    : root[2] !== undefined ? "bool"
    : root[3] !== undefined ? "int"
    : root[4] !== undefined ? "double"
    : root[5] !== undefined ? "struct"
    : root[6] !== undefined ? "string"
    : "absent";

  if (kind === "unit") return { kind };
  if (kind === "bool") return { kind, value: Boolean(toNumber(root[2])) };
  if (kind === "int") return { kind, value: zigzag64(toNumber(root[3])) };
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
function num(value) {
  return value === undefined ? 0 : Number(toNumber(value));
}

// "repeated uint64 fan_in = 6" is encoded packed (one wire-2 field holding
// varint after varint) by proto3 serializers, but accept the unpacked form
// too so hand-built or non-conforming encoders still decode.
function decodeFanIn(slot) {
  if (slot === undefined) return [];

  const entries = [].concat(slot);
  const out = [];

  for (const entry of entries) {
    if (entry && entry.__sub) {
      out.push(...decodePackedVarints(entry));
    } else {
      out.push(Number(toNumber(entry)));
    }
  }

  return out;
}

function decodeNode(root) {
  return {
    id: num(root[1]),
    kind: root[2] !== undefined ? asString(root[2]) : "",
    clock: root[3] !== undefined ? asString(root[3]) : "",
    label: root[4] !== undefined ? asString(root[4]) : "",
    port: root[5] !== undefined ? asString(root[5]) : "",
    fanIn: decodeFanIn(root[6]),
  };
}

function decodeCrossing(root) {
  return {
    from: num(root[1]),
    to: num(root[2]),
    policy: root[3] !== undefined ? asString(root[3]) : "",
  };
}

function decodePort(root) {
  return {
    nodeId: num(root[1]),
    name: root[2] !== undefined ? asString(root[2]) : "",
  };
}

// FieldSchema { string name = 1; string kind = 2; uint32 type_id = 3 }
// TypeSchema { uint32 id = 1; string name = 2; repeated FieldSchema fields = 3 }
function decodeFieldSchema(root) {
  return {
    name: root[1] !== undefined ? asString(root[1]) : "",
    kind: root[2] !== undefined ? asString(root[2]) : "",
    typeId: num(root[3]),
  };
}

function decodeTypeSchema(root) {
  return {
    id: num(root[1]),
    name: root[2] !== undefined ? asString(root[2]) : "",
    fields: (root[3] !== undefined ? [].concat(root[3]) : []).map((s) =>
      decodeFieldSchema(decodeMessage(asSubReader(s)))),
  };
}

export function decodeGraphDescription(root) {
  return {
    nodes: (root[1] !== undefined ? [].concat(root[1]) : []).map((s) => decodeNode(decodeMessage(asSubReader(s)))),
    sharingPoints: (root[2] !== undefined ? [].concat(root[2]) : []).map((s) => decodeCrossing(decodeMessage(asSubReader(s)))),
    inputs: (root[3] !== undefined ? [].concat(root[3]) : []).map((s) => decodePort(decodeMessage(asSubReader(s)))),
    outputs: (root[4] !== undefined ? [].concat(root[4]) : []).map((s) => decodePort(decodeMessage(asSubReader(s)))),
    types: (root[5] !== undefined ? [].concat(root[5]) : []).map((s) => decodeTypeSchema(decodeMessage(asSubReader(s)))),
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

function decodeTraceEvent(root) {
  return {
    sequence: num(root[1]),
    stampNs: num(root[2]),
    kind: EVENT_KINDS[num(root[3])] || "unknown",
    nodeId: num(root[4]),
    clock: root[5] !== undefined ? asString(root[5]) : "",
    epoch: num(root[6]),
    value: root[7] !== undefined ? decodeValue(decodeMessage(asSubReader(root[7]))) : null,
    threw: root[8] !== undefined ? Boolean(toNumber(root[8])) : false,
    durationNs: num(root[9]),
  };
}

export function decodeEventBatch(root) {
  return {
    clock: root[1] !== undefined ? asString(root[1]) : "",
    dropped: num(root[2]),
    events: (root[3] !== undefined ? [].concat(root[3]) : []).map((s) => decodeTraceEvent(decodeMessage(asSubReader(s)))),
  };
}

// ExampleList { repeated string names = 1; string active = 2 } — the set of
// live examples and which one is selected; the page rebuilds its selector.
export function decodeExampleList(root) {
  return {
    names: (root[1] !== undefined ? [].concat(root[1]) : []).map((s) => asString(s)),
    active: root[2] !== undefined ? asString(root[2]) : "",
  };
}

// The entry point: decode one binary WebSocket frame into one of
// { graph } / { batch } / { examples }.
export function decodeFrame(bytes) {
  const root = decodeMessage(new Reader(bytes));

  if (root[1] !== undefined) return { graph: decodeGraphDescription(decodeMessage(asSubReader(root[1]))) };
  if (root[2] !== undefined) return { batch: decodeEventBatch(decodeMessage(asSubReader(root[2]))) };
  if (root[3] !== undefined) return { examples: decodeExampleList(decodeMessage(asSubReader(root[3]))) };

  return { unknown: true };
}

// ClientMessage { oneof body { pause = 1; resume = 2; select_example = 3 } }
// — encoding side. SelectExample { string name = 1 } nests as the oneof's
// length-delimited payload; names here are short, so lengths stay one byte.
export function encodePause() {
  return new Uint8Array([0x0a, 0x00]); // field 1 (Pause), empty message
}

export function encodeResume() {
  return new Uint8Array([0x12, 0x00]); // field 2 (Resume), empty message
}

export function encodeSelectExample(name) {
  const bytes = new TextEncoder().encode(name);
  const inner = [0x0a, bytes.length, ...bytes]; // field 1 (name), string
  return new Uint8Array([0x1a, inner.length, ...inner]); // field 3 (SelectExample)
}