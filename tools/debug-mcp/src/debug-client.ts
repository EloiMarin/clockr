// The seam between the MCP layer and the debug client.
//
// Everything in `server.ts`, `tools.ts` and `resources.ts` programs against
// `DebugClientLike` — the slice of `DebugClient` this package actually uses —
// so the protocol-level tests can inject a mock client (see
// `test/support/mock-client.ts`, loaded through main.ts's `--client-module`
// test seam) without a WebSocket or a C++ host.
//
// The real implementation is imported *only* in `main.ts`; this file imports
// nothing at runtime (all imports are `import type`, erased by node's type
// stripping), which keeps the MCP protocol layer free of transport coupling.

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
  ValueInput,
} from "../../debug-client/src/index.ts";

/**
 * The client operations this server drives: observation (`graph`, `events`),
 * tick-boundary write-back (`setInput`, `getInput`, `getOutput`), example
 * switching, and the capture gate. Structurally satisfied by the debug client's
 * `DebugClient`; a mock only needs to return the shapes this package reads.
 */
export interface DebugClientLike {
  readonly state: ConnectionState;
  readonly graph: GraphDescription | null;
  readonly currentExample: string;
  readonly cursors: Record<string, bigint>;
  readonly dropped: DropStats;

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

export type { ConnectionState };
