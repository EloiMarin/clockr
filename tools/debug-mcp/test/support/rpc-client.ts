// A minimal MCP client for the tests: spawn the server over stdio, speak
// newline-delimited JSON-RPC 2.0 to it, and read responses.
//
// No framework, no MCP SDK: this deliberately exercises the wire the same way
// a real MCP client would (spawn the process, pipe its stdin/stdout) instead of
// calling the server class in-process. That way the transport, framing,
// handshake and JSON-RPC envelopes are all under test.

import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const MCP_ROOT = resolve(HERE, "../..");
const MAIN = resolve(MCP_ROOT, "src/main.ts");

/** A JSON-RPC error response, raised by `request()` when the server answers one. */
export class RpcError extends Error {
  readonly code: number;
  readonly data: unknown;

  constructor(message: string, code: number, data?: unknown) {
    super(message);
    this.name = "RpcError";
    this.code = code;
    this.data = data;
  }
}

interface Pending {
  resolve: (value: unknown) => void;
  reject: (error: Error) => void;
  timer: ReturnType<typeof setTimeout> | null;
}

export interface SpawnOptions {
  url: string;
  /** Test seam passed through to main.ts: a module exporting createClient(url). */
  clientModule?: string;
  env?: Record<string, string>;
  cwd?: string;
}

/** One spawned `clockr-debug-mcp` process plus its JSON-RPC plumbing. */
export class McpClient {
  #child: ChildProcessWithoutNullStreams;
  #pending = new Map<number, Pending>();
  #nextId = 1;
  #messages: Array<Record<string, unknown>> = [];
  #messageWaiters: Array<(message: Record<string, unknown>) => void> = [];
  #lineBuffer = "";
  #stderr = "";
  #exit: Promise<number | null>;

  private constructor(child: ChildProcessWithoutNullStreams) {
    this.#child = child;

    child.stdout.setEncoding("utf8");
    child.stdout.on("data", (chunk: string) => this.#onStdout(chunk));
    child.stderr.setEncoding("utf8");
    child.stderr.on("data", (chunk: string) => {
      this.#stderr += chunk;
    });

    this.#exit = new Promise((resolveExit) => {
      child.once("close", (code) => resolveExit(code));
    });
  }

  static async spawn(options: SpawnOptions): Promise<McpClient> {
    const args = [MAIN, "--url", options.url];

    if (options.clientModule !== undefined) args.push("--client-module", options.clientModule);

    const child = spawn(process.execPath, args, {
      cwd: options.cwd ?? MCP_ROOT,
      env: { ...process.env, ...options.env },
      stdio: ["pipe", "pipe", "pipe"],
    });

    await new Promise<void>((resolveSpawn, rejectSpawn) => {
      child.once("spawn", () => resolveSpawn());
      child.once("error", (error) => rejectSpawn(error));
    });

    return new McpClient(child);
  }

  get stderr(): string {
    return this.#stderr;
  }

  get pid(): number | undefined {
    return this.#child.pid;
  }

  /** Send one request and resolve with its `result` (reject with `RpcError` on an `error`). */
  request(method: string, params?: unknown, timeoutMs = 10_000): Promise<unknown> {
    const id = this.#nextId;
    this.#nextId += 1;

    const message: Record<string, unknown> = { jsonrpc: "2.0", id, method };

    if (params !== undefined) message.params = params;

    return new Promise((resolveRequest, rejectRequest) => {
      const timer =
        timeoutMs > 0
          ? setTimeout(() => {
              this.#pending.delete(id);
              rejectRequest(new Error(`no response to ${method} within ${timeoutMs} ms`));
            }, timeoutMs)
          : null;

      this.#pending.set(id, {
        resolve: (value) => {
          if (timer !== null) clearTimeout(timer);
          resolveRequest(value);
        },
        reject: (error) => {
          if (timer !== null) clearTimeout(timer);
          rejectRequest(error);
        },
        timer,
      });

      this.#write(message);
    });
  }

  /** Send a notification (no response expected). */
  notify(method: string, params?: unknown): void {
    const message: Record<string, unknown> = { jsonrpc: "2.0", method };

    if (params !== undefined) message.params = params;

    this.#write(message);
  }

  /** Write raw bytes on the stdin channel (framing/parse-error tests). */
  writeRaw(data: string | Buffer): void {
    this.#child.stdin.write(data);
  }

  /** The next message the server sends that did not match a pending request. */
  nextMessage(timeoutMs = 5_000): Promise<Record<string, unknown>> {
    const queued = this.#messages.shift();

    if (queued !== undefined) return Promise.resolve(queued);

    return new Promise((resolveMessage, rejectMessage) => {
      const timer = setTimeout(() => {
        this.#messageWaiters = this.#messageWaiters.filter((waiter) => waiter !== onMessage);
        rejectMessage(new Error(`no server message within ${timeoutMs} ms`));
      }, timeoutMs);

      const onMessage = (message: Record<string, unknown>): void => {
        clearTimeout(timer);
        resolveMessage(message);
      };

      this.#messageWaiters.push(onMessage);
    });
  }

  /** Resolve with the exit code (null if killed by a signal). */
  waitExit(timeoutMs = 5_000): Promise<number | null> {
    return Promise.race([
      this.#exit,
      new Promise<number | null>((resolveTimeout) => {
        setTimeout(() => resolveTimeout(-1), timeoutMs).unref();
      }),
    ]);
  }

  get exited(): boolean {
    return this.#child.exitCode !== null || this.#child.signalCode !== null;
  }

  /** SIGKILL the server (after a shutdown/exit test normally). */
  async kill(): Promise<void> {
    if (this.exited) return;

    this.#child.kill("SIGKILL");
    await this.#exit;
  }

  // --- internals -----------------------------------------------------------

  #write(message: Record<string, unknown>): void {
    this.#child.stdin.write(`${JSON.stringify(message)}\n`);
  }

  #onStdout(chunk: string): void {
    this.#lineBuffer += chunk;

    for (;;) {
      const newline = this.#lineBuffer.indexOf("\n");

      if (newline < 0) return;

      const line = this.#lineBuffer.slice(0, newline).replace(/\r$/, "");

      this.#lineBuffer = this.#lineBuffer.slice(newline + 1);

      if (line.trim() === "") continue;

      let message: Record<string, unknown>;

      try {
        message = JSON.parse(line) as Record<string, unknown>;
      } catch {
        // A malformed server line is a test failure, not something to ignore;
        // surface it on the next request/waiter timeout with context.
        this.#stderr += `\n[mcp-test] non-JSON line from server: ${line}`;
        continue;
      }

      const id = message.id;

      if (typeof id === "number" && this.#pending.has(id)) {
        const pending = this.#pending.get(id) as Pending;

        this.#pending.delete(id);

        const error = message.error as { message?: string; code?: number; data?: unknown } | undefined;

        if (error !== undefined) {
          pending.reject(new RpcError(error.message ?? "JSON-RPC error", error.code ?? 0, error.data));
        } else {
          pending.resolve(message.result);
        }

        continue;
      }

      const waiter = this.#messageWaiters.shift();

      if (waiter !== undefined) waiter(message);
      else this.#messages.push(message);
    }
  }
}
