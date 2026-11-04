// JSON-RPC 2.0 and MCP error plumbing.
//
// The MCP transport is JSON-RPC 2.0: requests carry an `id`, responses pair it
// with `result` or `error`, notifications carry no `id` and are never answered.
// Keeping the error codes in one place is what lets the protocol tests pin
// "protocol-level failure" against "tool-level failure" (the latter is an
// `isError` tool result, not a JSON-RPC error).

/** Standard JSON-RPC 2.0 error codes. */
export const PARSE_ERROR = -32700;
export const INVALID_REQUEST = -32600;
export const METHOD_NOT_FOUND = -32601;
export const INVALID_PARAMS = -32602;
export const INTERNAL_ERROR = -32603;

/** MCP's code for "the requested resource does not exist". */
export const RESOURCE_NOT_FOUND = -32002;

/** A JSON-RPC-level failure: answered with `error`, never a tool result. */
export class JsonRpcError extends Error {
  readonly code: number;
  readonly data: unknown;

  constructor(code: number, message: string, data?: unknown) {
    super(message);
    this.name = "JsonRpcError";
    this.code = code;
    this.data = data;
  }
}

/**
 * A tool was called with arguments its input schema does not allow. The
 * dispatcher turns this into `-32602 Invalid params` — the agent called the
 * tool wrong, which is not the same thing as the debugger failing.
 */
export class InvalidParamsError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "InvalidParamsError";
  }
}
