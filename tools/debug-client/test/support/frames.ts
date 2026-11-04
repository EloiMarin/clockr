// Hand-built *server-side* frames for the client tests. The package's codec
// decodes server messages and encodes client messages; tests need the other
// direction, so this helper builds clockr.debug.v1 ServerMessage frames from
// the same field numbers as proto/clockr_debug.proto.
//
// This also doubles as an independent check of the client codec: the bytes are
// assembled by hand here from the proto, not by reusing the decoder's tables.

import { Reader, decodeMessage } from "../../src/protobuf.ts";
import { encodeValue } from "../../src/schema.ts";
import {
  concatBytes,
  doubleField,
  messageField,
  packedVarintField,
  stringField,
  varintField,
} from "../../src/wire.ts";

import type { Value } from "../../src/types.ts";

/** proto EventKind values. */
export const EVENT_KIND = {
  REGION_EVAL_BEGIN: 0,
  REGION_EVAL_END: 1,
  NODE_EVAL_BEGIN: 2,
  NODE_EVAL_END: 3,
  NODE_EVAL_THROW: 4,
} as const;

export interface ServerNode {
  id?: bigint | number;
  kind?: string;
  clock?: string;
  label?: string;
  port?: string;
  fanIn?: Array<bigint | number>;
}

export interface ServerPort {
  nodeId?: bigint | number;
  name?: string;
}

export interface ServerCrossing {
  from?: bigint | number;
  to?: bigint | number;
  policy?: string;
}

export interface ServerGraph {
  name?: string;
  nodes?: ServerNode[];
  inputs?: ServerPort[];
  outputs?: ServerPort[];
  crossings?: ServerCrossing[];
}

function uint64Field(field: number, value: bigint | number | undefined): number[] {
  return value === undefined ? [] : varintField(field, value, { omitDefault: true });
}

function textField(field: number, value: string | undefined): number[] {
  return value === undefined || value === "" ? [] : stringField(field, value);
}

function nodeBody(node: ServerNode): number[] {
  return [
    ...uint64Field(1, node.id),
    ...textField(2, node.kind),
    ...textField(3, node.clock),
    ...textField(4, node.label),
    ...textField(5, node.port),
    ...(node.fanIn === undefined || node.fanIn.length === 0 ? [] : packedVarintField(6, node.fanIn)),
  ];
}

function portBody(port: ServerPort): number[] {
  return [...uint64Field(1, port.nodeId), ...textField(2, port.name)];
}

function crossingBody(crossing: ServerCrossing): number[] {
  return [...uint64Field(1, crossing.from), ...uint64Field(2, crossing.to), ...textField(3, crossing.policy)];
}

/** One ServerMessage.graph frame: GraphDescription { 1 nodes, 2 crossings, 3 inputs, 4 outputs, 5 types, 6 name } */
export function graphFrame(graph: ServerGraph): Uint8Array {
  const body = [
    ...(graph.nodes ?? []).flatMap((node) => messageField(1, nodeBody(node))),
    ...(graph.crossings ?? []).flatMap((crossing) => messageField(2, crossingBody(crossing))),
    ...(graph.inputs ?? []).flatMap((port) => messageField(3, portBody(port))),
    ...(graph.outputs ?? []).flatMap((port) => messageField(4, portBody(port))),
    ...textField(6, graph.name),
  ];

  return concatBytes([messageField(1, body)]);
}

/** One ServerMessage.examples frame: ExampleList { 1 names, 2 active }. */
export function examplesFrame(names: string[], active: string): Uint8Array {
  return concatBytes([
    messageField(3, [
      ...names.flatMap((name) => stringField(1, name)),
      ...textField(2, active),
    ]),
  ]);
}

export interface ServerEvent {
  sequence?: bigint | number;
  stampNs?: bigint | number;
  kind: number;
  nodeId?: bigint | number;
  clock?: string;
  epoch?: bigint | number;
  value?: Value;
  threw?: boolean;
  durationNs?: bigint | number;
}

function traceEventBody(event: ServerEvent): number[] {
  return [
    ...uint64Field(1, event.sequence),
    ...uint64Field(2, event.stampNs),
    ...varintField(3, event.kind, { omitDefault: true }),
    ...uint64Field(4, event.nodeId),
    ...textField(5, event.clock),
    ...uint64Field(6, event.epoch),
    ...(event.value === undefined ? [] : messageField(7, encodeValue(event.value))),
    ...(event.threw === true ? varintField(8, true) : []),
    ...uint64Field(9, event.durationNs),
  ];
}

export interface ServerBatch {
  /** Empty string = connect-time history replay (the real server's marker). */
  clock: string;
  dropped?: bigint | number;
  events?: ServerEvent[];
}

/** One ServerMessage.batch frame: EventBatch { 1 clock, 2 dropped, 3 events }. */
export function batchFrame(batch: ServerBatch): Uint8Array {
  return concatBytes([
    messageField(2, [
      ...textField(1, batch.clock),
      ...uint64Field(2, batch.dropped),
      ...(batch.events ?? []).flatMap((event) => messageField(3, traceEventBody(event))),
    ]),
  ]);
}

/** One ServerMessage.ack frame: Ack { 1 request_id, 2 error, 3 applied_epoch }. */
export function ackFrame(ack: { requestId: bigint | number; error?: string; appliedEpoch?: bigint | number }): Uint8Array {
  return concatBytes([
    messageField(4, [
      ...uint64Field(1, ack.requestId),
      ...textField(2, ack.error),
      ...uint64Field(3, ack.appliedEpoch),
    ]),
  ]);
}

/** One ServerMessage.port_value frame: PortValue { 1 request_id, 2 port, 3 value, 4 error }. */
export function portValueFrame(portValue: {
  requestId: bigint | number;
  port?: string;
  value?: Value;
  error?: string;
}): Uint8Array {
  return concatBytes([
    messageField(5, [
      ...uint64Field(1, portValue.requestId),
      ...textField(2, portValue.port),
      ...(portValue.value === undefined ? [] : messageField(3, encodeValue(portValue.value))),
      ...textField(4, portValue.error),
    ]),
  ]);
}

/** One ServerMessage.command_stats frame: CommandStats { 1 dropped }. */
export function commandStatsFrame(dropped: bigint | number): Uint8Array {
  return concatBytes([messageField(6, uint64Field(1, dropped))]);
}

/** A synthetic trace-event batch with sensible live-clock defaults. */
export function eventBatch(clock: string, sequences: Array<bigint | number>, extra: Partial<ServerEvent> = {}): Uint8Array {
  return batchFrame({
    clock,
    events: sequences.map((sequence) => ({
      sequence,
      stampNs: BigInt(sequence) * 1000n,
      kind: EVENT_KIND.NODE_EVAL_END,
      nodeId: 1,
      clock,
      epoch: 1,
      value: { kind: "double", value: Number(sequence) },
      durationNs: 10,
      ...extra,
    })),
  });
}

// --- client-frame inspection -------------------------------------------------

type MessageMap = Record<number, { __sub: Reader } | bigint | number | undefined>;

/** Decode the outer ClientMessage frame to its raw field map. */
export function decodeClientFrame(bytes: Uint8Array): MessageMap {
  return decodeMessage(new Reader(bytes)) as MessageMap;
}

/** The oneof field number a client frame set (1 pause, 2 resume, 3 select_example, 4/5/6 commands). */
export function clientFrameField(bytes: Uint8Array): number {
  const root = decodeClientFrame(bytes);

  for (const key of Object.keys(root)) return Number(key);

  throw new Error("empty ClientMessage frame");
}

/** Decode a nested oneof body (e.g. SetInput) to its raw field map. */
export function messageBody(bytes: Uint8Array, field: number): MessageMap {
  const slot = decodeClientFrame(bytes)[field] as { __sub: Reader } | undefined;

  if (slot === undefined || slot.__sub === undefined) throw new Error(`frame has no field ${field}`);

  return decodeMessage(slot.__sub) as MessageMap;
}

/** The request_id inside a set_input/read_input/read_output frame. */
export function requestIdOf(bytes: Uint8Array, oneofField: number): bigint {
  const body = messageBody(bytes, oneofField);
  const value = body[1];

  return typeof value === "bigint" ? value : BigInt((value as number | undefined) ?? 0);
}
