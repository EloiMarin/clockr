// stdio transport for MCP.
//
// The MCP stdio transport is newline-delimited JSON-RPC: one JSON message per
// line, in both directions. (Very early MCP drafts and some non-MCP clients use
// LSP-style `Content-Length` framing instead; the reader accepts both, the
// writer always emits newline-delimited JSON so a spec-compliant client is
// happy.)
//
// Stdout is the protocol channel — nothing else in this package may write
// there. Diagnostics go to stderr through the `log` callback. The reader is
// byte-oriented (a `Content-Length` counts UTF-8 bytes, not JS characters), so
// a message with non-ASCII text cannot desynchronise the stream.

import {
  INTERNAL_ERROR,
  INVALID_REQUEST,
  PARSE_ERROR,
} from "./errors.ts";

import type { McpServer } from "./server.ts";

/**
 * Incremental decoder for the two stdio framings:
 *
 *   {"jsonrpc":"2.0",...}\n                                  (MCP, one per line)
 *   Content-Length: 123\r\n\r\n{"jsonrpc":"2.0",...}         (LSP-style)
 *
 * `push(chunk)` returns every complete JSON text it can extract; `flush()`
 * drains a final unterminated line at EOF. Malformed framing (a body without a
 * valid Content-Length) is reported through `onMalformed`, not thrown: one bad
 * frame must not take the session down.
 */
export class MessageParser {
  #buffer = Buffer.alloc(0);
  #expectedBody: number | null = null;
  #onMalformed: (message: string) => void;

  constructor(options: { onMalformed?: (message: string) => void } = {}) {
    this.#onMalformed = options.onMalformed ?? (() => {});
  }

  push(chunk: Buffer): string[] {
    this.#buffer = this.#buffer.length === 0 ? chunk : Buffer.concat([this.#buffer, chunk]);

    const frames: string[] = [];

    for (;;) {
      if (this.#expectedBody !== null) {
        if (this.#buffer.length < this.#expectedBody) break;

        frames.push(this.#buffer.subarray(0, this.#expectedBody).toString("utf8"));
        this.#buffer = this.#buffer.subarray(this.#expectedBody);
        this.#expectedBody = null;
        continue;
      }

      if (this.#buffer.length === 0) break;

      const head = this.#buffer.toString("latin1", 0, Math.min(this.#buffer.length, 16));

      if (/^content-(length|type)\s*:/i.test(head)) {
        const crlf = this.#buffer.indexOf("\r\n\r\n");
        const lf = this.#buffer.indexOf("\n\n");
        let separator = -1;
        let separatorLength = 0;

        if (crlf >= 0 && (lf < 0 || crlf < lf)) {
          separator = crlf;
          separatorLength = 4;
        } else if (lf >= 0) {
          separator = lf;
          separatorLength = 2;
        }

        if (separator < 0) break; // headers not complete yet

        const headers = this.#buffer.subarray(0, separator).toString("latin1");
        const match = /content-length\s*:\s*(\d+)/i.exec(headers);

        this.#buffer = this.#buffer.subarray(separator + separatorLength);

        if (match === null) {
          this.#onMalformed("framed message without a valid Content-Length header");
          continue;
        }

        this.#expectedBody = Number.parseInt(match[1] ?? "", 10);
        continue;
      }

      const newline = this.#buffer.indexOf(0x0a);

      if (newline < 0) break; // partial line

      let line = this.#buffer.subarray(0, newline).toString("utf8");

      this.#buffer = this.#buffer.subarray(newline + 1);

      if (line.endsWith("\r")) line = line.slice(0, -1);
      if (line.trim() !== "") frames.push(line);
    }

    return frames;
  }

  /** Parse whatever is left when the input ends (a final line without "\n"). */
  flush(): string[] {
    if (this.#expectedBody !== null) {
      this.#onMalformed("stream ended in the middle of a Content-Length framed message");
      this.#expectedBody = null;
      this.#buffer = Buffer.alloc(0);
      return [];
    }

    if (this.#buffer.length === 0) return [];

    const rest = this.#buffer.toString("utf8");

    this.#buffer = Buffer.alloc(0);

    return rest.trim() === "" ? [] : [rest.replace(/\r$/, "")];
  }
}

export interface StdioOptions {
  server: McpServer;
  input: NodeJS.ReadableStream;
  output: NodeJS.WritableStream;
  /** Called once when the transport is finished (stdin EOF, stdout EPIPE, or `stop()`). */
  onClose?: () => void;
  log?: (message: string) => void;
}

/**
 * Serve MCP over stdio until the input ends or `stop()` is called. Returns a
 * transport handle; the caller owns process lifecycle (`process.exit`) — this
 * function only stops reading and flushes pending messages before `onClose`.
 */
export function serveStdio(options: StdioOptions): { stop: () => void } {
  const { server, input, output } = options;
  const log = options.log ?? (() => {});
  const pending = new Set<Promise<void>>();
  let closed = false;

  const write = (message: unknown): void => {
    try {
      output.write(`${JSON.stringify(message)}\n`);
    } catch (error) {
      log(`stdout write failed: ${String(error)}`);
    }
  };

  const writeError = (id: string | number | null, code: number, message: string): void => {
    write({ jsonrpc: "2.0", id, error: { code, message } });
  };

  const handleText = async (text: string): Promise<void> => {
    let message: unknown;

    try {
      message = JSON.parse(text);
    } catch (error) {
      writeError(null, PARSE_ERROR, `Parse error: ${(error as Error).message}`);
      return;
    }

    try {
      const response = await server.handle(message);

      if (response !== null) write(response);
    } catch (error) {
      // `handle` catches its own failures; this is a last-resort guard so one
      // bad message can never leave a request unanswered.
      log(`unhandled dispatch error: ${String(error)}`);
      writeError(null, INTERNAL_ERROR, `internal error: ${String((error as Error).message ?? error)}`);
    }
  };

  const enqueue = (text: string): void => {
    const promise = handleText(text).finally(() => pending.delete(promise));

    pending.add(promise);
  };

  const parser = new MessageParser({
    onMalformed: (message) => writeError(null, INVALID_REQUEST, message),
  });

  const finish = (): void => {
    if (closed) return;

    closed = true;

    const detach = (stream: unknown): void => {
      const emitter = stream as { removeAllListeners?: (event?: string) => void };

      emitter.removeAllListeners?.();
    };

    detach(input);
    detach(output);

    // Let already-dispatched requests answer before the owner tears down.
    void Promise.allSettled([...pending]).then(() => options.onClose?.());
  };

  input.on("data", (chunk: Buffer | string) => {
    const buffer = typeof chunk === "string" ? Buffer.from(chunk) : chunk;

    for (const text of parser.push(buffer)) enqueue(text);
  });

  input.on("end", () => {
    for (const text of parser.flush()) enqueue(text);

    finish();
  });

  input.on("error", (error: Error) => {
    log(`stdin error: ${error.message}`);
    finish();
  });

  output.on("error", (error: Error) => {
    log(`stdout error: ${error.message}`);
    finish();
  });

  return { stop: finish };
}
