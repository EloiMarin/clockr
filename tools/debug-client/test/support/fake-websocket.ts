// An in-process WebSocket stand-in for client unit tests. It is the seam that
// lets the reconnect, replay-dedupe and request/ack logic be tested without a
// live C++ host: the test drives the socket's lifecycle by hand and inspects
// the binary frames the client sends.

import { decodeClientFrame } from "./frames.ts";

import type { WebSocketLike, WebSocketFactory } from "../../src/client.ts";

type Listener = (event: never) => void;

export class FakeWebSocket implements WebSocketLike {
  binaryType = "arraybuffer";
  readonly url: string;
  readonly sent: Uint8Array[] = [];
  closed = false;

  readonly #listeners = new Map<string, Listener[]>();

  constructor(url: string) {
    this.url = url;
  }

  send(data: Uint8Array): void {
    if (this.closed) throw new Error("FakeWebSocket: send on a closed socket");

    this.sent.push(new Uint8Array(data));
  }

  close(): void {
    this.closed = true;
  }

  addEventListener(type: string, listener: Listener): void {
    const listeners = this.#listeners.get(type) ?? [];

    listeners.push(listener);
    this.#listeners.set(type, listeners);
  }

  // --- test drivers --------------------------------------------------------

  emitOpen(): void {
    this.#emit("open", {});
  }

  /** Deliver one binary server frame. */
  emitMessage(bytes: Uint8Array): void {
    // Exact-length ArrayBuffer, like undici's binaryType="arraybuffer" gives.
    const copy = bytes.slice();

    this.#emit("message", { data: copy.buffer });
  }

  emitClose(code = 1006, reason = ""): void {
    this.closed = true;
    this.#emit("close", { code, reason });
  }

  emitError(message = "fake error"): void {
    this.#emit("error", { message });
  }

  /** The last frame the client sent, decoded to field numbers. */
  lastSentFrame(): Record<number, unknown> {
    const frame = this.sent[this.sent.length - 1];

    if (frame === undefined) throw new Error("FakeWebSocket: no frame was sent");

    return decodeClientFrame(frame) as Record<number, unknown>;
  }

  #emit(type: string, event: unknown): void {
    for (const listener of this.#listeners.get(type) ?? []) listener(event as never);
  }
}

/** A factory that hands out scripted sockets, one per (re)connect attempt. */
export class FakeWebSocketFactory {
  readonly sockets: FakeWebSocket[] = [];
  readonly urls: string[] = [];

  readonly factory: WebSocketFactory = (url) => {
    const socket = new FakeWebSocket(url);

    this.sockets.push(socket);
    this.urls.push(url);

    return socket;
  };

  get last(): FakeWebSocket {
    const socket = this.sockets[this.sockets.length - 1];

    if (socket === undefined) throw new Error("FakeWebSocketFactory: no socket was created yet");

    return socket;
  }
}
