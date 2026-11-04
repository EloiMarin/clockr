// The MCP tools — the agent-facing surface over the debug client.
//
// Every tool is a thin translation: validate the JSON arguments, call one
// client method (or the `events()` iterator), and serialise the result as one
// JSON text content block. Nothing here touches the wire protocol, the
// protobuf codec, or the WebSocket; that all stays in the client.
//
// Three rules keep the layer honest:
//
//   1. **Protocol failures are JSON-RPC errors, tool failures are results.**
//      Bad arguments -> `InvalidParamsError` (the dispatcher answers -32602).
//      The debugger refusing a request (unknown port, timeout, not connected)
//      -> an `isError` tool result carrying `{ "error": ... }`, so the model
//      sees the failure and can react instead of the MCP session erroring out.
//
//   2. **Bigints never reach JSON.stringify.** Sequence numbers, epochs, node
//      ids and int64 values are `bigint` in the client; every one of them goes
//      through `bigintToJson` (number when lossless, decimal string otherwise).
//      Tool *inputs* are JSON, so a JSON number is a double by construction;
//      the schema description tells the model how to write an int64.
//
//   3. **Event queries are bounded at the protocol level, not by discipline.**
//      A live trace is verbose and a fresh connection replays retained history,
//      so `events_tail`/`events_watch` enforce a hard `maxBytes` on their
//      serialized result text and a `maxEvents` count cap, expose filters and a
//      `summary` mode, and return `nextSince` — the explicit paging cursor for
//      a truncated result. An agent cannot blow up its context by accident;
//      see the tools' own descriptions (they are model-facing docs) and
//      README.md.

import { bigintToJson, toValue, valueToJson } from "../../debug-client/src/index.ts";

import { INVALID_PARAMS, InvalidParamsError, JsonRpcError } from "./errors.ts";

import type { DebugClientLike } from "./debug-client.ts";

import type {
  EventBatchView,
  GraphDescription,
  TraceEvent,
  ValueInput,
} from "../../debug-client/src/index.ts";

/** One MCP tool result: content blocks, plus `isError` for tool failures. */
export interface ToolResult {
  content: Array<{ type: "text"; text: string }>;
  isError?: boolean;
}

/** What a tool handler may use: the connected client, nothing else. */
export interface ToolContext {
  client: DebugClientLike;
}

/** One tool definition, exactly as `tools/list` advertises it. */
export interface Tool {
  name: string;
  description: string;
  inputSchema: Record<string, unknown>;
  handler: (args: Record<string, unknown>, context: ToolContext) => Promise<ToolResult>;
}

/**
 * How long `events_watch` keeps collecting after the first delta arrives, so
 * one watch usually returns a whole multi-clock tick instead of one clock's
 * slice of it.
 */
const WATCH_SETTLE_MS = 15;

/** Hard maximum for `events_tail`'s desired tail size `n` and for `maxEvents`. */
const EVENTS_MAX_EVENTS_LIMIT = 2_000;

/** `maxEvents` default for `events_watch`. */
const WATCH_DEFAULT_MAX_EVENTS = 200;

/** `maxEvents` default ceiling for `events_tail` (effectively `min(n, 200)`). */
const TAIL_DEFAULT_MAX_EVENTS = 200;

/** Hard byte bound defaults/limits for one serialized tool-result text. */
const DEFAULT_MAX_BYTES = 262_144; // 256 KiB
const MIN_MAX_BYTES = 1_024; // 1 KiB
const MAX_MAX_BYTES = 2_097_152; // 2 MiB

/** `summary.nodes` is a top-N histogram; 20 keeps the aggregate small. */
const SUMMARY_MAX_NODES = 20;

/** The event kinds the trace can carry (validated filter values). */
const EVENT_KINDS = new Set([
  "region_step_begin",
  "region_step_end",
  "node_tick_begin",
  "node_tick_end",
  "node_tick_throw",
]);

// --- JSON conversions -------------------------------------------------------

/** An event with every bigint converted for JSON. */
export function eventToJson(event: TraceEvent): Record<string, unknown> {
  return {
    sequence: bigintToJson(event.sequence),
    stampNs: bigintToJson(event.stampNs),
    kind: event.kind,
    clock: event.clock,
    nodeId: bigintToJson(event.nodeId),
    epoch: bigintToJson(event.epoch),
    value: event.value === null ? null : valueToJson(event.value),
    threw: event.threw,
    durationNs: bigintToJson(event.durationNs),
  };
}

/** A graph description with every bigint converted for JSON. */
export function graphToJson(graph: GraphDescription): Record<string, unknown> {
  return {
    example: graph.name,
    counts: {
      nodes: graph.nodes.length,
      crossings: graph.crossings.length,
      inputs: graph.inputs.length,
      outputs: graph.outputs.length,
      types: graph.types.length,
    },
    nodes: graph.nodes.map((node) => ({
      id: bigintToJson(node.id),
      kind: node.kind,
      clock: node.clock,
      label: node.label,
      port: node.port,
      fanIn: node.fanIn.map((id) => bigintToJson(id)),
    })),
    crossings: graph.crossings.map((crossing) => ({
      from: bigintToJson(crossing.from),
      to: bigintToJson(crossing.to),
      policy: crossing.policy,
    })),
    inputs: graph.inputs.map((port) => ({ nodeId: bigintToJson(port.nodeId), name: port.name })),
    outputs: graph.outputs.map((port) => ({ nodeId: bigintToJson(port.nodeId), name: port.name })),
    types: graph.types,
  };
}

function cursorsToJson(cursors: Record<string, bigint>): Record<string, number | string> {
  const out: Record<string, number | string> = {};

  for (const [clock, sequence] of Object.entries(cursors)) out[clock] = bigintToJson(sequence);

  return out;
}

function droppedToJson(client: DebugClientLike): Record<string, unknown> {
  const events: Record<string, number | string> = {};

  for (const [clock, count] of Object.entries(client.dropped.events)) {
    events[clock] = bigintToJson(count);
  }

  return { events, commands: bigintToJson(client.dropped.commands) };
}

function summaryText(payload: Record<string, unknown>): string {
  return JSON.stringify(payload);
}

function jsonText(payload: unknown): ToolResult {
  return { content: [{ type: "text", text: JSON.stringify(payload) }] };
}

function toolFailure(error: unknown): ToolResult {
  const typed = error as { message?: string; code?: string };
  const payload: Record<string, unknown> = { error: typed?.message ?? String(error) };

  // Client errors carry a structured `code` (`ack`, `timeout`, `not_connected`,
  // `unknown_example`, ...); pass it through so the model can branch on it.
  if (typeof typed?.code === "string") payload.code = typed.code;

  return { content: [{ type: "text", text: JSON.stringify(payload) }], isError: true };
}

// --- argument validation ----------------------------------------------------

function checkAllowed(tool: string, args: Record<string, unknown>, allowed: readonly string[]): void {
  for (const key of Object.keys(args)) {
    if (!allowed.includes(key)) {
      throw new InvalidParamsError(`${tool}: unexpected argument "${key}" (allowed: ${allowed.join(", ")})`);
    }
  }
}

function requireString(tool: string, args: Record<string, unknown>, name: string): string {
  const value = args[name];

  if (typeof value !== "string" || value.trim() === "") {
    throw new InvalidParamsError(`${tool}: "${name}" must be a non-empty string`);
  }

  return value;
}

function optionalInteger(
  tool: string,
  args: Record<string, unknown>,
  name: string,
  fallback: number,
  minimum: number,
  maximum: number,
): number {
  const value = args[name];

  if (value === undefined) return fallback;

  if (typeof value !== "number" || !Number.isInteger(value) || value < minimum || value > maximum) {
    throw new InvalidParamsError(`${tool}: "${name}" must be an integer in [${minimum}, ${maximum}]`);
  }

  return value;
}

function optionalBoolean(
  tool: string,
  args: Record<string, unknown>,
  name: string,
  fallback: boolean,
): boolean {
  const value = args[name];

  if (value === undefined) return fallback;

  if (typeof value !== "boolean") throw new InvalidParamsError(`${tool}: "${name}" must be a boolean`);

  return value;
}

/**
 * One string or an array of strings (the `clock` / `kind` filter shape).
 * Returns null when the argument is absent; an empty list is rejected because a
 * filter that matches nothing is almost always a mistake.
 */
function optionalStringList(tool: string, args: Record<string, unknown>, name: string): string[] | null {
  const value = args[name];

  if (value === undefined) return null;

  const list = Array.isArray(value) ? value : [value];

  if (list.length === 0) throw new InvalidParamsError(`${tool}: "${name}" must not be an empty array`);

  for (const item of list) {
    if (typeof item !== "string" || item.trim() === "") {
      throw new InvalidParamsError(`${tool}: "${name}" entries must be non-empty strings`);
    }
  }

  return list as string[];
}

/** One node id (safe integer or decimal string) or an array of them. */
function optionalNodeIdList(tool: string, args: Record<string, unknown>, name: string): bigint[] | null {
  const value = args[name];

  if (value === undefined) return null;

  const list = Array.isArray(value) ? value : [value];

  if (list.length === 0) throw new InvalidParamsError(`${tool}: "${name}" must not be an empty array`);

  const ids: bigint[] = [];

  for (const item of list) {
    if (typeof item === "number" && Number.isSafeInteger(item) && item >= 0) {
      ids.push(BigInt(item));
    } else if (typeof item === "string" && /^\d+$/.test(item)) {
      ids.push(BigInt(item));
    } else {
      throw new InvalidParamsError(
        `${tool}: "${name}" entries must be non-negative safe integers or decimal strings`,
      );
    }
  }

  return ids;
}

/**
 * `since` is a per-clock sequence cursor: `{ "audio": 42, "ui": 17 }`. Values
 * may be JSON numbers (safe range) or decimal strings (when a cursor exceeds
 * 2^53, though a real trace would take aeons to get there).
 */
function optionalSince(
  tool: string,
  args: Record<string, unknown>,
): Record<string, bigint> | undefined {
  const value = args.since;

  if (value === undefined) return undefined;

  if (value === null || typeof value !== "object" || Array.isArray(value)) {
    throw new InvalidParamsError(`${tool}: "since" must be an object mapping clock name to sequence number`);
  }

  const since: Record<string, bigint> = {};

  for (const [clock, cursor] of Object.entries(value as Record<string, unknown>)) {
    if (typeof cursor === "number" && Number.isInteger(cursor) && cursor >= 0) {
      since[clock] = BigInt(cursor);
    } else if (typeof cursor === "string" && /^\d+$/.test(cursor)) {
      since[clock] = BigInt(cursor);
    } else {
      throw new InvalidParamsError(
        `${tool}: since["${clock}"] must be a non-negative integer or a decimal string`,
      );
    }
  }

  return since;
}

/** Validate a `value` the client can encode; the client's own `toValue` is authoritative. */
function requiredValue(tool: string, args: Record<string, unknown>): ValueInput {
  if (!Object.prototype.hasOwnProperty.call(args, "value")) {
    throw new InvalidParamsError(`${tool}: missing required argument "value"`);
  }

  try {
    toValue(args.value as ValueInput);
  } catch (error) {
    throw new InvalidParamsError(`${tool}: invalid value: ${(error as Error).message}`);
  }

  return args.value as ValueInput;
}

/** A stalled connection should fail the tool, not hang the MCP session. */
function optionalTimeoutMs(tool: string, args: Record<string, unknown>, fallback: number): number {
  const value = args.timeoutMs;

  if (value === undefined) return fallback;

  if (typeof value !== "number" || !Number.isInteger(value) || value <= 0 || value > 60_000) {
    throw new InvalidParamsError(`${tool}: "timeoutMs" must be an integer in [1, 60000]`);
  }

  return value;
}

// --- event filters ----------------------------------------------------------

interface EventFilters {
  clocks: Set<string> | null;
  nodeIds: Set<bigint> | null;
  kinds: Set<string> | null;
}

/**
 * Parse and validate the shared `clock` / `nodeId` / `kind` filters.
 *
 * Clock names are checked against the live graph when one is known — a filter
 * that cannot match is a caller error, not an empty result to guess about. When
 * no graph has arrived yet there is nothing to validate against, so the names
 * are accepted and the filter simply applies to the events seen.
 */
function parseEventFilters(
  tool: string,
  args: Record<string, unknown>,
  client: DebugClientLike,
): EventFilters {
  const clocks = optionalStringList(tool, args, "clock");

  if (clocks !== null && client.graph !== null) {
    const known = new Set(client.graph.nodes.map((node) => node.clock));

    for (const clock of clocks) {
      if (!known.has(clock)) {
        throw new InvalidParamsError(
          `${tool}: unknown clock "${clock}" (graph clocks: ${[...known].join(", ")})`,
        );
      }
    }
  }

  const nodeIds = optionalNodeIdList(tool, args, "nodeId");
  const kinds = optionalStringList(tool, args, "kind");

  if (kinds !== null) {
    for (const kind of kinds) {
      if (!EVENT_KINDS.has(kind)) {
        throw new InvalidParamsError(
          `${tool}: unknown kind "${kind}" (known: ${[...EVENT_KINDS].join(", ")})`,
        );
      }
    }
  }

  return {
    clocks: clocks === null ? null : new Set(clocks),
    nodeIds: nodeIds === null ? null : new Set(nodeIds),
    kinds: kinds === null ? null : new Set(kinds),
  };
}

function matchesFilters(event: TraceEvent, filters: EventFilters): boolean {
  if (filters.clocks !== null && !filters.clocks.has(event.clock)) return false;
  if (filters.nodeIds !== null && !filters.nodeIds.has(event.nodeId)) return false;
  if (filters.kinds !== null && !filters.kinds.has(event.kind)) return false;

  return true;
}

// --- bounded serialisation --------------------------------------------------

function textBytes(text: string): number {
  return Buffer.byteLength(text, "utf8");
}

/**
 * Build the payload for `events`, serialise it, and drop events until the
 * serialized text fits `maxBytes` — the one place the hard byte bound is
 * enforced.
 *
 * `direction` decides which end of the delta survives:
 *   - `"front"` (tail): drop the OLDEST events, keep the newest;
 *   - `"rear"`  (watch): drop the NEWEST events, keep the oldest prefix, so
 *     `nextSince` (the last kept sequence) lets the caller page the rest
 *     without skipping anything.
 *
 * The drop size is estimated from the overage (rather than one event at a
 * time), so trimming a large result converges in a few serialisations; the
 * final loop condition is the exact check, so the bound always holds.
 */
function trimEventPayload(
  build: (events: TraceEvent[]) => Record<string, unknown>,
  events: TraceEvent[],
  maxBytes: number,
  direction: "front" | "rear",
): { events: TraceEvent[]; text: string } {
  let kept = events;
  let text = JSON.stringify(build(kept));
  let bytes = textBytes(text);

  while (bytes > maxBytes && kept.length > 0) {
    const ratio = maxBytes / bytes;
    const drop = Math.max(1, Math.min(kept.length, Math.ceil(kept.length * (1 - ratio))));

    kept = direction === "front" ? kept.slice(drop) : kept.slice(0, kept.length - drop);

    text = JSON.stringify(build(kept));
    bytes = textBytes(text);
  }

  return { events: kept, text };
}

/** Seed the paging cursor from the caller's effective `since` (or nothing). */
function nextSinceSeed(since: Record<string, bigint> | null | undefined): Map<string, bigint> {
  return new Map(Object.entries(since ?? {}));
}

/** Advance a paging cursor over kept events. */
function advanceNextSince(seed: Map<string, bigint>, events: TraceEvent[]): Map<string, bigint> {
  const next = new Map(seed);

  for (const event of events) {
    const current = next.get(event.clock);

    if (current === undefined || event.sequence > current) next.set(event.clock, event.sequence);
  }

  return next;
}

function nextSinceJson(next: Map<string, bigint>): Record<string, number | string> {
  const out: Record<string, number | string> = {};

  for (const [clock, sequence] of next) out[clock] = bigintToJson(sequence);

  return out;
}

// --- summary mode -----------------------------------------------------------

interface ClockAggregate {
  events: number;
  byKind: Map<string, number>;
  firstEpoch: bigint | null;
  lastEpoch: bigint | null;
  threw: number;
}

interface SummaryAccumulator {
  matched: number;
  byClock: Map<string, ClockAggregate>;
  byKind: Map<string, number>;
  nodeCounts: Map<bigint, number>;
  firstEpoch: bigint | null;
  lastEpoch: bigint | null;
  threw: number;
  lastSequence: Map<string, bigint>;
}

function makeAccumulator(): SummaryAccumulator {
  return {
    matched: 0,
    byClock: new Map(),
    byKind: new Map(),
    nodeCounts: new Map(),
    firstEpoch: null,
    lastEpoch: null,
    threw: 0,
    lastSequence: new Map(),
  };
}

function bump(map: Map<string, number>, key: string): void {
  map.set(key, (map.get(key) ?? 0) + 1);
}

function accumulateEvent(accumulator: SummaryAccumulator, event: TraceEvent): void {
  accumulator.matched += 1;

  bump(accumulator.byKind, event.kind);

  let clock = accumulator.byClock.get(event.clock);

  if (clock === undefined) {
    clock = { events: 0, byKind: new Map(), firstEpoch: null, lastEpoch: null, threw: 0 };
    accumulator.byClock.set(event.clock, clock);
  }

  clock.events += 1;
  bump(clock.byKind, event.kind);

  if (clock.firstEpoch === null || event.epoch < clock.firstEpoch) clock.firstEpoch = event.epoch;
  if (clock.lastEpoch === null || event.epoch > clock.lastEpoch) clock.lastEpoch = event.epoch;

  if (accumulator.firstEpoch === null || event.epoch < accumulator.firstEpoch) accumulator.firstEpoch = event.epoch;
  if (accumulator.lastEpoch === null || event.epoch > accumulator.lastEpoch) accumulator.lastEpoch = event.epoch;

  if (event.threw) {
    accumulator.threw += 1;
    clock.threw += 1;
  }

  accumulator.nodeCounts.set(event.nodeId, (accumulator.nodeCounts.get(event.nodeId) ?? 0) + 1);

  const sequence = accumulator.lastSequence.get(event.clock);

  if (sequence === undefined || event.sequence > sequence) accumulator.lastSequence.set(event.clock, event.sequence);
}

function epochJson(epoch: bigint | null): number | string | null {
  return epoch === null ? null : bigintToJson(epoch);
}

function kindCountsJson(map: Map<string, number>, limit: number): Record<string, number> {
  const out: Record<string, number> = {};

  for (const [kind, count] of [...map.entries()].slice(0, limit)) out[kind] = count;

  return out;
}

/**
 * The aggregate document. `nodeLimit` / `clockLimit` / `kindLimit` are trimming
 * dials used when the serialized payload would exceed `maxBytes`; the shape
 * stays the same, entries are dropped from the least useful end.
 */
function summaryJson(
  accumulator: SummaryAccumulator,
  nodeLimit: number,
  clockLimit: number,
  kindLimit: number,
): Record<string, unknown> {
  const clocks = [...accumulator.byClock.entries()]
    .sort((left, right) => right[1].events - left[1].events)
    .slice(0, clockLimit);
  const byClock: Record<string, unknown> = {};

  for (const [clock, aggregate] of clocks) {
    byClock[clock] = {
      events: aggregate.events,
      byKind: kindCountsJson(aggregate.byKind, kindLimit),
      epochs: { first: epochJson(aggregate.firstEpoch), last: epochJson(aggregate.lastEpoch) },
      threw: aggregate.threw,
    };
  }

  const nodes = [...accumulator.nodeCounts.entries()]
    .sort((left, right) => right[1] - left[1] || (left[0] < right[0] ? -1 : left[0] > right[0] ? 1 : 0))
    .slice(0, nodeLimit)
    .map(([nodeId, count]) => ({ nodeId: bigintToJson(nodeId), count }));

  const byKind = kindCountsJson(accumulator.byKind, kindLimit);

  return {
    events: accumulator.matched,
    byClock,
    byKind,
    nodes,
    epochs: { first: epochJson(accumulator.firstEpoch), last: epochJson(accumulator.lastEpoch) },
    threw: accumulator.threw,
  };
}

/**
 * Build the summary payload and trim its histogram until the serialized text
 * fits `maxBytes` (nodes first — they are a top-N convenience — then clock and
 * kind breakdowns, keeping the totals). Returns whether anything was trimmed.
 */
function buildSummaryPayload(
  base: (summary: Record<string, unknown>) => Record<string, unknown>,
  accumulator: SummaryAccumulator,
  maxBytes: number,
): { text: string; truncated: boolean } {
  let nodeLimit = Math.min(accumulator.nodeCounts.size, SUMMARY_MAX_NODES);
  let clockLimit = accumulator.byClock.size;
  let kindLimit = Math.max(EVENT_KINDS.size, accumulator.byKind.size);
  let trimmed = false;

  for (;;) {
    const payload = base(summaryJson(accumulator, nodeLimit, clockLimit, kindLimit));

    payload.truncated = trimmed;

    const text = summaryText(payload);

    if (textBytes(text) <= maxBytes) return { text, truncated: trimmed };

    if (nodeLimit > 0) {
      nodeLimit -= 1;
      trimmed = true;
      continue;
    }

    if (clockLimit > 0) {
      clockLimit -= 1;
      trimmed = true;
      continue;
    }

    if (kindLimit > 0) {
      kindLimit -= 1;
      trimmed = true;
      continue;
    }

    // Metadata-only fallback: cannot happen for realistic clock counts and the
    // enforced 1 KiB minimum, but returning a tiny text still honours the bound.
    return {
      text: JSON.stringify({ ...base(summaryJson(accumulator, 0, 0, 0)), summary: { events: accumulator.matched } }),
      truncated: true,
    };
  }
}

// --- client helpers ---------------------------------------------------------

/**
 * The graph of the live example, connecting on demand. The MCP server starts
 * before the debugger host may exist; the client reconnects in the background
 * and `connect()` resolves as soon as the socket delivers a graph.
 */
async function ensureGraph(client: DebugClientLike): Promise<GraphDescription> {
  const graph = client.graph;

  if (graph !== null && client.state === "open") return graph;

  return client.connect();
}

/** The active example name, from the graph when it has one, else the example list. */
function activeExample(client: DebugClientLike): string {
  return client.graph?.name !== undefined && client.graph.name !== ""
    ? client.graph.name
    : client.currentExample;
}

// --- auto-paging state ------------------------------------------------------
//
// One stdio MCP process is one agent session, so module-level state *is*
// session state. A truncated `events_watch` leaves a paging cursor here; the
// next call that omits `since` continues from it instead of silently starting
// at the client's read head (which has already advanced past the trimmed
// remainder). The result echoes the cursor and `autoPaged`, so this is visible
// rather than magic — an explicit `since` always wins.

interface LastPageCursor {
  /** The example the cursor belongs to; ignored (and cleared) after a switch. */
  example: string;
  cursor: Map<string, bigint>;
}

let lastPageCursor: LastPageCursor | null = null;

/** Forget the auto-paging cursor (example switch, stale session, tests). */
export function clearAutoPaging(): void {
  lastPageCursor = null;
}

interface WatchPageBase {
  /** What to pass to the client's `events()`; undefined = its read-head default. */
  sinceSequence: Record<string, bigint> | undefined;
  /** The per-clock map actually in effect (for the result echo). */
  effective: Map<string, bigint>;
  autoPaged: boolean;
}

/**
 * Choose the effective `since` for a watch:
 *   1. an explicit `since` always wins;
 *   2. otherwise the stored cursor from the previous truncated page, when it
 *      belongs to the same example and the connection is open;
 *   3. otherwise the client's read-head default (and the stale cursor is gone).
 */
function resolveWatchSince(
  client: DebugClientLike,
  explicit: Record<string, bigint> | undefined,
): WatchPageBase {
  if (explicit !== undefined) {
    return { sinceSequence: explicit, effective: new Map(Object.entries(explicit)), autoPaged: false };
  }

  const example = activeExample(client);

  if (lastPageCursor !== null && lastPageCursor.example === example && client.state === "open") {
    return {
      sinceSequence: Object.fromEntries(lastPageCursor.cursor),
      effective: new Map(lastPageCursor.cursor),
      autoPaged: true,
    };
  }

  // Gone stale (example switched, connection dropped) or nothing to continue.
  lastPageCursor = null;

  return { sinceSequence: undefined, effective: new Map(), autoPaged: false };
}

/** Remember the continuation only while the caller is behind; caught up clears it. */
function updateAutoPaging(client: DebugClientLike, truncated: boolean, next: Map<string, bigint>): void {
  lastPageCursor = truncated ? { example: activeExample(client), cursor: new Map(next) } : null;
}

// --- tool handlers ----------------------------------------------------------

async function graphGet(args: Record<string, unknown>, context: ToolContext): Promise<ToolResult> {
  checkAllowed("graph_get", args, []);
  const graph = await ensureGraph(context.client);

  return jsonText(graphToJson(graph));
}

async function eventsTail(args: Record<string, unknown>, context: ToolContext): Promise<ToolResult> {
  checkAllowed("events_tail", args, [
    "n",
    "since",
    "clock",
    "nodeId",
    "kind",
    "maxEvents",
    "maxBytes",
    "summary",
  ]);

  const n = optionalInteger("events_tail", args, "n", 50, 1, EVENTS_MAX_EVENTS_LIMIT);
  // No cursor means "the retained trace", not "since my last MCP call": a tail
  // must be reproducible for an agent that starts fresh, so the empty object
  // explicitly overrides the client's read cursor.
  const since = optionalSince("events_tail", args) ?? {};
  const filters = parseEventFilters("events_tail", args, context.client);
  const maxEvents = optionalInteger(
    "events_tail",
    args,
    "maxEvents",
    Math.min(n, TAIL_DEFAULT_MAX_EVENTS),
    1,
    EVENTS_MAX_EVENTS_LIMIT,
  );
  const maxBytes = optionalInteger(
    "events_tail",
    args,
    "maxBytes",
    DEFAULT_MAX_BYTES,
    MIN_MAX_BYTES,
    MAX_MAX_BYTES,
  );
  const summary = optionalBoolean("events_tail", args, "summary", false);
  const replay = { value: false };

  if (summary) {
    const accumulator = makeAccumulator();

    for await (const batch of context.client.events({ sinceSequence: since, live: false })) {
      if (batch.replay) replay.value = true;

      for (const event of batch.events) {
        if (matchesFilters(event, filters)) accumulateEvent(accumulator, event);
      }
    }

    const next = new Map(nextSinceSeed(since));

    for (const [clock, sequence] of accumulator.lastSequence) {
      const current = next.get(clock);

      if (current === undefined || sequence > current) next.set(clock, sequence);
    }

    const { text } = buildSummaryPayload(
      (summaryJsonValue) => ({
        count: accumulator.matched,
        matched: accumulator.matched,
        summary: summaryJsonValue,
        dropped: droppedToJson(context.client),
        cursors: cursorsToJson(context.client.cursors),
        nextSince: nextSinceJson(next),
      }),
      accumulator,
      maxBytes,
    );

    return { content: [{ type: "text", text }] };
  }

  // A bounded ring keeps the last `min(n, maxEvents)` filtered events; the
  // stream itself is still scanned so `matched` reports what was dropped.
  const cap = Math.min(n, maxEvents);
  const ring: TraceEvent[] = [];
  let matched = 0;

  for await (const batch of context.client.events({ sinceSequence: since, live: false })) {
    if (batch.replay) replay.value = true;

    for (const event of batch.events) {
      if (!matchesFilters(event, filters)) continue;

      matched += 1;
      ring.push(event);

      if (ring.length > cap) ring.shift();
    }
  }

  const seed = nextSinceSeed(since);
  const { text } = trimEventPayload(
    (events) => ({
      count: events.length,
      matched,
      truncated: matched > events.length,
      events: events.map(eventToJson),
      dropped: droppedToJson(context.client),
      cursors: cursorsToJson(context.client.cursors),
      replay: replay.value,
      nextSince: nextSinceJson(advanceNextSince(seed, events)),
    }),
    ring,
    maxBytes,
    "front",
  );

  return { content: [{ type: "text", text }] };
}

async function eventsWatch(args: Record<string, unknown>, context: ToolContext): Promise<ToolResult> {
  checkAllowed("events_watch", args, [
    "since",
    "timeout",
    "clock",
    "nodeId",
    "kind",
    "maxEvents",
    "maxBytes",
    "summary",
  ]);

  const explicitSince = optionalSince("events_watch", args);
  const page = resolveWatchSince(context.client, explicitSince);
  const sinceEcho = cursorsToJson(Object.fromEntries(page.effective));
  const timeoutMs = optionalInteger("events_watch", args, "timeout", 2_000, 1, 60_000);
  const filters = parseEventFilters("events_watch", args, context.client);
  const maxEvents = optionalInteger(
    "events_watch",
    args,
    "maxEvents",
    WATCH_DEFAULT_MAX_EVENTS,
    1,
    EVENTS_MAX_EVENTS_LIMIT,
  );
  const maxBytes = optionalInteger(
    "events_watch",
    args,
    "maxBytes",
    DEFAULT_MAX_BYTES,
    MIN_MAX_BYTES,
    MAX_MAX_BYTES,
  );
  const summary = optionalBoolean("events_watch", args, "summary", false);
  const seed = new Map(page.effective);
  const abort = new AbortController();
  let timedOut = false;
  let settleTimer: ReturnType<typeof setTimeout> | null = null;

  const timer = setTimeout(() => {
    timedOut = true;
    abort.abort();
  }, timeoutMs);

  if (summary) {
    const accumulator = makeAccumulator();

    try {
      for await (const batch of context.client.events({
        sinceSequence: page.sinceSequence,
        live: true,
        signal: abort.signal,
      })) {
        for (const event of batch.events) {
          if (matchesFilters(event, filters)) accumulateEvent(accumulator, event);
        }

        if (settleTimer === null) settleTimer = setTimeout(() => abort.abort(), WATCH_SETTLE_MS);
      }
    } catch (error) {
      if ((error as Error).name !== "AbortError") throw error;
    } finally {
      clearTimeout(timer);
      if (settleTimer !== null) clearTimeout(settleTimer);
    }

    const next = new Map(seed);

    for (const [clock, sequence] of accumulator.lastSequence) {
      const current = next.get(clock);

      if (current === undefined || sequence > current) next.set(clock, sequence);
    }

    const { text, truncated } = buildSummaryPayload(
      (summaryJsonValue) => ({
        count: accumulator.matched,
        matched: accumulator.matched,
        timedOut: accumulator.matched === 0 && timedOut,
        autoPaged: page.autoPaged,
        since: sinceEcho,
        summary: summaryJsonValue,
        dropped: droppedToJson(context.client),
        cursors: cursorsToJson(context.client.cursors),
        nextSince: nextSinceJson(next),
      }),
      accumulator,
      maxBytes,
    );

    updateAutoPaging(context.client, truncated, next);

    return { content: [{ type: "text", text }] };
  }

  // Collect the delta until the count cap, then stop consuming the iterator:
  // the remainder is reachable through `since: nextSince`.
  const events: TraceEvent[] = [];
  let matched = 0;
  let capped = false;

  try {
    outer: for await (const batch of context.client.events({
      sinceSequence: page.sinceSequence,
      live: true,
      signal: abort.signal,
    })) {
      for (const event of batch.events) {
        if (!matchesFilters(event, filters)) continue;

        matched += 1;
        events.push(event);

        if (events.length >= maxEvents) {
          capped = true;
          abort.abort();
          break outer;
        }
      }

      if (settleTimer === null) settleTimer = setTimeout(() => abort.abort(), WATCH_SETTLE_MS);
    }
  } catch (error) {
    if ((error as Error).name !== "AbortError") throw error;
  } finally {
    clearTimeout(timer);
    if (settleTimer !== null) clearTimeout(settleTimer);
  }

  const trimmed = trimEventPayload(
    (kept) => ({
      count: kept.length,
      matched,
      truncated: capped || matched > kept.length,
      timedOut: matched === 0 && timedOut,
      autoPaged: page.autoPaged,
      since: sinceEcho,
      events: kept.map(eventToJson),
      dropped: droppedToJson(context.client),
      cursors: cursorsToJson(context.client.cursors),
      nextSince: nextSinceJson(advanceNextSince(seed, kept)),
    }),
    events,
    maxBytes,
    "rear",
  );

  const truncated = capped || matched > trimmed.events.length;

  updateAutoPaging(context.client, truncated, advanceNextSince(seed, trimmed.events));

  return { content: [{ type: "text", text: trimmed.text }] };
}

async function exampleList(args: Record<string, unknown>, context: ToolContext): Promise<ToolResult> {
  checkAllowed("example_list", args, []);
  const list = await context.client.examples();

  return jsonText({ active: activeExample(context.client) || list.active, names: list.names });
}

async function exampleSelect(args: Record<string, unknown>, context: ToolContext): Promise<ToolResult> {
  checkAllowed("example_select", args, ["name"]);
  const name = requireString("example_select", args, "name");

  await context.client.selectExample(name);
  const graph = await ensureGraph(context.client);

  // A page cursor belongs to the example it was recorded on.
  clearAutoPaging();

  return jsonText({
    selected: name,
    active: graph.name,
    graph: {
      nodes: graph.nodes.length,
      inputs: graph.inputs.map((port) => port.name),
      outputs: graph.outputs.map((port) => port.name),
    },
  });
}

async function inputSet(args: Record<string, unknown>, context: ToolContext): Promise<ToolResult> {
  checkAllowed("input_set", args, ["port", "value", "timeoutMs"]);
  const port = requireString("input_set", args, "port");
  const value = requiredValue("input_set", args);
  const timeoutMs = optionalTimeoutMs("input_set", args, 5_000);
  const result = await context.client.setInput(port, value, { timeoutMs });

  return jsonText({
    port,
    requestId: bigintToJson(result.requestId),
    appliedEpoch: bigintToJson(result.appliedEpoch),
  });
}

async function inputGet(args: Record<string, unknown>, context: ToolContext): Promise<ToolResult> {
  checkAllowed("input_get", args, ["port", "timeoutMs"]);
  const port = requireString("input_get", args, "port");
  const timeoutMs = optionalTimeoutMs("input_get", args, 5_000);
  const snapshot = await context.client.getInput(port, { timeoutMs });

  return jsonText({
    requestId: bigintToJson(snapshot.requestId),
    port: snapshot.port === "" ? port : snapshot.port,
    value: valueToJson(snapshot.value),
  });
}

async function outputGet(args: Record<string, unknown>, context: ToolContext): Promise<ToolResult> {
  checkAllowed("output_get", args, ["port", "timeoutMs"]);
  const port = requireString("output_get", args, "port");
  const timeoutMs = optionalTimeoutMs("output_get", args, 5_000);
  const snapshot = await context.client.getOutput(port, { timeoutMs });

  return jsonText({
    requestId: bigintToJson(snapshot.requestId),
    port: snapshot.port === "" ? port : snapshot.port,
    value: valueToJson(snapshot.value),
  });
}

async function pause(args: Record<string, unknown>, context: ToolContext): Promise<ToolResult> {
  checkAllowed("pause", args, []);
  await context.client.pause();

  return jsonText({ paused: true });
}

async function resume(args: Record<string, unknown>, context: ToolContext): Promise<ToolResult> {
  checkAllowed("resume", args, []);
  await context.client.resume();

  return jsonText({ paused: false });
}

// --- schema fragments (shared by both event tools) -------------------------

const CLOCK_FILTER_SCHEMA = {
  oneOf: [
    { type: "string", minLength: 1 },
    { type: "array", items: { type: "string", minLength: 1 }, minItems: 1 },
  ],
  description:
    "Keep only events from these clock(s), e.g. \"audio\" or [\"audio\",\"ui\"]. " +
    "Validated against the live graph's clocks when it is known.",
} as const;

const NODE_ID_FILTER_SCHEMA = {
  oneOf: [
    { type: "integer", minimum: 0 },
    { type: "string", pattern: "^[0-9]+$" },
    {
      type: "array",
      minItems: 1,
      items: { oneOf: [{ type: "integer", minimum: 0 }, { type: "string", pattern: "^[0-9]+$" }] },
    },
  ],
  description:
    "Keep only events from these node id(s): a safe integer or decimal string " +
    "(node ids are uint64), or an array of those. Use graph_get to map an id to its label.",
} as const;

const KIND_FILTER_SCHEMA = {
  oneOf: [
    { type: "string", enum: [...EVENT_KINDS] },
    { type: "array", minItems: 1, items: { type: "string", enum: [...EVENT_KINDS] } },
  ],
  description: "Keep only these event kind(s).",
} as const;

const MAX_EVENTS_SCHEMA = (fallback: string) => ({
  type: "integer",
  minimum: 1,
  maximum: EVENTS_MAX_EVENTS_LIMIT,
  description:
    `Hard cap on the number of events returned (${fallback}). The result also reports ` +
    "`matched` and `truncated`; page the remainder with `since: nextSince`.",
});

const MAX_BYTES_SCHEMA = {
  type: "integer",
  minimum: MIN_MAX_BYTES,
  maximum: MAX_MAX_BYTES,
  default: DEFAULT_MAX_BYTES,
  description:
    "HARD bound on the serialized result text in bytes (default 262144 = 256 KiB; " +
    "hard range 1024..2097152). If the result would be bigger, events are dropped and " +
    "`truncated` is true; page with `since: nextSince`.",
} as const;

const SUMMARY_SCHEMA = {
  type: "boolean",
  default: false,
  description:
    "Return aggregates only (counts by clock/kind/node, epoch spans, throws) instead of " +
    "an events array. Cheap and bounded even over the whole retained history.",
} as const;

const SINCE_DESCRIPTION =
  'Per-clock sequence cursor, e.g. { "audio": 1200, "ui": 40 }. ' +
  "Pass a previous result's `nextSince` to page a truncated result (and its `cursors` " +
  "when you want everything the client has accepted). Omit it to start at the current head.";

// --- the table --------------------------------------------------------------

const VALUE_DESCRIPTION =
  "The input's value. A JSON number is a double (the client's Value mapping: " +
  "`112.0` is a double, never an int64); a boolean is a bool; a string is text. " +
  'For a lossless int64 write { "kind": "int", "value": 42 } (number or decimal string). ' +
  'A full mapping also accepts { "kind": "double", "value": 1.5 }, ' +
  '{ "kind": "bool", "value": true }, { "kind": "unit" } and { "kind": "absent" }.';

export const TOOLS: readonly Tool[] = [
  {
    name: "graph_get",
    description:
      "The live network's structure: nodes (kind, clock, label, port, fan-in), crossings and " +
      "their policy, boundary inputs/outputs, described user types, and the example name. " +
      "Call this first — it tells you which port names and clock names every other tool takes. " +
      "Also available as the MCP resource graph://<example>.",
    inputSchema: { type: "object", properties: {}, additionalProperties: false },
    handler: graphGet,
  },
  {
    name: "events_tail",
    description:
      "Tail of the recorded trace (most recent events), bounded by default to 200 events and " +
      "256 KiB of result text. Each event carries clock, node id, epoch, kind, produced value and " +
      "duration. Args: `n` (desired tail, default 50, max 2000), `since` (per-clock cursor), " +
      "filters `clock`/`nodeId`/`kind` (string or array), `maxEvents` (1..2000, default " +
      "min(n,200)), `maxBytes` (1024..2097152, default 262144) and `summary` (aggregates only, " +
      "no events). The result has `count`, `matched`, `truncated`, `cursors` and `nextSince`; " +
      "when `truncated` is true, page forwards with `since: nextSince` (not `cursors`).",
    inputSchema: {
      type: "object",
      properties: {
        n: {
          type: "integer",
          minimum: 1,
          maximum: EVENTS_MAX_EVENTS_LIMIT,
          default: 50,
          description: "Desired tail size (hard max 2000); the result is also capped by maxEvents/maxBytes.",
        },
        since: { type: "object", description: SINCE_DESCRIPTION },
        clock: CLOCK_FILTER_SCHEMA,
        nodeId: NODE_ID_FILTER_SCHEMA,
        kind: KIND_FILTER_SCHEMA,
        maxEvents: MAX_EVENTS_SCHEMA("default min(n, 200)"),
        maxBytes: MAX_BYTES_SCHEMA,
        summary: SUMMARY_SCHEMA,
      },
      additionalProperties: false,
    },
    handler: eventsTail,
  },
  {
    name: "events_watch",
    description:
      "Wait for the next trace delta: the retained delta after `since` if there is one, " +
      "otherwise the next live batch, plus a short settle to gather the same tick's clocks. " +
      "Bounded by default to 200 events and 256 KiB of result text; a capped watch stops " +
      "consuming the stream and returns `nextSince` so nothing is silently skipped. If `since` " +
      "is omitted after a truncated page the server continues from that page automatically " +
      "(`autoPaged: true`, and the cursor used is echoed as `since`); pass an explicit `since` " +
      "(e.g. {}) to override. Args: " +
      "`since`, `timeout` (ms, default 2000), filters `clock`/`nodeId`/`kind` (string or array), " +
      "`maxEvents` (1..2000, default 200), `maxBytes` (1024..2097152, default 262144) and " +
      "`summary` (aggregates only). Result: `count`, `matched`, `truncated`, `timedOut` " +
      "(`true` only when nothing matched in time), `cursors`, `nextSince`; page with " +
      "`since: nextSince` (not `cursors`).",
    inputSchema: {
      type: "object",
      properties: {
        since: { type: "object", description: SINCE_DESCRIPTION },
        timeout: {
          type: "integer",
          minimum: 1,
          maximum: 60_000,
          default: 2_000,
          description: "Maximum milliseconds to wait for a delta.",
        },
        clock: CLOCK_FILTER_SCHEMA,
        nodeId: NODE_ID_FILTER_SCHEMA,
        kind: KIND_FILTER_SCHEMA,
        maxEvents: MAX_EVENTS_SCHEMA("default 200"),
        maxBytes: MAX_BYTES_SCHEMA,
        summary: SUMMARY_SCHEMA,
      },
      additionalProperties: false,
    },
    handler: eventsWatch,
  },
  {
    name: "example_list",
    description: "The examples the host can switch to, and which one is live.",
    inputSchema: { type: "object", properties: {}, additionalProperties: false },
    handler: exampleList,
  },
  {
    name: "example_select",
    description:
      "Switch the live example (the server broadcasts the new graph to every client) and " +
      "return its summary. Afterwards read the new ports with graph_get — the trace and the " +
      "write-back targets belong to the newly active example. Per-example addressing is not " +
      "implemented: this only works on the active example.",
    inputSchema: {
      type: "object",
      properties: { name: { type: "string", minLength: 1, description: "Example name, from example_list." } },
      required: ["name"],
      additionalProperties: false,
    },
    handler: exampleSelect,
  },
  {
    name: "input_set",
    description:
      "Write a value to a boundary input. The command is queued and applied by the host's tick " +
      "loop immediately before its next step — the semantically correct boundary for a clocked " +
      "program — so the result is the epoch it applied at (`appliedEpoch`, 0 means an error " +
      "ack). The effect shows up in the trace as an event with an epoch >= appliedEpoch: read " +
      "it back with events_watch / events_tail, or snapshot it with input_get / output_get. " +
      "An unknown port or an unsupported value kind is a tool error, not a protocol error.",
    inputSchema: {
      type: "object",
      properties: {
        port: { type: "string", minLength: 1, description: "Boundary input port name, from graph_get." },
        value: {
          description: VALUE_DESCRIPTION,
          oneOf: [
            { type: "boolean" },
            { type: "number" },
            { type: "string" },
            { type: "object" },
            { type: "null" },
          ],
        },
        timeoutMs: {
          type: "integer",
          minimum: 1,
          maximum: 60_000,
          description:
            "How long to wait for the host to apply the write (default 5000). A timeout means " +
            "the target region is probably not ticking; the write may still apply later.",
        },
      },
      required: ["port", "value"],
      additionalProperties: false,
    },
    handler: inputSet,
  },
  {
    name: "input_get",
    description:
      "Snapshot a boundary input, read at the host's next tick boundary (a read is host-loop " +
      "work too, never a server-thread peek). Returns the value in the client's Value mapping.",
    inputSchema: {
      type: "object",
      properties: {
        port: { type: "string", minLength: 1, description: "Boundary input port name." },
        timeoutMs: { type: "integer", minimum: 1, maximum: 60_000, description: "Default 5000." },
      },
      required: ["port"],
      additionalProperties: false,
    },
    handler: inputGet,
  },
  {
    name: "output_get",
    description:
      "Snapshot a boundary output's value from the last completed tick, read at the host's next " +
      "tick boundary. Use this to confirm the effect of an input_set without scanning the trace.",
    inputSchema: {
      type: "object",
      properties: {
        port: { type: "string", minLength: 1, description: "Boundary output port name." },
        timeoutMs: { type: "integer", minimum: 1, maximum: 60_000, description: "Default 5000." },
      },
      required: ["port"],
      additionalProperties: false,
    },
    handler: outputGet,
  },
  {
    name: "pause",
    description:
      "Gate the trace capture off (the wire stays connected). Ticks keep running; they are " +
      "just not recorded, so pausing gives you a stable window to inspect. Pair with resume.",
    inputSchema: { type: "object", properties: {}, additionalProperties: false },
    handler: pause,
  },
  {
    name: "resume",
    description: "Gate the trace capture back on after pause.",
    inputSchema: { type: "object", properties: {}, additionalProperties: false },
    handler: resume,
  },
];

export const TOOL_BY_NAME: ReadonlyMap<string, Tool> = new Map(TOOLS.map((tool) => [tool.name, tool]));

/** Serialisable `tools/list` view (handlers are not advertised). */
export function toolsList(): Array<Pick<Tool, "name" | "description" | "inputSchema">> {
  return TOOLS.map(({ name, description, inputSchema }) => ({ name, description, inputSchema }));
}

/**
 * Dispatch one `tools/call`. Argument mistakes become `InvalidParamsError`
 * (JSON-RPC -32602); anything the debugger rejects becomes an `isError` tool
 * result so the model can read the failure and try something else.
 */
export async function callTool(
  client: DebugClientLike,
  params: Record<string, unknown>,
): Promise<ToolResult> {
  const name = params.name;

  if (typeof name !== "string" || name === "") {
    throw new JsonRpcError(INVALID_PARAMS, 'tools/call: missing required "name"');
  }

  const tool = TOOL_BY_NAME.get(name);

  if (tool === undefined) {
    throw new JsonRpcError(INVALID_PARAMS, `Unknown tool: ${name}`);
  }

  const rawArgs = params.arguments ?? {};

  if (rawArgs === null || typeof rawArgs !== "object" || Array.isArray(rawArgs)) {
    throw new JsonRpcError(INVALID_PARAMS, "tools/call: arguments must be an object");
  }

  try {
    return await tool.handler(rawArgs as Record<string, unknown>, { client });
  } catch (error) {
    // Bad arguments are a protocol error the model should see as -32602, not a
    // tool result: retrying the same call cannot succeed. Everything else the
    // debugger rejects is a tool failure it can react to.
    if (error instanceof InvalidParamsError) throw new JsonRpcError(INVALID_PARAMS, error.message);
    if (error instanceof JsonRpcError) throw error;

    return toolFailure(error);
  }
}
