// clockr-debug-client — a dependency-free TypeScript client for the
// clockr debug WebSocket protocol (clockr.debug.v1).
//
// It is the observation/injection half of the debug-agent tooling: it
// attaches to a host serving `tools/web-debugger`, streams the live trace,
// and writes values back through the server's tick-boundary command queue. It
// deliberately knows nothing about MCP or any host integration — the MCP
// server and the dsh plugin are thin wrappers over this class.
//
// Threading model: the protocol is lock-free on the host side and
// message-delimited on the wire. This client is a thin event-loop consumer:
// frames are decoded in arrival order and fanned out to listeners, async
// iterators and pending requests. Nothing blocks, nothing polls.
//
// See README.md for the API reference and the Value mapping.

import {
  decodeFrame,
  encodePause,
  encodeReadInput,
  encodeReadOutput,
  encodeResume,
  encodeSelectExample,
  encodeSetInput,
} from "./schema.ts";

import type {
  Ack,
  EventBatchView,
  ExampleList,
  GraphDescription,
  PortValue,
  ServerFrame,
  TraceEvent,
  Value,
} from "./types.ts";

import { toValue, type ValueInput } from "./value.ts";

/** Connection state as reported by `client.state` and `onConnection`. */
export type ConnectionState = "idle" | "connecting" | "open" | "reconnecting" | "closed";

/** A structured error kind, so callers can branch without matching strings. */
export type ClientErrorCode =
  | "not_connected"
  | "timeout"
  | "ack"
  | "connect"
  | "closed"
  | "unknown_example"
  | "decode";

/** Base class for every error this client raises. */
export class DebugClientError extends Error {
  readonly code: ClientErrorCode;

  constructor(code: ClientErrorCode, message: string, options: { cause?: unknown } = {}) {
    super(message, options.cause === undefined ? undefined : { cause: options.cause });
    this.name = "DebugClientError";
    this.code = code;
  }
}

/** The request reached the server, which answered `Ack { error }` / `PortValue { error }`. */
export class AckError extends DebugClientError {
  readonly requestId: bigint;
  /** The wire error text (what the host wrote), verbatim. */
  readonly ackError: string;
  readonly port: string | undefined;

  constructor(requestId: bigint, ackError: string, port?: string) {
    super("ack", `request ${requestId}${port === undefined ? "" : ` (${port})`} failed: ${ackError}`);
    this.name = "AckError";
    this.requestId = requestId;
    this.ackError = ackError;
    this.port = port;
  }
}

/** No reply arrived within the configured window; the request may still apply later. */
export class RequestTimeoutError extends DebugClientError {
  readonly requestId: bigint;
  readonly method: string;
  readonly port: string;
  readonly timeoutMs: number;

  constructor(requestId: bigint, method: string, port: string, timeoutMs: number) {
    super(
      "timeout",
      `${method}("${port}") timed out after ${timeoutMs} ms ` +
        `(request_id ${requestId}); the host may still apply it if its tick loop drains late`,
    );
    this.name = "RequestTimeoutError";
    this.requestId = requestId;
    this.method = method;
    this.port = port;
    this.timeoutMs = timeoutMs;
  }
}

/** A request was attempted while the socket is not open (idle, reconnecting, closed). */
export class NotConnectedError extends DebugClientError {
  constructor(detail = "socket is not open") {
    super("not_connected", `not connected: ${detail}`);
    this.name = "NotConnectedError";
  }
}

/** `connect()` could not reach the server (or the client was closed mid-connect). */
export class ConnectError extends DebugClientError {
  constructor(message: string, options: { cause?: unknown } = {}) {
    super("connect", message, options);
    this.name = "ConnectError";
  }
}

/** The client was closed by its owner; every pending request is rejected with this. */
export class ClientClosedError extends DebugClientError {
  constructor() {
    super("closed", "client is closed");
    this.name = "ClientClosedError";
  }
}

// --- WebSocket boundary -----------------------------------------------------
//
// Node's global WebSocket (undici, node >= 22) is what we use in production.
// The tiny interface below is also the seam a test injects a fake socket
// through, so the reconnect/history-replay logic is unit-testable without a
// live C++ host.

/** The slice of the WebSocket API this client uses. */
export interface WebSocketLike {
  binaryType: string;
  send(data: Uint8Array): void;
  close(code?: number, reason?: string): void;
  addEventListener(type: "open", listener: (event: unknown) => void): void;
  addEventListener(type: "message", listener: (event: { data: unknown }) => void): void;
  addEventListener(type: "close", listener: (event: CloseEventLike) => void): void;
  addEventListener(type: "error", listener: (event: unknown) => void): void;
}

/** The fields of the socket's `close` event this client reads. */
export interface CloseEventLike {
  code?: number;
  reason?: string;
}

/** Construct a WebSocket-like object for `url`; the test seam for the client. */
export type WebSocketFactory = (url: string) => WebSocketLike;

const defaultWebSocketFactory: WebSocketFactory = (url) => {
  if (typeof WebSocket === "undefined") {
    throw new Error("no global WebSocket in this runtime (node >= 22 provides one); pass options.webSocketFactory");
  }

  return new WebSocket(url) as unknown as WebSocketLike;
};

// --- public option/result types --------------------------------------------

/** Options accepted by `createClient(url, options)`. */
export interface ClientOptions {
  /** Reconnect automatically after an unexpected socket close (default true). */
  reconnect?: boolean;
  /** First reconnect delay; doubles per attempt up to `reconnectMaxDelayMs`. */
  reconnectBaseDelayMs?: number;
  reconnectMaxDelayMs?: number;
  /** How long a request waits for its Ack/PortValue before rejecting (default 5000). */
  requestTimeoutMs?: number;
  /** How long the first `connect()` waits for a GraphDescription (default 10000). */
  connectTimeoutMs?: number;
  /** Events retained per clock for `events({ sinceSequence })` replay (default 10000). */
  historyLimit?: number;
  /** How quiet the connect-time history replay must be to count as drained (default 150). */
  replaySettleMs?: number;
  /** The WebSocket constructor; override in tests. */
  webSocketFactory?: WebSocketFactory;
  /** Verbose protocol logging (default: silent). */
  debug?: (message: string) => void;
}

/** Options for `events()`. */
export interface EventsOptions {
  /**
   * Per-clock sequence cursor: yield events with `sequence > since[clock]`.
   * Defaults to the client's read cursor (advanced by every event already
   * yielded to an `events()` consumer), i.e. "everything since my last read".
   */
  sinceSequence?: Record<string, bigint | number>;
  /** Keep streaming after the history replay (default true). */
  live?: boolean;
  /** Stop iterating when aborted. */
  signal?: AbortSignal;
}

/** Extra knobs shared by the request-style methods. */
export interface RequestOptions {
  /** Per-request timeout override (ms). */
  timeoutMs?: number;
  /** Example name to pass through; absent targets the server's active example. */
  example?: string;
}

/** The reply to `setInput` (and a successfully applied write command). */
export interface SetInputResult {
  requestId: bigint;
  /** The epoch at which the host applied the value; always > 0 on success. */
  appliedEpoch: bigint;
}

/** Connection-state notification (`onConnection`). */
export interface ConnectionInfo {
  state: ConnectionState;
  /** Reconnect attempt number: 0 while connected, 1+ while backing off. */
  attempt: number;
  code?: number;
  reason?: string;
}

/** Drop accounting, surfaced from EventBatch.dropped and CommandStats.dropped. */
export interface DropStats {
  /** Dropped trace events, by clock. */
  events: Record<string, bigint>;
  /** Dropped injection commands (the server counts them; a dropped command is never acked). */
  commands: bigint;
}

// --- async-iterator plumbing ------------------------------------------------

const REPLAY_DONE = Symbol("replay-done");

interface Subscriber {
  since: Map<string, bigint>;
  queue: EventBatchView[];
  waiters: Array<(batch: EventBatchView | null) => void>;
  ended: boolean;
}

function pushToSubscriber(subscriber: Subscriber, batch: EventBatchView): void {
  if (subscriber.ended) return;

  // Hand the batch to a parked iterator, or queue it for the next `next()`
  // call — never both: doing both delivered every batch twice.
  const waiter = subscriber.waiters.shift();

  if (waiter !== undefined) {
    waiter(batch);
    return;
  }

  subscriber.queue.push(batch);
}

function subscriberNext(subscriber: Subscriber, signal?: AbortSignal): Promise<EventBatchView | null> {
  if (subscriber.queue.length > 0) return Promise.resolve(subscriber.queue.shift() ?? null);
  if (subscriber.ended) return Promise.resolve(null);

  return new Promise((resolve, reject) => {
    const waiter = (batch: EventBatchView | null) => {
      cleanup();
      resolve(batch);
    };

    const onAbort = () => {
      cleanup();
      const error = new Error("events() aborted");
      error.name = "AbortError";
      reject(error);
    };

    const cleanup = () => {
      const index = subscriber.waiters.indexOf(waiter);

      if (index >= 0) subscriber.waiters.splice(index, 1);
      signal?.removeEventListener("abort", onAbort);
    };

    subscriber.waiters.push(waiter);
    signal?.addEventListener("abort", onAbort, { once: true });
  });
}

async function subscriberNextOrReplayDone(
  subscriber: Subscriber,
  replayDone: Promise<void>,
  signal?: AbortSignal,
): Promise<EventBatchView | null | typeof REPLAY_DONE> {
  if (subscriber.queue.length > 0) return subscriber.queue.shift() ?? null;
  if (subscriber.ended) return null;

  let waiter: ((batch: EventBatchView | null) => void) | null = null;

  const pushed = new Promise<EventBatchView | null>((resolve) => {
    waiter = (batch) => resolve(batch);
    subscriber.waiters.push(waiter);
  });

  const onAbort = () => waiter?.(null);

  signal?.addEventListener("abort", onAbort, { once: true });

  try {
    const result = await Promise.race([pushed, replayDone.then(() => REPLAY_DONE)]);

    if (result === REPLAY_DONE) {
      // A batch may have arrived in the same tick the replay settled; serve it.
      return subscriber.queue.shift() ?? REPLAY_DONE;
    }

    return result;
  } finally {
    if (waiter !== null) {
      const index = subscriber.waiters.indexOf(waiter);

      if (index >= 0) subscriber.waiters.splice(index, 1);
    }

    signal?.removeEventListener("abort", onAbort);
  }
}

// --- client -----------------------------------------------------------------

interface PendingRequest {
  kind: "write" | "read";
  method: string;
  port: string;
  resolve: (value: never) => void;
  reject: (error: Error) => void;
  timer: ReturnType<typeof setTimeout> | null;
}

interface GraphWaiter {
  predicate: (graph: GraphDescription) => boolean;
  resolve: (graph: GraphDescription) => void;
  reject: (error: Error) => void;
  timer: ReturnType<typeof setTimeout> | null;
}

interface ExamplesWaiter {
  resolve: (list: ExampleList) => void;
  reject: (error: Error) => void;
  timer: ReturnType<typeof setTimeout> | null;
}

interface Deferred<T> {
  promise: Promise<T>;
  resolve: (value: T) => void;
  reject: (error: Error) => void;
}

function deferred<T>(): Deferred<T> {
  let resolve!: (value: T) => void;
  let reject!: (error: Error) => void;

  const promise = new Promise<T>((res, rej) => {
    resolve = res;
    reject = rej;
  });

  return { promise, resolve, reject };
}

/** The live debug client. Construct via `createClient`. */
export class DebugClient {
  readonly url: string;

  #options: Required<Omit<ClientOptions, "webSocketFactory" | "debug">> & {
    webSocketFactory: WebSocketFactory;
    debug: (message: string) => void;
  };

  #ws: WebSocketLike | null = null;
  #state: ConnectionState = "idle";
  #closedByUser = false;
  #everConnected = false;
  #socketGeneration = 0;
  #reconnectAttempt = 0;
  #reconnectTimer: ReturnType<typeof setTimeout> | null = null;

  #graph: GraphDescription | null = null;
  #examples: ExampleList | null = null;

  /** Per-clock highest sequence number accepted so far (the dedupe frontier). */
  #lastSequence = new Map<string, bigint>();
  /** Per-clock retained events, oldest first, capped at historyLimit. */
  #history = new Map<string, TraceEvent[]>();
  /** Per-clock total of dropped trace events reported by the server. */
  #dropped = new Map<string, bigint>();
  #commandDropped = 0n;
  /** Per-clock read cursor: highest sequence yielded to an `events()` consumer. */
  #readCursor = new Map<string, bigint>();

  #pending = new Map<bigint, PendingRequest>();
  #nextRequestId = 1n;
  #graphWaiters: GraphWaiter[] = [];
  #examplesWaiters: ExamplesWaiter[] = [];
  #subscribers = new Set<Subscriber>();
  #graphListeners = new Set<(graph: GraphDescription) => void>();
  #batchListeners = new Set<(batch: EventBatchView) => void>();
  #examplesListeners = new Set<(list: ExampleList) => void>();
  #connectionListeners = new Set<(info: ConnectionInfo) => void>();
  #statsListeners = new Set<(stats: DropStats) => void>();
  #decodeErrors = 0;
  #orphanAcks = 0;

  #replayPending = false;
  #replayDeferred: Deferred<void> | null = null;
  #replayTimer: ReturnType<typeof setTimeout> | null = null;

  #frameChain: Promise<void> = Promise.resolve();

  constructor(url = "ws://127.0.0.1:9002/ws", options: ClientOptions = {}) {
    this.url = url;
    this.#options = {
      reconnect: options.reconnect ?? true,
      reconnectBaseDelayMs: options.reconnectBaseDelayMs ?? 100,
      reconnectMaxDelayMs: options.reconnectMaxDelayMs ?? 2000,
      requestTimeoutMs: options.requestTimeoutMs ?? 5000,
      connectTimeoutMs: options.connectTimeoutMs ?? 10000,
      historyLimit: options.historyLimit ?? 10000,
      replaySettleMs: options.replaySettleMs ?? 150,
      webSocketFactory: options.webSocketFactory ?? defaultWebSocketFactory,
      debug: options.debug ?? (() => {}),
    };
  }

  // --- observation ---------------------------------------------------------

  get state(): ConnectionState {
    return this.#state;
  }

  /** The most recent GraphDescription (null before the first connects). */
  get graph(): GraphDescription | null {
    return this.#graph;
  }

  /** The live example name (graph name, else the examples list's active). */
  get currentExample(): string {
    if (this.#graph !== null && this.#graph.name !== "") return this.#graph.name;

    return this.#examples?.active ?? "";
  }

  /** The most recent ExampleList (null before the first arrives). */
  get examplesList(): ExampleList | null {
    return this.#examples;
  }

  /** Highest sequence accepted per clock — pass back as `sinceSequence`. */
  get cursors(): Record<string, bigint> {
    return Object.fromEntries(this.#lastSequence);
  }

  /** Drop accounting from EventBatch.dropped and CommandStats.dropped. */
  get dropped(): DropStats {
    return { events: Object.fromEntries(this.#dropped), commands: this.#commandDropped };
  }

  /** Frames whose bytes did not decode (protocol mismatch; surfaced, not fatal). */
  get decodeErrors(): number {
    return this.#decodeErrors;
  }

  /** Acks/PortValues that matched no pending request (late replies, example switches). */
  get orphanAcks(): number {
    return this.#orphanAcks;
  }

  get pendingRequestCount(): number {
    return this.#pending.size;
  }

  onGraph(listener: (graph: GraphDescription) => void): () => void {
    this.#graphListeners.add(listener);

    return () => this.#graphListeners.delete(listener);
  }

  /** Every accepted (deduped) batch, including the connect-time history replay. */
  onEventBatch(listener: (batch: EventBatchView) => void): () => void {
    this.#batchListeners.add(listener);

    return () => this.#batchListeners.delete(listener);
  }

  onExamples(listener: (list: ExampleList) => void): () => void {
    this.#examplesListeners.add(listener);

    return () => this.#examplesListeners.delete(listener);
  }

  onConnection(listener: (info: ConnectionInfo) => void): () => void {
    this.#connectionListeners.add(listener);

    return () => this.#connectionListeners.delete(listener);
  }

  onStats(listener: (stats: DropStats) => void): () => void {
    this.#statsListeners.add(listener);

    return () => this.#statsListeners.delete(listener);
  }

  // --- lifecycle -----------------------------------------------------------

  /**
   * Open the socket (idempotent) and resolve with the network's
   * GraphDescription, the first thing the server sends. Auto-reconnects in the
   * background afterwards; `onGraph` fires again for every later description
   * (example switches, reconnects).
   */
  async connect(): Promise<GraphDescription> {
    if (this.#closedByUser) throw new ClientClosedError();
    if (this.#graph !== null && this.#state === "open") return this.#graph;

    if (this.#state === "closed" && !this.#options.reconnect) {
      throw new ConnectError("connection is closed and reconnect is disabled");
    }

    const promise = this.#waitForGraph(() => true, this.#options.connectTimeoutMs);

    if (this.#state === "idle") this.#openSocket();

    return promise;
  }

  /** Close the socket and stop reconnecting; pending requests are rejected. */
  close(): void {
    if (this.#closedByUser) return;

    this.#closedByUser = true;
    this.#clearReconnectTimer();
    this.#markReplayDone();
    this.#state = "closed";

    const socket = this.#ws;

    this.#ws = null;
    this.#socketGeneration += 1;

    if (socket !== null) {
      try {
        socket.close(1000, "client closed");
      } catch {
        // A socket already closing/closed throws; nothing to do.
      }
    }

    this.#failPending(new ClientClosedError());
    this.#failGraphWaiters(new ClientClosedError());
    this.#failExamplesWaiters(new ClientClosedError());
    this.#notifyConnection({ state: "closed", attempt: 0, code: 1000, reason: "client closed" });

    for (const subscriber of this.#subscribers) {
      subscriber.ended = true;

      for (const waiter of subscriber.waiters.splice(0)) waiter(null);
    }

    this.#subscribers.clear();
  }

  // --- trace stream --------------------------------------------------------

  /**
   * Async iterable of trace batches — the read side of the debug loop.
   *
   * The iterator first yields the client's retained history for the requested
   * `sinceSequence` (per clock), then — with `live !== false` — every new batch
   * as it arrives, surviving reconnects transparently. Each yielded batch is
   * `{ clock, dropped, events, replay }`; `dropped` is the producer-side loss
   * count for that clock and `replay` marks the connect-time history tail.
   *
   * By default `sinceSequence` is the client's read cursor, so the second call
   * picks up exactly where the first stopped. Pass explicit cursors to replay
   * from the retained history (or from the server's connect-time tail).
   *
   * ```js
   * for await (const batch of client.events({ live: false })) {
   *   for (const event of batch.events) console.log(event.clock, event.kind, event.value);
   * }
   * ```
   */
  async *events(options: EventsOptions = {}): AsyncGenerator<EventBatchView, void, void> {
    const live = options.live !== false;
    const signal = options.signal;
    const since = new Map<string, bigint>();

    if (options.sinceSequence !== undefined) {
      for (const [clock, sequence] of Object.entries(options.sinceSequence)) {
        since.set(clock, BigInt(sequence));
      }
    } else {
      for (const [clock, sequence] of this.#readCursor) since.set(clock, sequence);
    }

    const subscriber: Subscriber = { since: new Map(since), queue: [], waiters: [], ended: false };

    this.#subscribers.add(subscriber);

    try {
      // 1. The client-retained history: everything since the requested cursor
      //    that already arrived (the server's connect-time replay included).
      for (const batch of this.#historySince(since)) {
        if (signal?.aborted) return;

        this.#noteDelivered(subscriber, batch);
        yield batch;
      }

      // 2. Live subscription: batches arriving from now on. The subscriber's
      //    `since` was advanced over the snapshot above, so no event repeats.
      if (!live) {
        const replayDone = this.#waitReplayDone();

        for (;;) {
          const result = await subscriberNextOrReplayDone(subscriber, replayDone, signal);

          if (result === REPLAY_DONE || result === null) return;
          if (signal?.aborted) return;

          this.#noteDelivered(subscriber, result);
          yield result;
        }
      }

      for (;;) {
        if (signal?.aborted) return;

        const batch = await subscriberNext(subscriber, signal);

        if (batch === null) return;

        this.#noteDelivered(subscriber, batch);
        yield batch;
      }
    } catch (error) {
      if ((error as Error).name === "AbortError") return;
      throw error;
    } finally {
      subscriber.ended = true;

      for (const waiter of subscriber.waiters.splice(0)) waiter(null);

      this.#subscribers.delete(subscriber);
    }
  }

  // --- commands: write-back and read-back ---------------------------------

  /**
   * Write a value to a boundary input. Resolves with the epoch the host applied
   * it at once the tick loop drains the command queue; rejects with `AckError`
   * (unknown port, host validation failure) or `RequestTimeoutError` (the
   * region is not ticking, or the reply was lost). Never silently unacks.
   */
  async setInput(port: string, value: ValueInput, options: RequestOptions = {}): Promise<SetInputResult> {
    const wireValue: Value = toValue(value);

    return (await this.#request("write", "setInput", port, (requestId) =>
      encodeSetInput({ requestId, port, value: wireValue, example: options.example }), options)) as SetInputResult;
  }

  /** Snapshot a boundary input (answered by the host's tick loop). */
  async getInput(port: string, options: RequestOptions = {}): Promise<PortValue> {
    return (await this.#request("read", "getInput", port, (requestId) =>
      encodeReadInput({ requestId, port, example: options.example }), options)) as PortValue;
  }

  /** Snapshot a boundary output (answered by the host's tick loop). */
  async getOutput(port: string, options: RequestOptions = {}): Promise<PortValue> {
    return (await this.#request("read", "getOutput", port, (requestId) =>
      encodeReadOutput({ requestId, port, example: options.example }), options)) as PortValue;
  }

  /** The cached example list; waits for the first ExampleList if necessary. */
  async examples(): Promise<ExampleList> {
    if (this.#examples !== null) return this.#examples;
    if (this.#closedByUser) throw new ClientClosedError();

    return new Promise<ExampleList>((resolve, reject) => {
      const waiter: ExamplesWaiter = { resolve, reject, timer: null };

      if (this.#options.requestTimeoutMs > 0) {
        waiter.timer = setTimeout(() => {
          this.#examplesWaiters = this.#examplesWaiters.filter((entry) => entry !== waiter);
          reject(new RequestTimeoutError(0n, "examples", "*", this.#options.requestTimeoutMs));
        }, this.#options.requestTimeoutMs);
      }

      this.#examplesWaiters.push(waiter);
    });
  }

  /**
   * Switch the live example. Resolves when the new GraphDescription arrives
   * (the server broadcasts it to every client); rejects for an unknown name
   * when the cached example list already disproves it.
   */
  async selectExample(name: string): Promise<void> {
    if (this.#closedByUser) throw new ClientClosedError();

    if (this.#graph !== null && this.#graph.name === name) return;

    const known = this.#examples?.names;

    if (known !== undefined && known.length > 0 && !known.includes(name)) {
      throw new DebugClientError("unknown_example", `unknown example "${name}" (known: ${known.join(", ")})`);
    }

    if (this.#state !== "open" || this.#ws === null) throw new NotConnectedError("selectExample");

    const waiting = this.#waitForGraph((graph) => graph.name === name, this.#options.requestTimeoutMs);

    try {
      this.#send(encodeSelectExample(name));
    } catch (error) {
      // The graph may already be in flight; let the waiter win if it resolves.
      this.#options.debug(`selectExample(${name}) send failed: ${String(error)}`);
    }

    await waiting;
  }

  /** Gate capture off (the host stops recording; the wire stays connected). */
  async pause(): Promise<void> {
    this.#send(encodePause());
  }

  /** Gate capture back on. */
  async resume(): Promise<void> {
    this.#send(encodeResume());
  }

  // --- socket lifecycle internals -----------------------------------------

  #openSocket(): void {
    if (this.#closedByUser) return;

    const generation = ++this.#socketGeneration;
    this.#state = this.#everConnected ? "reconnecting" : "connecting";

    let socket: WebSocketLike;

    try {
      socket = this.#options.webSocketFactory(this.url);
    } catch (error) {
      this.#debug(`socket construction failed: ${String(error)}`);
      this.#scheduleReconnect();
      return;
    }

    this.#ws = socket;

    try {
      socket.binaryType = "arraybuffer";
    } catch {
      // Some WebSocket implementations refuse the assignment; decode handles both.
    }

    socket.addEventListener("open", () => this.#onOpen(generation));
    socket.addEventListener("message", (event) => {
      this.#enqueueFrame(generation, event.data);
    });
    socket.addEventListener("close", (event) => this.#onClose(generation, event));
    socket.addEventListener("error", (event) => {
      this.#debug(`socket error: ${String((event as { message?: string } | null)?.message ?? "error")}`);
    });
  }

  #onOpen(generation: number): void {
    if (generation !== this.#socketGeneration || this.#closedByUser) return;

    this.#state = "open";
    this.#everConnected = true;
    this.#reconnectAttempt = 0;
    this.#beginReplayPhase();
    this.#debug("socket open");
    this.#notifyConnection({ state: "open", attempt: 0 });
  }

  #onClose(generation: number, event: CloseEventLike): void {
    if (generation !== this.#socketGeneration) return;

    this.#ws = null;
    this.#markReplayDone();

    if (this.#closedByUser) {
      this.#state = "closed";
      return;
    }

    const code = event?.code ?? 1006;
    const reason = event?.reason ?? "";

    this.#debug(`socket closed (${code}${reason === "" ? "" : `: ${reason}`})`);

    if (this.#options.reconnect) {
      this.#state = "reconnecting";
      this.#notifyConnection({ state: "reconnecting", attempt: this.#reconnectAttempt + 1, code, reason });
      this.#scheduleReconnect();
      return;
    }

    this.#state = "closed";
    const error = new ConnectError(`connection closed (${code}${reason === "" ? "" : `: ${reason}`}); reconnect is disabled`);

    this.#failPending(error);
    this.#failGraphWaiters(error);
    this.#failExamplesWaiters(error);
    this.#notifyConnection({ state: "closed", attempt: 0, code, reason });
  }

  #scheduleReconnect(): void {
    if (this.#closedByUser || this.#reconnectTimer !== null) return;

    this.#reconnectAttempt += 1;

    const delay = Math.min(
      this.#options.reconnectBaseDelayMs * 2 ** (this.#reconnectAttempt - 1),
      this.#options.reconnectMaxDelayMs,
    );

    this.#debug(`reconnecting in ${delay} ms (attempt ${this.#reconnectAttempt})`);

    this.#reconnectTimer = setTimeout(() => {
      this.#reconnectTimer = null;

      if (!this.#closedByUser && this.#ws === null) this.#openSocket();
    }, delay);
  }

  #clearReconnectTimer(): void {
    if (this.#reconnectTimer !== null) {
      clearTimeout(this.#reconnectTimer);
      this.#reconnectTimer = null;
    }
  }

  // --- frame ingestion ----------------------------------------------------

  #enqueueFrame(generation: number, data: unknown): void {
    // Preserve frame order even when a Blob has to be awaited.
    this.#frameChain = this.#frameChain
      .then(async () => {
        if (generation !== this.#socketGeneration || this.#closedByUser) return;

        const bytes = await toBytes(data);

        if (bytes === null) return;
        if (generation !== this.#socketGeneration) return;

        this.#handleFrame(bytes);
      })
      .catch((error: unknown) => {
        this.#decodeErrors += 1;
        this.#debug(`frame ingestion failed: ${String(error)}`);
      });
  }

  #handleFrame(bytes: Uint8Array): void {
    let frame: ServerFrame;

    try {
      frame = decodeFrame(bytes);
    } catch (error) {
      this.#decodeErrors += 1;
      this.#debug(`frame decode failed: ${String(error)}`);
      return;
    }

    if ("graph" in frame) this.#handleGraph(frame.graph);
    else if ("batch" in frame) this.#handleBatch(frame.batch);
    else if ("examples" in frame) this.#handleExamples(frame.examples);
    else if ("ack" in frame) this.#handleAck(frame.ack);
    else if ("portValue" in frame) this.#handlePortValue(frame.portValue);
    else if ("commandStats" in frame) this.#handleCommandStats(frame.commandStats);
  }

  #handleGraph(graph: GraphDescription): void {
    const firstGraph = this.#graph === null;
    const exampleChanged = !firstGraph && this.#graph?.name !== graph.name;

    if (firstGraph || exampleChanged) {
      // The page resets its whole view on any graph message; so do we, but
      // only when the example actually changed — on a reconnect the server
      // resends the same graph and the sequence frontier must survive, or the
      // replayed tail would be re-delivered.
      this.#lastSequence.clear();
      this.#history.clear();
      this.#dropped.clear();
      this.#commandDropped = 0n;
      this.#readCursor.clear();

      // A live iterator outlives an example switch. Sequence numbers are
      // per-engine, so the old example's cursor would filter the new
      // example's events away; reset each subscriber to "everything".
      if (exampleChanged) {
        for (const subscriber of this.#subscribers) subscriber.since.clear();
      }
    }

    this.#graph = graph;
    this.#debug(`graph "${graph.name}": ${graph.nodes.length} nodes, ${graph.inputs.length} inputs`);

    for (const waiter of [...this.#graphWaiters]) {
      if (waiter.predicate(graph)) this.#settleGraphWaiter(waiter, graph);
    }

    for (const listener of [...this.#graphListeners]) safeInvoke(listener, graph);
  }

  #handleExamples(examples: ExampleList): void {
    this.#examples = examples;
    this.#debug(`examples: [${examples.names.join(", ")}] active=${examples.active}`);

    for (const waiter of [...this.#examplesWaiters]) {
      if (waiter.timer !== null) clearTimeout(waiter.timer);
      this.#examplesWaiters = this.#examplesWaiters.filter((entry) => entry !== waiter);
      waiter.resolve(examples);
    }

    for (const listener of [...this.#examplesListeners]) safeInvoke(listener, examples);
  }

  #handleBatch(batch: { clock: string; dropped: bigint; events: TraceEvent[] }): void {
    // The connect-time history replay travels as batches with an empty
    // `clock` (each event still carries its own); live batches always name a
    // clock. One live batch means the replay tail is drained.
    const replay = batch.clock === "";

    if (this.#replayPending) {
      if (replay) this.#armReplayTimer();
      else this.#markReplayDone();
    }

    // Server restart on reconnect: the replayed tail can contain sequence
    // numbers at or below our frontier with *different* stamp_ns (a normal
    // reconnect replays the same events, stamps included). Treat that as a new
    // session and reset the frontier, or the new server's stream would be
    // filtered away forever. Sequences older than the retained history window
    // cannot be compared; they are assumed to be duplicates (documented in
    // README — the protocol has no session id to do better).
    if (this.#hasNewSessionEvents(batch)) {
      this.#debug("replayed events disagree with the retained trace: treating reconnect as a server restart");
      this.#lastSequence.clear();
      this.#history.clear();
      this.#dropped.clear();
      this.#readCursor.clear();

      for (const subscriber of this.#subscribers) subscriber.since.clear();
    }

    const accepted = new Map<string, TraceEvent[]>();

    for (const event of batch.events) {
      const clock = this.#batchClock(batch, event);
      const last = this.#lastSequence.get(clock);

      if (last !== undefined && event.sequence <= last) continue;

      this.#lastSequence.set(clock, event.sequence);

      let events = accepted.get(clock);

      if (events === undefined) {
        events = [];
        accepted.set(clock, events);
      }

      events.push(event);

      const history = this.#history.get(clock) ?? [];

      history.push(event);

      while (history.length > this.#options.historyLimit) history.shift();

      this.#history.set(clock, history);
    }

    const droppedClock = batch.clock === "" ? this.#single(accepted.keys()) ?? "unknown" : batch.clock;

    if (batch.dropped > 0n) {
      this.#dropped.set(droppedClock, (this.#dropped.get(droppedClock) ?? 0n) + batch.dropped);
    }

    const clocks = new Set(accepted.keys());

    if (batch.dropped > 0n) clocks.add(droppedClock);
    if (clocks.size === 0) return;

    for (const clock of clocks) {
      const view: EventBatchView = {
        clock,
        dropped: clock === droppedClock ? batch.dropped : 0n,
        events: accepted.get(clock) ?? [],
        replay,
      };

      if (view.events.length === 0 && view.dropped === 0n) continue;

      for (const listener of [...this.#batchListeners]) safeInvoke(listener, view);

      for (const subscriber of [...this.#subscribers]) this.#deliverToSubscriber(subscriber, view);
    }

    if (batch.dropped > 0n) {
      for (const listener of [...this.#statsListeners]) safeInvoke(listener, this.dropped);
    }
  }

  #batchClock(batch: { clock: string; events: TraceEvent[] }, event: TraceEvent): string {
    if (event.clock !== "") return event.clock;
    if (batch.clock !== "") return batch.clock;

    return "unknown";
  }

  #single<T>(iterator: IterableIterator<T>): T | undefined {
    for (const value of iterator) return value;

    return undefined;
  }

  // True when this batch carries an event the server claims is old (at or
  // below our frontier) but whose stamp_ns differs from the retained copy:
  // the producer restarted and its sequence numbers restarted with it.
  #hasNewSessionEvents(batch: { clock: string; events: TraceEvent[] }): boolean {
    for (const event of batch.events) {
      const clock = this.#batchClock(batch, event);
      const last = this.#lastSequence.get(clock);

      if (last === undefined || event.sequence > last) continue;
      if (!this.#isRetainedDuplicate(clock, event)) return true;
    }

    return false;
  }

  // A sequence at or below the frontier is normally a reconnect replay of an
  // event we already accepted; the retained history is ordered by sequence, so
  // a binary search can confirm the stamp. A sequence we no longer retain is
  // assumed to be a duplicate (it is older than the replay window).
  #isRetainedDuplicate(clock: string, event: TraceEvent): boolean {
    const events = this.#history.get(clock);

    if (events === undefined || events.length === 0) return true;

    let low = 0;
    let high = events.length - 1;

    while (low <= high) {
      const middle = (low + high) >> 1;
      const candidate = events[middle]!;

      if (candidate.sequence === event.sequence) return candidate.stampNs === event.stampNs;
      if (candidate.sequence < event.sequence) low = middle + 1;
      else high = middle - 1;
    }

    return true;
  }

  #deliverToSubscriber(subscriber: Subscriber, view: EventBatchView): void {
    const since = subscriber.since.get(view.clock);
    const events = since === undefined ? view.events : view.events.filter((event) => event.sequence > since);

    if (events.length === 0 && view.dropped === 0n) return;

    if (events.length > 0) subscriber.since.set(view.clock, events[events.length - 1]!.sequence);

    pushToSubscriber(subscriber, { ...view, events });
  }

  #historySince(since: Map<string, bigint>): EventBatchView[] {
    const batches: EventBatchView[] = [];
    const chunkSize = 512;

    for (const [clock, events] of this.#history) {
      const from = since.get(clock);
      const fresh = from === undefined ? events.slice() : events.filter((event) => event.sequence > from);

      for (let offset = 0; offset < fresh.length; offset += chunkSize) {
        batches.push({
          clock,
          dropped: 0n,
          events: fresh.slice(offset, offset + chunkSize),
          replay: true,
        });
      }
    }

    return batches;
  }

  #noteDelivered(subscriber: Subscriber, batch: EventBatchView): void {
    for (const event of batch.events) this.#advanceCursor(event.clock, event.sequence);

    if (batch.events.length > 0) {
      const last = batch.events[batch.events.length - 1]!.sequence;
      const since = subscriber.since.get(batch.clock);

      if (since === undefined || last > since) subscriber.since.set(batch.clock, last);
    }
  }

  #advanceCursor(clock: string, sequence: bigint): void {
    const current = this.#readCursor.get(clock);

    if (current === undefined || sequence > current) this.#readCursor.set(clock, sequence);
  }

  // --- replay-phase bookkeeping ------------------------------------------

  #beginReplayPhase(): void {
    this.#replayPending = true;
    this.#replayDeferred = deferred<void>();
    this.#armReplayTimer();
  }

  #armReplayTimer(): void {
    if (this.#replayTimer !== null) clearTimeout(this.#replayTimer);

    this.#replayTimer = setTimeout(() => {
      this.#replayTimer = null;
      this.#markReplayDone();
    }, this.#options.replaySettleMs);
  }

  #markReplayDone(): void {
    if (this.#replayTimer !== null) {
      clearTimeout(this.#replayTimer);
      this.#replayTimer = null;
    }

    if (!this.#replayPending) return;

    this.#replayPending = false;
    this.#replayDeferred?.resolve();
    this.#replayDeferred = null;
  }

  #waitReplayDone(): Promise<void> {
    if (!this.#replayPending || this.#replayDeferred === null) return Promise.resolve();

    return this.#replayDeferred.promise;
  }

  // --- acks / reads --------------------------------------------------------

  #handleAck(ack: Ack): void {
    const pending = this.#pending.get(ack.requestId);

    if (pending === undefined) {
      this.#orphanAcks += 1;
      this.#debug(`ack for unknown request ${ack.requestId} (error="${ack.error}")`);
      return;
    }

    if (ack.error !== "") {
      this.#settle(pending, ack.requestId, () => pending.reject(new AckError(ack.requestId, ack.error, pending.port)));
      return;
    }

    if (pending.kind === "read") {
      // A read is answered by PortValue; a bare success ack is only progress.
      this.#debug(`read ${pending.method}("${pending.port}") acked, awaiting PortValue`);
      return;
    }

    this.#settle(pending, ack.requestId, () => (pending.resolve as (value: unknown) => void)({
      requestId: ack.requestId,
      appliedEpoch: ack.appliedEpoch,
    }));
  }

  #handlePortValue(portValue: PortValue): void {
    const pending = this.#pending.get(portValue.requestId);

    if (pending === undefined) {
      this.#orphanAcks += 1;
      this.#debug(`port value for unknown request ${portValue.requestId} (port="${portValue.port}")`);
      return;
    }

    if (pending.kind !== "read") {
      this.#settle(pending, portValue.requestId, () => pending.reject(
        new DebugClientError("decode", `unexpected PortValue for write request ${portValue.requestId}`),
      ));
      return;
    }

    if (portValue.error !== "") {
      this.#settle(pending, portValue.requestId, () => pending.reject(
        new AckError(portValue.requestId, portValue.error, portValue.port === "" ? pending.port : portValue.port),
      ));
      return;
    }

    this.#settle(pending, portValue.requestId, () => (pending.resolve as (value: unknown) => void)({
      requestId: portValue.requestId,
      port: portValue.port === "" ? pending.port : portValue.port,
      value: portValue.value,
    }));
  }

  #handleCommandStats(stats: { dropped: bigint }): void {
    // CommandStats.dropped is the *cumulative* count of commands the server
    // lost since it started (per active example), broadcast only when it
    // moves — so this is an assignment, not an accumulation.
    if (stats.dropped === this.#commandDropped) return;

    this.#commandDropped = stats.dropped;
    this.#debug(`server command-drop count is now ${stats.dropped}`);

    for (const listener of [...this.#statsListeners]) safeInvoke(listener, this.dropped);
  }

  // --- requests ------------------------------------------------------------

  #request(
    kind: "write" | "read",
    method: string,
    port: string,
    encode: (requestId: bigint) => Uint8Array,
    options: RequestOptions,
  ): Promise<unknown> {
    if (this.#closedByUser) return Promise.reject(new ClientClosedError());

    if (this.#state !== "open" || this.#ws === null) {
      return Promise.reject(new NotConnectedError(`${method}("${port}") while ${this.#state}`));
    }

    const requestId = this.#nextRequestId++;
    const timeoutMs = options.timeoutMs ?? this.#options.requestTimeoutMs;

    return new Promise((resolve, reject) => {
      const pending: PendingRequest = {
        kind,
        method,
        port,
        resolve: resolve as (value: never) => void,
        reject,
        timer: null,
      };

      this.#pending.set(requestId, pending);

      if (timeoutMs > 0) {
        pending.timer = setTimeout(() => {
          this.#pending.delete(requestId);
          reject(new RequestTimeoutError(requestId, method, port, timeoutMs));
        }, timeoutMs);
      }

      try {
        this.#ws!.send(encode(requestId));
      } catch (error) {
        if (pending.timer !== null) clearTimeout(pending.timer);
        this.#pending.delete(requestId);
        reject(new NotConnectedError(`${method}("${port}") send failed: ${String(error)}`));
      }
    });
  }

  #settle(pending: PendingRequest, requestId: bigint, action: () => void): void {
    if (pending.timer !== null) clearTimeout(pending.timer);

    this.#pending.delete(requestId);
    action();
  }

  #send(bytes: Uint8Array): void {
    if (this.#closedByUser) throw new ClientClosedError();
    if (this.#state !== "open" || this.#ws === null) throw new NotConnectedError(`while ${this.#state}`);

    this.#ws.send(bytes);
  }

  #failPending(error: Error): void {
    const entries = [...this.#pending.values()];

    this.#pending.clear();

    for (const pending of entries) {
      if (pending.timer !== null) clearTimeout(pending.timer);
      pending.reject(error);
    }
  }

  // --- graph / examples waiters -------------------------------------------

  #waitForGraph(predicate: (graph: GraphDescription) => boolean, timeoutMs: number): Promise<GraphDescription> {
    if (this.#graph !== null && predicate(this.#graph)) return Promise.resolve(this.#graph);

    return new Promise<GraphDescription>((resolve, reject) => {
      const waiter: GraphWaiter = { predicate, resolve, reject, timer: null };

      if (timeoutMs > 0) {
        waiter.timer = setTimeout(() => {
          this.#graphWaiters = this.#graphWaiters.filter((entry) => entry !== waiter);
          reject(new ConnectError(`no GraphDescription within ${timeoutMs} ms`));
        }, timeoutMs);
      }

      this.#graphWaiters.push(waiter);
    });
  }

  #settleGraphWaiter(waiter: GraphWaiter, graph: GraphDescription): void {
    if (waiter.timer !== null) clearTimeout(waiter.timer);
    this.#graphWaiters = this.#graphWaiters.filter((entry) => entry !== waiter);
    waiter.resolve(graph);
  }

  #failGraphWaiters(error: Error): void {
    const waiters = this.#graphWaiters;

    this.#graphWaiters = [];

    for (const waiter of waiters) {
      if (waiter.timer !== null) clearTimeout(waiter.timer);
      waiter.reject(error);
    }
  }

  #failExamplesWaiters(error: Error): void {
    const waiters = this.#examplesWaiters;

    this.#examplesWaiters = [];

    for (const waiter of waiters) {
      if (waiter.timer !== null) clearTimeout(waiter.timer);
      waiter.reject(error);
    }
  }

  // --- misc ----------------------------------------------------------------

  #notifyConnection(info: ConnectionInfo): void {
    for (const listener of [...this.#connectionListeners]) safeInvoke(listener, info);
  }

  #debug(message: string): void {
    this.#options.debug(message);
  }
}

/** Create a client for the debug server at `url` (default: the demo's 9002). */
export function createClient(url?: string, options?: ClientOptions): DebugClient {
  return new DebugClient(url, options);
}

// --- helpers ----------------------------------------------------------------

function safeInvoke<T>(listener: (value: T) => void, value: T): void {
  try {
    listener(value);
  } catch {
    // A misbehaving listener must not break the socket's event loop.
  }
}

async function toBytes(data: unknown): Promise<Uint8Array | null> {
  if (data instanceof ArrayBuffer) return new Uint8Array(data);

  if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);

  if (typeof data === "string") return null; // the debug protocol is binary-only

  if (data !== null && typeof (data as { arrayBuffer?: unknown }).arrayBuffer === "function") {
    return new Uint8Array(await (data as Blob).arrayBuffer());
  }

  return null;
}
