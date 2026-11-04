// Child-process plumbing shared by the protocol and e2e harnesses: free-port
// picking, debugger_demo startup/readiness, and clean teardown.
//
// Readiness is a TCP connect, not a stdout line: the demo's stdout is
// block-buffered when piped through another process, so the "listening" line
// can arrive after the first client already connected (or never flush before
// the test moves on). A bound socket is the fact that matters.
//
// The demo is SIGKILLed on teardown: it has three ticking threads and a uWS
// loop, and we do not want a slow shutdown to leak a listener into the next
// test run.

import { spawn, type ChildProcess } from "node:child_process";
import { existsSync } from "node:fs";
import { connect, createServer } from "node:net";
import { dirname, resolve } from "node:path";
import type { Readable } from "node:stream";
import { fileURLToPath } from "node:url";

import { sleep } from "./harness.ts";

const HERE = dirname(fileURLToPath(import.meta.url));

/** The repository root, from tools/debug-mcp/test/support. */
export const REPO_ROOT = resolve(HERE, "../../../..");

/** The native debug build tree binary (the usual local/CI preset). */
export const DEFAULT_DEMO = resolve(REPO_ROOT, "_build/linux_x64_debug/tools/web-debugger/debugger_demo");

export function isPortFree(port: number): Promise<boolean> {
  return new Promise((resolveFree) => {
    const server = createServer();

    server.once("error", () => resolveFree(false));
    server.listen(port, "127.0.0.1", () => server.close(() => resolveFree(true)));
  });
}

/** A free port in 19000-19999 (the team's verification range). */
export async function pickPort(): Promise<number> {
  for (let attempt = 0; attempt < 50; attempt += 1) {
    const port = 19_000 + Math.floor(Math.random() * 1_000);

    if (await isPortFree(port)) return port;
  }

  throw new Error("could not find a free port in 19000-19999");
}

function tcpReachable(port: number): Promise<boolean> {
  return new Promise((resolveReachable) => {
    const socket = connect({ host: "127.0.0.1", port });

    const settle = (reachable: boolean): void => {
      socket.removeAllListeners();
      socket.destroy();
      resolveReachable(reachable);
    };

    socket.once("connect", () => settle(true));
    socket.once("error", () => settle(false));
    socket.setTimeout(500, () => settle(false));
  });
}

export interface RunningDemo {
  port: number;
  child: ChildProcess & { stdout: Readable; stderr: Readable };
  stderr: () => string;
  stop: () => Promise<void>;
}

/** Spawn `debugger_demo` on `port` and wait until its socket accepts connections. */
export async function startDebuggerDemo(options: {
  port: number;
  demoPath?: string;
  example?: string;
  throwEvery?: number;
  readyTimeoutMs?: number;
}): Promise<RunningDemo> {
  const demoPath = options.demoPath ?? process.env.DEBUGGER_DEMO ?? DEFAULT_DEMO;
  const webRoot = resolve(REPO_ROOT, "tools/web-debugger/web");
  const args = [
    demoPath,
    "--host",
    "127.0.0.1",
    "--port",
    String(options.port),
    "--web-root",
    webRoot,
  ];

  if (options.example !== undefined) args.push("--example", options.example);
  if (options.throwEvery !== undefined) args.push("--throw-every", String(options.throwEvery));

  const child = spawn(args[0] as string, args.slice(1), {
    cwd: REPO_ROOT,
    stdio: ["ignore", "pipe", "pipe"],
  }) as ChildProcess & { stdout: Readable; stderr: Readable };

  let stderrText = "";

  child.stderr.on("data", (chunk: Buffer) => {
    stderrText += chunk.toString("utf8");
  });
  // Drain stdout even though readiness does not parse it: an unread pipe can
  // fill and stall the demo.
  child.stdout.resume();

  const stop = async (): Promise<void> => {
    if (child.exitCode === null && child.signalCode === null) {
      child.kill("SIGKILL");
      await new Promise<void>((resolveExit) => child.once("close", () => resolveExit()));
    }
  };

  const deadline = Date.now() + (options.readyTimeoutMs ?? 30_000);

  while (Date.now() < deadline) {
    if (child.exitCode !== null || child.signalCode !== null) {
      throw new Error(`debugger_demo exited before listening (${child.exitCode}); stderr:\n${stderrText}`);
    }

    if (await tcpReachable(options.port)) {
      // Give the server's uWS loop a beat to finish setup after the bind; the
      // first websocket upgrade is then accepted deterministically.
      await sleep(100);
      return { port: options.port, child, stderr: () => stderrText, stop };
    }

    await sleep(25);
  }

  await stop();
  throw new Error(`debugger_demo never listened on 127.0.0.1:${options.port}; stderr:\n${stderrText}`);
}

/** True when a `debugger_demo` binary is available to run the real e2e. */
export function hasDebuggerDemo(): boolean {
  return existsSync(process.env.DEBUGGER_DEMO ?? DEFAULT_DEMO);
}
