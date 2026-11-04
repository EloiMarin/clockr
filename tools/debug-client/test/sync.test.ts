// Codec sync test: the client's hand-rolled codec must stay faithful to the
// two sources of truth it is stage-copied/extended from.
//
//   1. tools/web-debugger/proto/clockr_debug.proto — every message field number and enum value
//      in the client's WIRE_SCHEMA table must match the .proto exactly (both
//      directions: an unpinned proto field fails too).
//   2. tools/web-debugger/web/schema.js — the shared v1 server messages must decode
//      identically in the page's decoder and this package's decoder (up to the
//      documented bigint and `crossings` naming divergences).
//
// It also pins the web page's pause/resume/select_example encoding bytes
// against this package's encoders.
//
// Run: node test/sync.test.ts

import { readFile } from "node:fs/promises";

import { isDeepStrictEqual } from "node:util";

import {
  CLIENT_V1_ADDITIONS,
  WIRE_SCHEMA,
  decodeFrame as clientDecodeFrame,
  encodePause as clientEncodePause,
  encodeResume as clientEncodeResume,
  encodeSelectExample as clientEncodeSelectExample,
} from "../src/schema.ts";

import type { ServerFrame } from "../src/types.ts";

import * as web from "../../web-debugger/web/schema.js";

import {
  EVENT_KIND,
  batchFrame,
  examplesFrame,
  graphFrame,
} from "./support/frames.ts";
import { check, finish, ok, section } from "./support/harness.ts";

// --- .proto parsing ----------------------------------------------------------
//
// A deliberately small parser: this repo owns the .proto, it uses no exotic
// syntax, and the test only needs message -> { field: number } plus
// enum -> { NAME: number }. Braces are matched by counting (the file puts them
// on their own lines); comments are stripped first.

interface ProtoSchema {
  messages: Record<string, Record<string, number>>;
  enums: Record<string, Record<string, number>>;
}

function stripComments(source: string): string {
  return source.replace(/\/\*[\s\S]*?\*\//g, "").replace(/\/\/[^\n]*/g, "");
}

function parseProto(source: string): ProtoSchema {
  const clean = stripComments(source);
  const messages: Record<string, Record<string, number>> = {};
  const enums: Record<string, Record<string, number>> = {};

  const declaration = /\b(message|enum)\s+([A-Za-z_]\w*)\s*\{/g;
  let match;

  while ((match = declaration.exec(clean)) !== null) {
    const kind = match[1] as "message" | "enum";
    const name = match[2]!;
    const open = declaration.lastIndex - 1;

    let depth = 0;
    let close = -1;

    for (let index = open; index < clean.length; index += 1) {
      const character = clean[index];

      if (character === "{") depth += 1;
      else if (character === "}") {
        depth -= 1;

        if (depth === 0) {
          close = index;
          break;
        }
      }
    }

    if (close < 0) throw new Error(`sync: unbalanced braces for ${kind} ${name}`);

    const body = clean.slice(open + 1, close);

    if (kind === "message") {
      const fields: Record<string, number> = {};
      const fieldPattern = /(?:^|\n)\s*(?:optional\s+|repeated\s+)?[A-Za-z_][\w.]*\s+([A-Za-z_]\w*)\s*=\s*(\d+)\s*;/g;
      let field;

      while ((field = fieldPattern.exec(body)) !== null) fields[field[1]!] = Number(field[2]);

      messages[name] = fields;
    } else {
      const values: Record<string, number> = {};
      const valuePattern = /(?:^|\n)\s*([A-Za-z_]\w*)\s*=\s*(-?\d+)\s*;/g;
      let value;

      while ((value = valuePattern.exec(body)) !== null) values[value[1]!] = Number(value[2]);

      enums[name] = values;
    }
  }

  return { messages, enums };
}

const here = new URL(".", import.meta.url);
const proto = parseProto(await readFile(new URL("../../web-debugger/proto/clockr_debug.proto", here), "utf8"));

// --- 1. the .proto is the wire contract --------------------------------------

section("WIRE_SCHEMA vs proto/clockr_debug.proto");

const pendingAdditions: string[] = [];

for (const [message, expected] of Object.entries(WIRE_SCHEMA.messages)) {
  const actual = proto.messages[message];

  if (actual === undefined) {
    if ((CLIENT_V1_ADDITIONS as readonly string[]).includes(message)) {
      pendingAdditions.push(message);
      continue;
    }

    check(`proto declares message ${message}`, false, true);
    continue;
  }

  check(`proto fields match for ${message}`, actual, expected);
}

for (const [name, expected] of Object.entries(WIRE_SCHEMA.enums)) {
  check(`proto enum matches for ${name}`, proto.enums[name], expected);
}

const pinned = new Set(Object.keys(WIRE_SCHEMA.messages));
const unpinned = Object.keys(proto.messages).filter((message) => !pinned.has(message));

check("no unpinned proto messages (add new ones to WIRE_SCHEMA)", unpinned, []);

ok(
  "frozen v1 additions are in the proto (the debug server landed)",
  pendingAdditions.length === 0,
  pendingAdditions.length > 0 ? `pending: ${pendingAdditions.join(", ")}` : undefined,
);

// --- 2. the web decoder agrees on the shared messages ------------------------

type Normalized = Record<string, unknown>;

function normalizeNumbers(value: unknown): unknown {
  if (typeof value === "bigint") return Number(value);

  if (Array.isArray(value)) return value.map((entry) => normalizeNumbers(entry));

  if (value !== null && typeof value === "object") {
    const out: Record<string, unknown> = {};

    for (const [key, entry] of Object.entries(value)) out[key] = normalizeNumbers(entry);

    return out;
  }

  return value;
}

// The two documented divergences between this client and the page:
// 64-bit as bigint (normalized away here) and `crossings` vs `sharingPoints`.
function clientFrameToWebShape(frame: ServerFrame): Normalized {
  if ("graph" in frame) {
    const { crossings, ...rest } = frame.graph;

    return { graph: normalizeNumbers({ ...rest, sharingPoints: crossings }) };
  }

  return normalizeNumbers(frame) as Normalized;
}

function sameDecode(what: string, bytes: Uint8Array): void {
  const fromWeb = web.decodeFrame(bytes);
  const fromClient = clientFrameToWebShape(clientDecodeFrame(bytes));

  check(what, fromClient, fromWeb);
}

section("web decoder parity on shared v1 frames");

sameDecode("graph frame", graphFrame({
  name: "demo",
  nodes: [
    { id: 0, kind: "input", clock: "ui", label: "input", port: "bpm" },
    { id: 1, kind: "internal", clock: "audio", label: "lift", fanIn: [0, 2] },
  ],
  crossings: [{ from: 0, to: 1, policy: "latest" }],
  inputs: [{ nodeId: 0, name: "bpm" }],
  outputs: [{ nodeId: 1, name: "out" }],
}));

sameDecode("examples frame", examplesFrame(["demo", "daw"], "daw"));

sameDecode("event batch frame (all shared value variants)", batchFrame({
  clock: "audio",
  dropped: 4n,
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
      kind: EVENT_KIND.NODE_EVAL_END,
      nodeId: 2n,
      clock: "ui",
      epoch: 5n,
      value: { kind: "int", value: -42n },
    },
    {
      sequence: 9n,
      stampNs: 3000n,
      kind: EVENT_KIND.NODE_EVAL_END,
      nodeId: 3n,
      clock: "ui",
      epoch: 6n,
      value: { kind: "struct", value: { typeId: 7, fields: [{ kind: "bool", value: false }] } },
    },
    {
      sequence: 10n,
      stampNs: 4000n,
      kind: EVENT_KIND.REGION_EVAL_END,
      clock: "ui",
      epoch: 7n,
      threw: true,
      durationNs: 999n,
    },
  ],
}));

sameDecode("empty batch frame", batchFrame({ clock: "worker" }));

// --- 3. the web encoder's bytes match this package's ------------------------

section("client encoders byte-match web/schema.js");

for (const name of ["demo", "daw", "yang-b", ""]) {
  check(`select_example("${name}") bytes`, [...clientEncodeSelectExample(name)], [...web.encodeSelectExample(name)]);
}

check("pause bytes", [...clientEncodePause()], [...web.encodePause()]);
check("resume bytes", [...clientEncodeResume()], [...web.encodeResume()]);

// A final sanity print so a future reader sees what "sync" means concretely.
{
  const protoUrl = new URL("../../web-debugger/proto/clockr_debug.proto", here);

  console.log(`\npinned against ${protoUrl.pathname}`);
  console.log(`web decoder: ${new URL("../../web-debugger/web/schema.js", here).pathname}`);
  console.log(`proto messages: ${Object.keys(proto.messages).length}, pinned: ${pinned.size}`);
}

// Keep the unused-helper warning quiet in editors that don't see the loop.
ok("normalizer handles nested comparisons", isDeepStrictEqual(normalizeNumbers(1n), 1));

finish("sync.test.ts");
