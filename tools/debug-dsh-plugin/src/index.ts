// clockr-debug-dsh-plugin — the DeepSeek Harness (dsh) / Cordis adapter for
// live clockr debugging.
//
// ⚠ PREVIEW: dsh is developer-preview. The Cordis plugin contract (the
// `name`/`Config`/`apply` module shape, `ctx.provide`, `ctx.effect`, Standard
// Schema config validation) is verified against the read-only dsh checkout in
// this environment, but may change. This package is a *scaffold*: it ships as
// source, is not published to npm, and is not part of dsh's built-in bundle.
// See README.md; the risk is documented there on purpose.
//
// The design is one sentence: **keep all protocol coupling in the client,
// adapt only lifecycle and service shape here.** This module creates the debug client's
// `DebugClient` (via a relative import, like `tools/debug-mcp`), wraps it in
// one service, registers that service on the Cordis context, and closes the
// client when the plugin unloads. It never speaks the wire protocol, never
// decodes protobuf, and never re-implements reconnect or request handling.
//
// Cordis contract this file relies on (verified in the dsh checkout's vendored
// implementation, `vendor/cordis/src/{fiber,reflect}.ts`):
//
//   - a plugin module may export `name` (diagnostic label), `Config` (a
//     Standard Schema validator, not a plain object — Cordis calls
//     `Config['~standard'].validate(raw)` before `apply`), and
//     `apply(ctx, config)`;
//   - `ctx.provide(name, value)` registers a service owned by the plugin's
//     fiber and returns a disposer; it is an effect, so unloading unregisters
//     it automatically;
//   - `ctx.effect(body)` runs `body` now and calls the disposer it returns on
//     unload (timers, sockets — anything Cordis does not already manage).
//
// Nothing else of the Cordis API is used, and no `@deepseek-ai/*` package is
// imported: the dependency-free plugin types the minimal context surface
// structurally (`PluginContextLike`), and the real Cordis `Context` satisfies
// it at runtime.

import { createClient } from "../../debug-client/src/index.ts";

import type {
  ConnectionState,
  DropStats,
  EventBatchView,
  EventsOptions,
  ExampleList,
  GraphDescription,
  PortValue,
  RequestOptions,
  SetInputResult,
  TraceEvent,
  ValueInput,
} from "../../debug-client/src/index.ts";

// --- public constants -------------------------------------------------------

/** The Cordis service name: consumers reach the wrapper as `ctx.clockrDebug`. */
export const SERVICE_NAME = "clockrDebug";

/** The plugin's diagnostic name (the `name` export Cordis labels the plugin with). */
export const PLUGIN_NAME = "clockr-debug";

/** Same default as the debug client and the debugger host. */
export const DEFAULT_URL = "ws://127.0.0.1:9002/ws";

// --- the client seam --------------------------------------------------------
//
// The wrapper programs against this structural interface rather than the
// concrete class, so tests can inject a fake client through `clientFactory`.
// It is exactly the part of the debug client's public API this plugin forwards to.

/** The slice of the debug client's `DebugClient` this plugin uses. */
export interface DebugClientLike {
  readonly url: string;
  readonly state: ConnectionState;
  readonly graph: GraphDescription | null;
  readonly currentExample: string;
  readonly cursors: Record<string, bigint>;
  readonly dropped: DropStats;
  /** Optional diagnostics: present on the real client, defaulted in fake ones. */
  readonly decodeErrors?: number;
  readonly orphanAcks?: number;
  readonly pendingRequestCount?: number;

  connect(): Promise<GraphDescription>;
  close(): void;
  events(options?: EventsOptions): AsyncGenerator<EventBatchView, void, void>;
  setInput(port: string, value: ValueInput, options?: RequestOptions): Promise<SetInputResult>;
  getInput(port: string, options?: RequestOptions): Promise<PortValue>;
  getOutput(port: string, options?: RequestOptions): Promise<PortValue>;
  examples(): Promise<ExampleList>;
  selectExample(name: string): Promise<void>;
  pause(): Promise<void>;
  resume(): Promise<void>;
}

/** Construct a client for `url`; the real one is the debug client's `createClient`. */
export type ClientFactory = (url: string) => DebugClientLike;

const realClientFactory: ClientFactory = (url) => createClient(url);

// --- options / status -------------------------------------------------------

/** Options the wrapper accepts in code (the Cordis `config` maps onto these). */
export interface ClockrDebugServiceOptions {
  /** Debug server WebSocket URL; defaults to {@link DEFAULT_URL}. */
  url?: string;
  /** Connect eagerly when the service is created (default true). */
  connect?: boolean;
  /** Test seam: construct the underlying client (default: the debug client). */
  clientFactory?: ClientFactory;
}

/** A failure kept as structured state instead of thrown into a timer callback. */
export interface DebugErrorInfo {
  message: string;
  /** The client's structured error code when it had one (`connect`, `timeout`, ...). */
  code?: string;
}

/** A JSON-friendly snapshot of the wrapper's state — for logs, tooling, the UI. */
export interface DebugStatus {
  url: string;
  closed: boolean;
  state: ConnectionState;
  example: string;
  graph: {
    nodes: number;
    crossings: number;
    inputs: number;
    outputs: number;
    types: number;
  } | null;
  /** Per-clock `last event sequence` as decimal strings (bigint-safe). */
  cursors: Record<string, string>;
  dropped: {
    events: Record<string, string>;
    commands: string;
  };
  diagnostics: {
    decodeErrors: number;
    orphanAcks: number;
    pendingRequests: number;
  };
  /** The last connect failure, if a background connect has failed. */
  lastError: DebugErrorInfo | null;
}

function describeError(error: unknown): DebugErrorInfo {
  const typed = error as { message?: string; code?: string };
  const info: DebugErrorInfo = { message: typed?.message ?? String(error) };

  if (typeof typed?.code === "string") info.code = typed.code;

  return info;
}

// --- the service ------------------------------------------------------------

/**
 * One debug-client session, exposed as a Cordis service.
 *
 * It is deliberately a thin, lifecycle-aware adapter: every observation and
 * injection method forwards to the debug client. The only added behaviour is
 * (a) a deduplicated `connect()`, (b) a structured `status()` snapshot, and
 * (c) `close()` being idempotent so a plugin unload is always safe.
 */
export class ClockrDebugService {
  /** The debugger WebSocket URL this service is bound to. */
  readonly url: string;

  #client: DebugClientLike;
  #closed = false;
  #lastError: DebugErrorInfo | null = null;
  #connecting: Promise<GraphDescription> | null = null;

  constructor(options: ClockrDebugServiceOptions = {}) {
    this.url = options.url ?? DEFAULT_URL;
    this.#client = (options.clientFactory ?? realClientFactory)(this.url);

    // Auto-connect is a lifecycle concern of this wrapper, not of the client:
    // a failed attempt is recorded in `lastError`/`status()` and the client
    // keeps retrying, so there is nothing to throw into the constructor.
    if (options.connect === true) {
      void this.connect().catch(() => {});
    }
  }

  /** The underlying client — the escape hatch for anything the wrapper omits. */
  get client(): DebugClientLike {
    return this.#client;
  }

  /** Connection state, forwarded from the client (`idle`/`connecting`/`open`/...). */
  get state(): ConnectionState {
    return this.#client.state;
  }

  /** The live example's name (graph name, else the example list's active). */
  get currentExample(): string {
    return this.#client.currentExample;
  }

  /** True once the WebSocket is open (a graph has not necessarily arrived yet — see `connect`). */
  get connected(): boolean {
    return !this.#closed && this.#client.state === "open";
  }

  /** True after `close()` (by the owner or by plugin unload). */
  get closed(): boolean {
    return this.#closed;
  }

  /** The last background connect failure, cleared by a successful connect. */
  get lastError(): DebugErrorInfo | null {
    return this.#lastError;
  }

  /** The most recent graph description, or null before the first connect. */
  graph(): GraphDescription | null {
    return this.#client.graph;
  }

  /** A JSON-friendly snapshot of everything above, useful in logs and tools. */
  status(): DebugStatus {
    const graph = this.#client.graph;
    const cursors: Record<string, string> = {};

    for (const [clock, sequence] of Object.entries(this.#client.cursors)) cursors[clock] = sequence.toString();

    const droppedEvents: Record<string, string> = {};

    for (const [clock, count] of Object.entries(this.#client.dropped.events)) droppedEvents[clock] = count.toString();

    return {
      url: this.url,
      closed: this.#closed,
      state: this.#client.state,
      example: this.#client.currentExample,
      graph:
        graph === null
          ? null
          : {
              nodes: graph.nodes.length,
              crossings: graph.crossings.length,
              inputs: graph.inputs.length,
              outputs: graph.outputs.length,
              types: graph.types.length,
            },
      cursors,
      dropped: { events: droppedEvents, commands: this.#client.dropped.commands.toString() },
      diagnostics: {
        decodeErrors: this.#client.decodeErrors ?? 0,
        orphanAcks: this.#client.orphanAcks ?? 0,
        pendingRequests: this.#client.pendingRequestCount ?? 0,
      },
      lastError: this.#lastError,
    };
  }

  /**
   * Connect (idempotent while in flight) and resolve with the graph.
   *
   * Concurrent callers share one attempt; a failure is recorded in
   * `lastError` and rethrown to the caller. The client keeps reconnecting in
   * the background after a failure, so a later call can succeed.
   */
  connect(): Promise<GraphDescription> {
    if (this.#closed) return Promise.reject(new Error("clockr-debug service is closed"));

    if (this.#connecting !== null) return this.#connecting;

    this.#connecting = this.#client
      .connect()
      .then((graph) => {
        this.#lastError = null;
        return graph;
      })
      .catch((error: unknown) => {
        this.#lastError = describeError(error);
        throw error;
      })
      .finally(() => {
        this.#connecting = null;
      });

    return this.#connecting;
  }

  /** Close the client and stop its reconnect loop. Idempotent and safe before any connect. */
  close(): void {
    if (this.#closed) return;

    this.#closed = true;
    this.#client.close();
  }

  // --- observation ---------------------------------------------------------

  /** The trace stream, forwarded from `client.events()` (history replay + live). */
  events(options: EventsOptions = {}): AsyncGenerator<EventBatchView, void, void> {
    return this.#client.events(options);
  }

  /**
   * Convenience tail: collect the retained trace (optionally after `since`),
   * return the last `limit` events. For the full live/pull API use `events()`.
   */
  async tailEvents(limit = 50, since?: Record<string, bigint>): Promise<TraceEvent[]> {
    if (!Number.isInteger(limit) || limit < 1 || limit > 10_000) {
      throw new RangeError("tailEvents: limit must be an integer in [1, 10000]");
    }

    const events: TraceEvent[] = [];

    for await (const batch of this.#client.events({ sinceSequence: since ?? {}, live: false })) {
      events.push(...batch.events);
    }

    return events.slice(-limit);
  }

  // --- examples ------------------------------------------------------------

  examples(): Promise<ExampleList> {
    return this.#client.examples();
  }

  selectExample(name: string): Promise<void> {
    return this.#client.selectExample(name);
  }

  // --- command / read-back path -------------------------------------------

  setInput(port: string, value: ValueInput, options?: RequestOptions): Promise<SetInputResult> {
    return this.#client.setInput(port, value, options);
  }

  getInput(port: string, options?: RequestOptions): Promise<PortValue> {
    return this.#client.getInput(port, options);
  }

  getOutput(port: string, options?: RequestOptions): Promise<PortValue> {
    return this.#client.getOutput(port, options);
  }

  pause(): Promise<void> {
    return this.#client.pause();
  }

  resume(): Promise<void> {
    return this.#client.resume();
  }
}

// --- the Cordis config schema ----------------------------------------------

/** The minimal Cordis context surface this plugin uses (structurally compatible). */
export interface PluginContextLike {
  provide(name: string, value: unknown): () => void;
  effect(execute: () => () => void, label?: string): unknown;
}

/** Plugin config as declared to Cordis (`cordis.yml` `config:` block). */
export interface Config {
  /** Debug server WebSocket URL. Default `ws://127.0.0.1:9002/ws`. */
  url: string;
  /** Connect eagerly on plugin load (default true); a failed background connect is retried by the client. */
  connect: boolean;
}

/** The validated defaults, also used when `apply` is called without config in code/tests. */
export function defaultConfig(): Config {
  return { url: DEFAULT_URL, connect: true };
}

interface StandardSchemaIssue {
  message: string;
  path?: ReadonlyArray<string | number>;
}

/**
 * The Standard Schema v1 validator shape Cordis consumes
 * (`vendor/cordis/src/fiber.ts` — `resolveConfig` calls
 * `Config['~standard'].validate(raw)` and rejects `issues`).
 *
 * Hand-rolled instead of imported: this package has zero runtime dependencies
 * and a plain object is *not* accepted by Cordis — it must carry `~standard`.
 * The `types` field Standard Schema allows is optional and omitted; TypeScript
 * callers get `Config` from the exported interface, not from the schema.
 */
export interface StandardSchemaV1<T> {
  readonly "~standard": {
    readonly version: 1;
    readonly vendor: string;
    readonly validate: (value: unknown) => { value: T } | { issues: readonly StandardSchemaIssue[] };
  };
}

export const Config: StandardSchemaV1<Config> = {
  "~standard": {
    version: 1,
    vendor: "clockr",
    validate(value) {
      const issues: StandardSchemaIssue[] = [];
      const input = value === undefined || value === null ? {} : value;

      if (typeof input !== "object" || Array.isArray(input)) {
        return { issues: [{ message: "expected an object with optional `url` and `connect`" }] };
      }

      const raw = input as Record<string, unknown>;
      const config = defaultConfig();

      if (raw.url !== undefined) {
        if (typeof raw.url === "string" && raw.url.trim() !== "") {
          config.url = raw.url;
        } else {
          issues.push({ message: `expected a non-empty string, got ${typeof raw.url}`, path: ["url"] });
        }
      }

      if (raw.connect !== undefined) {
        if (typeof raw.connect === "boolean") {
          config.connect = raw.connect;
        } else {
          issues.push({ message: `expected a boolean, got ${typeof raw.connect}`, path: ["connect"] });
        }
      }

      if (issues.length > 0) return { issues };

      // Unknown keys are ignored: the harness loader may add metadata, and this
      // scaffold does not want to reject a larger composition over one typo'd
      // extra key it does not read. Documented in README.md.

      return { value: config };
    },
  },
};

// --- plugin entry -----------------------------------------------------------

/** Extra hooks for tests; never part of the Cordis config, ignored when absent. */
export interface ApplyHooks {
  /** Test seam: construct the underlying client. */
  clientFactory?: ClientFactory;
  /** Optional diagnostic sink (the harness has a logger; this scaffold does not inject it). */
  log?: (message: string) => void;
}

/**
 * The Cordis plugin entry point.
 *
 * Registers exactly one service (`ctx.clockrDebug`, see {@link SERVICE_NAME})
 * and one lifecycle effect: auto-connect if configured, and close the client on
 * unload. Cordis calls this as `apply(ctx, config)` after validating `config`
 * with {@link Config}; the optional third argument is a test seam only.
 */
export function apply(ctx: PluginContextLike, config: Config = defaultConfig(), hooks: ApplyHooks = {}): void {
  const log = hooks.log ?? (() => {});
  const service = new ClockrDebugService({
    url: config.url,
    connect: config.connect,
    clientFactory: hooks.clientFactory,
  });

  // One service. `ctx.provide` is an effect: unloading unregisters it.
  ctx.provide(SERVICE_NAME, service);

  // One lifecycle effect: the socket is a resource Cordis does not manage
  // (chapter 2 of the Cordis tutorial), so close it explicitly on unload. The
  // auto-connect itself lives in the service constructor; a failure is kept as
  // structured state in `service.status().lastError`, never an unhandled
  // rejection.
  ctx.effect(() => {
    return () => service.close();
  }, "clockr-debug: client lifecycle");

  log(`clockr-debug: service "${SERVICE_NAME}" ready (url ${config.url}, connect ${String(config.connect)})`);
}
