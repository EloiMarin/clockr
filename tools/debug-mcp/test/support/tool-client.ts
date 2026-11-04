// Shared JSON-RPC conveniences for the two harnesses: call a tool and decode
// its text content block, retry until the debugger is ready, and assert a
// JSON-RPC error code. No framework, just the three shapes both tests use.

import { ok, sleep } from "./harness.ts";
import { RpcError } from "./rpc-client.ts";

import type { McpClient } from "./rpc-client.ts";

export interface ToolCallResult {
  isError: boolean;
  payload: unknown;
  raw: unknown;
}

/** Call `tools/call` and parse the single JSON text content block. */
export async function callTool(
  client: McpClient,
  name: string,
  args: Record<string, unknown> = {},
  timeoutMs = 10_000,
): Promise<ToolCallResult> {
  const raw = (await client.request("tools/call", { name, arguments: args }, timeoutMs)) as {
    content?: Array<{ type?: string; text?: string }>;
    isError?: boolean;
  };
  const text = raw.content?.[0]?.text ?? "";
  let payload: unknown = text;

  try {
    payload = JSON.parse(text);
  } catch {
    // Leave the raw text in `payload`: a non-JSON tool result is a failure the
    // caller's shape check reports with the offending text.
  }

  return { isError: raw.isError === true, payload, raw };
}

/**
 * Call a tool until it is a non-error result satisfying `predicate` (or until
 * `timeoutMs`). Used right after spawn, where the client may still be
 * connecting; returns the last result so the caller asserts on it with detail.
 */
export async function waitForTool(
  client: McpClient,
  name: string,
  args: Record<string, unknown>,
  predicate: (payload: unknown) => boolean,
  timeoutMs = 8_000,
  intervalMs = 100,
): Promise<ToolCallResult> {
  const deadline = Date.now() + timeoutMs;
  let last: ToolCallResult | null = null;

  for (;;) {
    try {
      last = await callTool(client, name, args);

      if (!last.isError && predicate(last.payload)) return last;
    } catch {
      // A request timeout / transport hiccup just means "not ready yet".
    }

    if (Date.now() >= deadline) {
      if (last !== null) return last;

      // Nothing ever answered: rethrow the next failure with context.
      throw new Error(`waitForTool(${name}): no result within ${timeoutMs} ms`);
    }

    await sleep(intervalMs);
  }
}

/** Run `action` and assert it rejects with a JSON-RPC error of `code`. */
export async function expectRpcError(
  what: string,
  action: () => Promise<unknown>,
  code: number,
): Promise<void> {
  try {
    const value = await action();

    ok(what, false, { resolved: value });
  } catch (error) {
    if (error instanceof RpcError) {
      ok(what, error.code === code, { code: error.code, message: error.message });
    } else {
      ok(what, false, { error: String(error) });
    }
  }
}
