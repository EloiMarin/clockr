// Smoke test for the web client's protobuf decoder (schema.js).
//
// Builds a couple of wire-format frames by hand (mirroring
// proto/clockr_debug.proto), decodes them with the same code the page uses,
// and checks the result. This keeps the JS half of the wire contract honest
// without a live server or a browser DOM.
//
// Run: node schema_smoke_test.js

import { decodeFrame, encodeSelectExample } from "./schema.js";

// --- a tiny proto3 encoder --------------------------------------------------

function varint(value) {
  const out = [];
  let v = BigInt(value);
  for (;;) {
    const byte = Number(v & 0x7fn);
    v >>= 7n;
    if (v === 0n) { out.push(byte); break; }
    out.push(byte | 0x80);
  }
  return out;
}

function zigzag(value) {
  const n = BigInt(value);
  return Number((n << 1n) ^ (n >> 63n));
}

function tag(field, wire) {
  return varint((field << 3) | wire);
}

function bytesField(field, bytes) {
  return [...tag(field, 2), ...varint(bytes.length), ...bytes];
}

function stringField(field, text) {
  return bytesField(field, [...Buffer.from(text, "utf8")]);
}

function varintField(field, value) {
  return [...tag(field, 0), ...varint(value)];
}

function sintField(field, value) {
  return [...tag(field, 0), ...varint(zigzag(value))];
}

function doubleField(field, value) {
  const buf = Buffer.alloc(8);
  buf.writeDoubleLE(value, 0);
  return [tag(field, 1), ...buf];
}

function messageField(field, bytes) {
  return bytesField(field, bytes);
}

// proto3 packs repeated scalar fields: "repeated uint64 fan_in = 6" travels
// as one wire-2 field whose payload is a run of varints.
function packedVarintField(field, values) {
  const payload = values.flatMap((v) => varint(v));
  return bytesField(field, payload);
}

// --- frames -----------------------------------------------------------------

// GraphDescription { nodes = 1 (repeated Node), inputs = 3, outputs = 4 }
const node0 = [
  ...varintField(1, 0),          // id = 0 (omitted by proto3 in the real
  ...stringField(2, "input"),    // encoder — the decoder must still default it)
  ...stringField(3, "audio"),
  ...stringField(4, "input"),
  ...stringField(5, "bpm"),
];

const node1 = [
  ...varintField(1, 1),
  ...stringField(2, "internal"),
  ...stringField(3, "audio"),
  ...stringField(4, "lift"),
  ...packedVarintField(6, [0, 2]), // fan_in = [0, 2], packed
];

const graph = [
  ...messageField(1, node0),
  ...messageField(1, node1),
  ...messageField(3, [...varintField(1, 0), ...stringField(2, "bpm")]),
  ...messageField(4, [...varintField(1, 1), ...stringField(2, "out")]),
];

const graphFrame = [...messageField(1, graph)];

// A graph description that also carries a user type schema:
// TypeSchema { id = 1, name = 2, fields = 3 (repeated FieldSchema) },
// FieldSchema { name = 1, kind = 2, type_id = 3 }.
const typeSchema = [
  ...varintField(1, 7),                        // id
  ...stringField(2, "demo.region"),            // name
  ...messageField(3, [...stringField(1, "start"), ...stringField(2, "double")]),
  ...messageField(3, [...stringField(1, "end"), ...stringField(2, "double")]),
];

const graphWithTypesFrame = [
  ...messageField(1, [...graph, ...messageField(5, typeSchema)]),
];

// EventBatch { thread = 1, dropped = 2, events = 3 }
const event0 = [
  ...varintField(1, 7),          // sequence
  ...varintField(2, 1000),       // stamp_ns
  ...varintField(3, 3),          // kind = NODE_EVAL_END
  ...varintField(4, 1),          // node_id
  ...stringField(5, "audio"),
  ...varintField(6, 4),          // epoch
  ...messageField(7, [...doubleField(4, 2.5)]), // value = double 2.5
  ...varintField(9, 123456),     // duration_ns
];

const event1 = [
  ...varintField(1, 8),
  ...varintField(3, 1),          // REGION_EVAL_END
  ...stringField(5, "ui"),
  ...varintField(6, 9),
  ...varintField(8, 1),          // threw
  ...varintField(9, 1500000),    // duration_ns: whole-tick time
];

const batchFrame = [
  ...messageField(2, [
    ...stringField(1, "audio"),
    ...varintField(2, 3),        // dropped = 3
    ...messageField(3, event0),
    ...messageField(3, event1),
  ]),
];

// StructValue { type_id = 1, fields = 2 (repeated Value) } inside
// Value.struct_value = 5.
const structValue = [
  ...varintField(1, 7),
  ...messageField(2, [...doubleField(4, 1.0)]),
  ...messageField(2, [...doubleField(4, 4.0)]),
];

const structEvent = [
  ...varintField(1, 9),          // sequence
  ...varintField(3, 3),          // kind = NODE_EVAL_END
  ...varintField(4, 2),          // node_id
  ...stringField(5, "ui"),
  ...varintField(6, 5),          // epoch
  ...messageField(7, [...messageField(5, structValue)]), // value = struct
];

const structBatchFrame = [
  ...messageField(2, [...stringField(1, "ui"), ...messageField(3, structEvent)]),
];

// An anonymous struct: no type_id, no fields (renders as a bare `struct`).
const anonymousBatchFrame = [
  ...messageField(2, [
    ...stringField(1, "ui"),
    ...messageField(3, [
      ...varintField(3, 3),
      ...messageField(7, [...messageField(5, [])]),
    ]),
  ]),
];

// --- checks -----------------------------------------------------------------

let failed = 0;

function check(what, got, want) {
  const ok = JSON.stringify(got) === JSON.stringify(want);
  if (!ok) { failed += 1; console.log(`FAIL ${what}: got=${JSON.stringify(got)} want=${JSON.stringify(want)}`); }
  else console.log(`ok ${what}`);
}

const g = decodeFrame(new Uint8Array(graphFrame));
check("graph frame detected", Boolean(g.graph), true);
check("node count", g.graph.nodes.length, 2);
check("node 0 label", g.graph.nodes[0].label, "input");
check("node 0 port", g.graph.nodes[0].port, "bpm");
check("node 0 thread", g.graph.nodes[0].clock, "audio");
check("node 1 fan-in", g.graph.nodes[1].fanIn, [0, 2]);
check("inputs", g.graph.inputs.map((p) => p.name), ["bpm"]);
check("outputs", g.graph.outputs.map((p) => p.name), ["out"]);

const b = decodeFrame(new Uint8Array(batchFrame));
check("batch frame detected", Boolean(b.batch), true);
check("batch thread", b.batch.clock, "audio");
check("batch dropped", b.batch.dropped, 3);
check("event count", b.batch.events.length, 2);
check("event 0 kind", b.batch.events[0].kind, "node_tick_end");
check("event 0 node", b.batch.events[0].nodeId, 1);
check("event 0 epoch", b.batch.events[0].epoch, 4);
check("event 0 value", b.batch.events[0].value, { kind: "double", value: 2.5 });
check("event 0 duration", b.batch.events[0].durationNs, 123456);
check("event 1 kind", b.batch.events[1].kind, "region_step_end");
check("event 1 threw", b.batch.events[1].threw, true);
check("event 1 duration", b.batch.events[1].durationNs, 1500000);

// User type schemas travel with the graph description, and a struct value
// decodes into a type id plus positional fields.
const gt = decodeFrame(new Uint8Array(graphWithTypesFrame));
check("type count", gt.graph.types.length, 1);
check("type id", gt.graph.types[0].id, 7);
check("type name", gt.graph.types[0].name, "demo.region");
check("type field names", gt.graph.types[0].fields.map((f) => f.name), ["start", "end"]);
check("type field kinds", gt.graph.types[0].fields.map((f) => f.kind), ["double", "double"]);

const s = decodeFrame(new Uint8Array(structBatchFrame));
check("struct value kind", s.batch.events[0].value.kind, "struct");
check("struct type id", s.batch.events[0].value.value.typeId, 7);
check(
  "struct fields",
  s.batch.events[0].value.value.fields,
  [{ kind: "double", value: 1 }, { kind: "double", value: 4 }]
);

const anon = decodeFrame(new Uint8Array(anonymousBatchFrame));
check("anonymous struct type id", anon.batch.events[0].value.value.typeId, 0);
check("anonymous struct fields", anon.batch.events[0].value.value.fields, []);

// Missing scalar fields default (proto3): a Node without id/thread reads 0/"".
const sparseNode = [...stringField(4, "delay")];
const sparse = decodeFrame(new Uint8Array([...messageField(1, [...messageField(1, sparseNode)])]));
check("sparse node id defaults to 0", sparse.graph.nodes[0].id, 0);
check("sparse node kind defaults to empty", sparse.graph.nodes[0].kind, "");
check("sparse node label", sparse.graph.nodes[0].label, "delay");

// A graph description carries its example name (field 6); an unnamed one
// (old servers) defaults to empty.
const namedGraphFrame = [...messageField(1, [...graph, ...stringField(6, "daw")])];
const ng = decodeFrame(new Uint8Array(namedGraphFrame));
check("graph name", ng.graph.name, "daw");
check("unnamed graph defaults to empty", g.graph.name, "");

// ExampleList { repeated string names = 1; string active = 2 } travels as
// ServerMessage field 3; the page rebuilds its example selector from it.
const examplesFrame = [
  ...messageField(3, [
    ...stringField(1, "yang-a"),
    ...stringField(1, "yang-b"),
    ...stringField(2, "yang-b"),
  ]),
];

const ex = decodeFrame(new Uint8Array(examplesFrame));
check("examples frame detected", Boolean(ex.examples), true);
check("examples names", ex.examples.names, ["yang-a", "yang-b"]);
check("examples active", ex.examples.active, "yang-b");

// An empty set (host with no live instances) decodes to empty names/active.
const emptyExamplesFrame = [...messageField(3, [])];
const ee = decodeFrame(new Uint8Array(emptyExamplesFrame));
check("empty examples names", ee.examples.names, []);
check("empty examples active", ee.examples.active, "");

// SelectExample encodes byte-exactly: ClientMessage field 3 holding
// SelectExample { name = "daw" }.
check(
  "select example encoding",
  [...encodeSelectExample("daw")],
  [0x1a, 0x05, 0x0a, 0x03, 0x64, 0x61, 0x77]);

if (failed > 0) { console.log(`${failed} check(s) failed`); process.exit(1); }
console.log("schema smoke test passed");