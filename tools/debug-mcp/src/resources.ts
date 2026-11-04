// MCP resources — the graph, addressable by URI.
//
// The tools are verbs; the graph is also a *noun* an agent may want to read
// into context (or a client may render): `graph://<example>`. Per-example
// addressing is deferred on the host side (the server is active-example-only), so
// the server advertises and serves exactly one graph resource: the live
// example's. It moves with example_select, which is precisely the semantics a
// client gets by re-listing resources after a switch.
//
// A read is a plain JSON document (mimeType application/json) built by the
// same conversion `graph_get` uses — one shape, two doors.

import { JsonRpcError, RESOURCE_NOT_FOUND } from "./errors.ts";
import { graphToJson } from "./tools.ts";

import type { DebugClientLike } from "./debug-client.ts";

/** One entry of `resources/list`. */
export interface Resource {
  uri: string;
  name: string;
  description: string;
  mimeType: string;
}

const URI_PREFIX = "graph://";

async function ensureGraph(client: DebugClientLike) {
  if (client.graph !== null && client.state === "open") return client.graph;

  return client.connect();
}

function activeExample(client: DebugClientLike): string {
  return client.graph?.name !== undefined && client.graph.name !== "" ? client.graph.name : client.currentExample;
}

/** The resources this server exposes: the live example's graph (possibly none yet). */
export function listResources(client: DebugClientLike): Resource[] {
  const example = activeExample(client);

  if (example === "") return [];

  return [
    {
      uri: `${URI_PREFIX}${example}`,
      name: `graph: ${example}`,
      description:
        `The lowered network description of the live "${example}" example ` +
        "(nodes, crossings, boundary ports, described user types). Moves with example_select: " +
        "re-list after a switch. Same JSON as the graph_get tool.",
      mimeType: "application/json",
    },
  ];
}

/** Read one `graph://<example>` resource; throws a JSON-RPC resource error otherwise. */
export async function readResource(client: DebugClientLike, uri: unknown): Promise<string> {
  if (typeof uri !== "string" || !uri.startsWith(URI_PREFIX)) {
    throw new JsonRpcError(
      RESOURCE_NOT_FOUND,
      `resource not found: ${JSON.stringify(uri)} (this server exposes graph://<example>)`,
    );
  }

  const example = uri.slice(URI_PREFIX.length);

  if (example === "") {
    throw new JsonRpcError(
      RESOURCE_NOT_FOUND,
      "resource not found: graph:// needs an example name (try the uri from resources/list)",
    );
  }

  const graph = await ensureGraph(client);
  const active = graph.name !== "" ? graph.name : activeExample(client);

  if (example !== active) {
    throw new JsonRpcError(
      RESOURCE_NOT_FOUND,
      `resource not found: graph://${example} — only the active example is addressable ` +
        `(active: ${active === "" ? "none" : active}). Per-example addressing is deferred; ` +
        "call example_select first, then re-list resources.",
    );
  }

  return JSON.stringify(graphToJson(graph));
}
