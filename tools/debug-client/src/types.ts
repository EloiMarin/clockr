// The decoded wire types of the debug protocol (package clockr.debug.v1),
// as the client hands them to consumers.
//
// 64-bit integers are `bigint` on purpose: sequence numbers, epochs, request
// ids and kernel `int64` values are unsigned/signed 64-bit on the wire, and
// this client refuses to silently truncate them to a JS number. The MCP layer
// the MCP server converts to JSON numbers or strings at its own boundary; see
// `valueToJson` in value.ts for a convenience conversion.

/** The kernel value variant as it travels over the wire. */
export type Value =
  | { kind: "unit" }
  | { kind: "bool"; value: boolean }
  | { kind: "int"; value: bigint }
  | { kind: "double"; value: number }
  | { kind: "struct"; value: StructValue }
  | { kind: "string"; value: string }
  | { kind: "absent" };

/** One user struct payload: a type id plus positional field values. */
export interface StructValue {
  /** The `TypeSchema.id` this value positions its fields against (0 = unknown). */
  typeId: number;
  fields: Value[];
}

/** One described field of a user type (see `GraphDescription.types`). */
export interface FieldSchema {
  name: string;
  kind: "bool" | "int" | "uint" | "double" | "struct" | string;
  typeId: number;
}

/** The debug schema of one user value type. */
export interface TypeSchema {
  id: number;
  name: string;
  fields: FieldSchema[];
}

/** One lowered graph node. */
export interface Node {
  /** Lowered node id (uint64). */
  id: bigint;
  kind: "input" | "output" | "internal" | "delay" | string;
  clock: string;
  label: string;
  port: string;
  fanIn: bigint[];
}

/** A cross-region edge and its resampling mode. */
export interface Crossing {
  from: bigint;
  to: bigint;
  policy: "latest" | "queued" | string;
}

/** A boundary port: the node id plus its human-readable name. */
export interface Port {
  nodeId: bigint;
  name: string;
}

/** The static structure of the network the server is ticking. */
export interface GraphDescription {
  nodes: Node[];
  crossings: Crossing[];
  inputs: Port[];
  outputs: Port[];
  types: TypeSchema[];
  /** The example this graph belongs to ("" on old servers). */
  name: string;
}

/** Trace event kinds, named as the web UI names them. */
export type EventKind =
  | "region_step_begin"
  | "region_step_end"
  | "node_tick_begin"
  | "node_tick_end"
  | "node_tick_throw"
  | "unknown";

/** One evaluation step of the live trace. */
export interface TraceEvent {
  /** The producer engine's monotonic counter — total order within one clock. */
  sequence: bigint;
  stampNs: bigint;
  kind: EventKind;
  nodeId: bigint;
  clock: string;
  epoch: bigint;
  /** Set (may be `{kind:"unit"}`) for node_tick_end; null otherwise. */
  value: Value | null;
  threw: boolean;
  durationNs: bigint;
}

/** A coalesced batch of events from one producer thread. */
export interface EventBatch {
  clock: string;
  /** Events the producer's bounded queue could not accept since the last batch. */
  dropped: bigint;
  events: TraceEvent[];
}

/** `events()` view of an `EventBatch`; `replay` marks the connect-time history tail. */
export interface EventBatchView extends EventBatch {
  replay: boolean;
}

/** The server's example set and which one is live. */
export interface ExampleList {
  names: string[];
  active: string;
}

/** Reply to a command: applied (with the epoch it landed at) or failed. */
export interface Ack {
  requestId: bigint;
  /** Human-readable failure text; "" on success. */
  error: string;
  /** Epoch the command was applied at; 0 in an error ack (and for reads). */
  appliedEpoch: bigint;
}

/** Reply to a ReadInput/ReadOutput: the snapshot, or a failure text. */
export interface PortValue {
  requestId: bigint;
  port: string;
  value: Value;
  /** Human-readable failure text; "" on success. */
  error: string;
}

/** Command-queue drop accounting (a lost command is visibly lost, never silently). */
export interface CommandStats {
  dropped: bigint;
}

/** One decoded server frame. */
export type ServerFrame =
  | { graph: GraphDescription }
  | { batch: EventBatch }
  | { examples: ExampleList }
  | { ack: Ack }
  | { portValue: PortValue }
  | { commandStats: CommandStats }
  | { unknown: true };
