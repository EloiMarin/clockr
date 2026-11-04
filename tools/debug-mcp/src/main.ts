#!/usr/bin/env node
// clockr-debug-mcp — a stdio MCP server for live-debugging a clockr host.
//
// Run it from an MCP client (OpenCode, Claude Desktop, DSH, ...) or by hand:
//
//   node tools/debug-mcp/src/main.ts --url ws://127.0.0.1:9002/ws
//
// It connects to the host's debug WebSocket (tools/web-debugger's server), then
// speaks newline-delimited JSON-RPC 2.0/MCP on stdin/stdout. The ten tools and
// the graph://<example> resource are documented in README.md and
// doc/how-to/agent-live-debug.md.
//
// The package has no build step and no dependencies: node >= 24 runs the
// TypeScript directly via type stripping. It imports the debug client by
// relative path (`../../debug-client/src/index.ts`) because this repository ships both
// packages side by side; see README.md for the publish story.

import { resolve } from "node:path";
import { pathToFileURL } from "node:url";

import { createClient } from "../../debug-client/src/index.ts";

import { McpServer } from "./server.ts";
import { serveStdio } from "./stdio.ts";

import type { ClientFactory, DebugClientLike } from "./debug-client.ts";

const VERSION = "0.1.0";
const DEFAULT_URL = "ws://127.0.0.1:9002/ws";

interface CliOptions {
  url: string;
  clientModule: string | null;
  debug: boolean;
  help: boolean;
  version: boolean;
}

function parseArgs(argv: string[]): CliOptions {
  const options: CliOptions = {
    url: process.env.CLOCKR_DEBUG_URL ?? DEFAULT_URL,
    clientModule: null,
    debug: process.env.CLOCKR_DEBUG_MCP_DEBUG === "1",
    help: false,
    version: false,
  };

  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];

    switch (argument) {
      case "--url":
      case "-u": {
        const value = argv[++index];

        if (value === undefined) throw new Error(`${argument} needs a WebSocket URL`);
        options.url = value;
        break;
      }
      case "--client-module": {
        const value = argv[++index];

        if (value === undefined) throw new Error("--client-module needs a path");
        options.clientModule = value;
        break;
      }
      case "--debug":
        options.debug = true;
        break;
      case "--help":
      case "-h":
        options.help = true;
        break;
      case "--version":
      case "-V":
        options.version = true;
        break;
      default:
        throw new Error(`unknown option: ${argument}`);
    }
  }

  return options;
}

const HELP = `clockr-debug-mcp — stdio MCP server for live clockr debugging

Usage: clockr-debug-mcp [options]

Options:
  -u, --url <ws-url>    debug server WebSocket (default ${DEFAULT_URL};
                        env CLOCKR_DEBUG_URL)
      --debug           log protocol/connection activity to stderr
                        (env CLOCKR_DEBUG_MCP_DEBUG=1)
      --client-module <path>
                        test seam: load an alternate client factory module
                        (must export createClient(url) or a default function)
  -h, --help            show this help
  -V, --version         print the version

Point the MCP client at this process (see README.md for the OpenCode snippet).
The debugger host must be running: tools/web-debugger/debugger_demo on 127.0.0.1:9002
by default. Connection happens in the background and retries, so start order
does not matter.
`;

async function loadClientFactory(modulePath: string): Promise<ClientFactory> {
  const module = (await import(pathToFileURL(resolve(modulePath)).href)) as {
    createClient?: unknown;
    default?: unknown;
  };
  const candidate = module.createClient ?? module.default;

  if (typeof candidate !== "function") {
    throw new Error(`${modulePath} does not export a createClient(url) function`);
  }

  return candidate as ClientFactory;
}

async function main(): Promise<void> {
  let options: CliOptions;

  try {
    options = parseArgs(process.argv.slice(2));
  } catch (error) {
    process.stderr.write(`clockr-debug-mcp: ${(error as Error).message}\n`);
    process.stderr.write("try --help\n");
    process.exitCode = 2;
    return;
  }

  if (options.help) {
    process.stdout.write(HELP);
    return;
  }

  if (options.version) {
    process.stdout.write(`${VERSION}\n`);
    return;
  }

  const log = (message: string): void => {
    if (options.debug) process.stderr.write(`[clockr-debug-mcp] ${message}\n`);
  };

  let client: DebugClientLike;
  let shuttingDown = false;

  try {
    if (options.clientModule === null) {
      client = createClient(options.url);
    } else {
      const factory = await loadClientFactory(options.clientModule);

      client = factory(options.url);
      log(`using client module ${options.clientModule}`);
    }
  } catch (error) {
    process.stderr.write(`clockr-debug-mcp: cannot create client: ${(error as Error).message}\n`);
    process.exitCode = 1;
    return;
  }

  let transport: { stop: () => void } | null = null;

  const shutdown = (code: number): void => {
    if (shuttingDown) return;

    shuttingDown = true;

    try {
      client.close();
    } catch (error) {
      log(`client close failed: ${String(error)}`);
    }

    transport?.stop();
    process.exitCode = code;

    // Flush the response bytes that may still be buffered on the protocol
    // channel, then leave. The timeout is a fallback if stdout never drains
    // (a closed pipe, for instance).
    const leave = (): void => process.exit(code);

    try {
      process.stdout.write("", leave);
    } catch {
      leave();
    }

    setTimeout(leave, 500).unref();
  };

  const server = new McpServer({
    client,
    version: VERSION,
    log: options.debug ? (message) => log(message) : undefined,
    onExit: () => shutdown(0),
  });

  transport = serveStdio({
    server,
    input: process.stdin,
    output: process.stdout,
    onClose: () => shutdown(0),
    log: options.debug ? (message) => log(message) : undefined,
  });

  // Connect eagerly, but do not gate the protocol on it: an MCP client may
  // start this process before the debugger host exists. The client reconnects
  // in the background, and every tool call that needs the debugger awaits
  // `connect()` itself, so the first call simply waits for the host.
  void client
    .connect()
    .then(() => log(`connected to ${options.url}`))
    .catch((error: unknown) => log(`initial connect failed (${(error as Error).message}); will retry in the background`));

  process.on("SIGINT", () => shutdown(0));
  process.on("SIGTERM", () => shutdown(0));

  log(`serving MCP on stdio; debugger url ${options.url}`);
}

await main();
