// The MCP protocol layer: JSON-RPC 2.0 dispatch for the subset of the Model
// Context Protocol this server implements.
//
// Deliberately hand-rolled and dependency-free. The subset is everything an
// agent needs to debug a live host, no more:
//
//   initialize / notifications/initialized   handshake + capability exchange
//   ping                                     liveness
//   tools/list, tools/call                   the ten debug tools (tools.ts)
//   resources/list, resources/read           graph://<example> (resources.ts)
//   shutdown / exit                          graceful stdio teardown
//
// Everything else answers JSON-RPC `-32601 Method not found`; a request for an
// unknown tool answers `-32602` with "Unknown tool". Notifications are never
// answered (that is what makes them notifications), including unknown ones.
//
// The class knows nothing about stdio or WebSockets: `handle(message)` takes a
// decoded JSON-RPC message and returns the response object (or null for a
// notification), which `stdio.ts` frames and writes. That is also the seam the
// protocol tests would exercise in-process if they did not spawn the server.

import {
  INTERNAL_ERROR,
  INVALID_PARAMS,
  INVALID_REQUEST,
  JsonRpcError,
  METHOD_NOT_FOUND,
} from "./errors.ts";
import { listResources, readResource } from "./resources.ts";
import { callTool, toolsList } from "./tools.ts";

import type { DebugClientLike } from "./debug-client.ts";

const SERVER_NAME = "clockr-debug-mcp";
const SERVER_VERSION = "0.1.0";

/**
 * Protocol revisions this server speaks. The handshake echoes a requested
 * revision when it is one of these, else negotiates down to the latest known
 * (MCP's rule: the server answers with a version it supports and the client
 * decides whether to proceed). Everything this server implements has been
 * stable across these revisions; capability advertising is what actually
 * constrains the session.
 */
export const SUPPORTED_PROTOCOL_VERSIONS = ["2024-11-05", "2025-03-26", "2025-06-18"] as const;
export const LATEST_PROTOCOL_VERSION = "2025-06-18";

/** The `instructions` string returned from initialize: the workflow in brief. */
export const INSTRUCTIONS = [
  "Live debugging over a running clockr host. The tools are one loop:",
  "1. graph_get — learn the example, its boundary inputs/outputs and node labels.",
  "2. events_tail / events_watch — read the trace (per-clock cursors in every result; pass",
  "   `cursors` back as `since` so successive reads never overlap).",
  "3. input_set — write a boundary input; it is applied by the host's tick loop immediately",
  "   before its next step, and `appliedEpoch` tells you which tick consumed it.",
  "4. events_watch / events_tail / output_get — read the traced effect of that write.",
  "5. pause / resume gate trace capture: pause for a stable window, resume before continuing.",
  "input_set rejects unknown ports and non-scalar values with a tool error; reads work for",
  "all kinds. Only the active example is addressable — use example_list/example_select to",
  "switch, then re-read graph_get (the graph resource graph://<example> moves with it).",
  "Treat the app as live: the trace is a moving target, so read deltas with cursors, not",
  "whole-trace snapshots, and correlate by epoch.",
].join("\n");

/** One MCP server bound to one debug client. */
export class McpServer {
  #client: DebugClientLike;
  #name: string;
  #version: string;
  #log: (message: string) => void;
  #onExit: (() => void) | null;
  #initialized = false;
  #shuttingDown = false;

  constructor(options: {
    client: DebugClientLike;
    name?: string;
    version?: string;
    log?: (message: string) => void;
    onExit?: () => void;
  }) {
    this.#client = options.client;
    this.#name = options.name ?? SERVER_NAME;
    this.#version = options.version ?? SERVER_VERSION;
    this.#log = options.log ?? (() => {});
    this.#onExit = options.onExit ?? null;
  }

  get client(): DebugClientLike {
    return this.#client;
  }

  get shuttingDown(): boolean {
    return this.#shuttingDown;
  }

  /**
   * Handle one decoded JSON-RPC message. Returns the response to send, or
   * `null` when the message was a notification (or a parse-level non-event
   * that the transport already answered).
   */
  async handle(message: unknown): Promise<Record<string, unknown> | null> {
    if (message === null || typeof message !== "object" || Array.isArray(message)) {
      return this.#error(null, INVALID_REQUEST, "invalid JSON-RPC message: expected an object");
    }

    const request = message as Record<string, unknown>;
    const hasId = Object.prototype.hasOwnProperty.call(request, "id") && request.id !== undefined;
    const id = hasId ? (request.id as string | number | null) : undefined;
    const method = request.method;

    if (typeof method !== "string" || method === "") {
      if (!hasId) return null;
      return this.#error(id, INVALID_REQUEST, 'invalid JSON-RPC request: missing string "method"');
    }

    const rawParams = request.params;

    if (rawParams !== undefined && (rawParams === null || typeof rawParams !== "object" || Array.isArray(rawParams))) {
      if (!hasId) return null;
      return this.#error(id, INVALID_PARAMS, "invalid JSON-RPC params: expected an object");
    }

    const params = (rawParams ?? {}) as Record<string, unknown>;

    if (!hasId) {
      await this.#handleNotification(method, params);
      return null;
    }

    try {
      const result = await this.#handleRequest(method, params);
      return { jsonrpc: "2.0", id, result };
    } catch (error) {
      if (error instanceof JsonRpcError) {
        return this.#error(id, error.code, error.message, error.data);
      }

      const message = error instanceof Error ? error.message : String(error);

      this.#log(`internal error handling ${method}: ${message}`);

      return this.#error(id, INTERNAL_ERROR, `internal error: ${message}`);
    }
  }

  // --- requests ------------------------------------------------------------

  async #handleRequest(method: string, params: Record<string, unknown>): Promise<unknown> {
    if (this.#shuttingDown) {
      throw new JsonRpcError(INVALID_REQUEST, `server is shutting down; ignoring "${method}"`);
    }

    switch (method) {
      case "initialize":
        return this.#initialize(params);
      case "ping":
        return {};
      case "tools/list":
        return { tools: toolsList() };
      case "tools/call":
        return callTool(this.#client, params);
      case "resources/list":
        return { resources: listResources(this.#client) };
      case "resources/read":
        return { contents: [await this.#readResource(params)] };
      case "shutdown":
        this.#shuttingDown = true;
        this.#log("shutdown requested");
        return {};
      default:
        throw new JsonRpcError(METHOD_NOT_FOUND, `Method not found: ${method}`);
    }
  }

  #initialize(params: Record<string, unknown>): Record<string, unknown> {
    const requested = typeof params.protocolVersion === "string" ? params.protocolVersion : "";
    const protocolVersion = (SUPPORTED_PROTOCOL_VERSIONS as readonly string[]).includes(requested)
      ? requested
      : LATEST_PROTOCOL_VERSION;

    this.#initialized = true;
    this.#log(`initialize: client=${JSON.stringify(params.clientInfo ?? {})} protocol=${protocolVersion}`);

    return {
      protocolVersion,
      capabilities: {
        tools: { listChanged: false },
        resources: { subscribe: false, listChanged: false },
      },
      serverInfo: { name: this.#name, version: this.#version },
      instructions: INSTRUCTIONS,
    };
  }

  async #readResource(params: Record<string, unknown>): Promise<Record<string, unknown>> {
    const uri = params.uri;
    const text = await readResource(this.#client, uri);

    return { uri, mimeType: "application/json", text };
  }

  // --- notifications -------------------------------------------------------

  async #handleNotification(method: string, params: Record<string, unknown>): Promise<void> {
    switch (method) {
      case "notifications/initialized":
        this.#log("client initialized");
        return;
      case "notifications/cancelled":
        // No long-running request state to cancel: every tool call is either a
        // fast client request or bounded by events_watch's timeout.
        return;
      case "exit":
        this.#log(`exit notification (initialized=${String(this.#initialized)}, shutdown=${String(this.#shuttingDown)})`);
        this.#onExit?.();
        return;
      default:
        // Unknown notifications must never be answered; log them if helpful.
        this.#log(`ignoring notification: ${method} ${JSON.stringify(params)}`);
    }
  }

  // --- JSON-RPC envelopes --------------------------------------------------

  #error(
    id: string | number | null | undefined,
    code: number,
    message: string,
    data?: unknown,
  ): Record<string, unknown> {
    const error: Record<string, unknown> = { code, message };

    if (data !== undefined) error.data = data;

    return { jsonrpc: "2.0", id: id ?? null, error };
  }
}
