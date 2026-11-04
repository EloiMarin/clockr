// clockr-debug-client — public entry point.
//
// The client is a dependency-free ESM TypeScript package: node >= 24 runs it
// directly via type stripping, so there is no build step and no npm install.
//
//   import { createClient } from "clockr-debug-client";
//   const client = createClient("ws://127.0.0.1:9002/ws");
//   const graph = await client.connect();
//
// See README.md for the full API.

export {
  createClient,
  DebugClient,
  DebugClientError,
  AckError,
  RequestTimeoutError,
  NotConnectedError,
  ConnectError,
  ClientClosedError,
} from "./client.ts";

export type {
  ClientOptions,
  ConnectionInfo,
  ConnectionState,
  ClientErrorCode,
  CloseEventLike,
  DropStats,
  EventsOptions,
  RequestOptions,
  SetInputResult,
  WebSocketFactory,
  WebSocketLike,
} from "./client.ts";

export {
  CLIENT_V1_ADDITIONS,
  WIRE_SCHEMA,
  decodeAck,
  decodeCommandStats,
  decodeEventBatch,
  decodeExampleList,
  decodeFrame,
  decodeGraphDescription,
  decodePortValue,
  decodeValue,
  encodePause,
  encodeReadInput,
  encodeReadOutput,
  encodeResume,
  encodeSelectExample,
  encodeSetInput,
  encodeValue,
} from "./schema.ts";

export type { ReadMessage, SetInputMessage } from "./schema.ts";

export type {
  Ack,
  CommandStats,
  Crossing,
  EventBatch,
  EventBatchView,
  EventKind,
  ExampleList,
  FieldSchema,
  GraphDescription,
  Node,
  Port,
  PortValue,
  ServerFrame,
  StructValue,
  TraceEvent,
  TypeSchema,
  Value,
} from "./types.ts";

export { bigintToJson, toValue, valueToJson } from "./value.ts";

export type { ValueInput } from "./value.ts";
