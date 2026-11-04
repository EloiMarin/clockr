// A fake Cordis-like context: it records the two plugin-contract operations the
// adapter uses and models their effect semantics.
//
// It is NOT a Cordis implementation. It implements the small surface verified
// in the dsh checkout (`ctx.provide(name, value)` registers a service, returns
// a disposer, and is an effect; `ctx.effect(body)` runs the body now and keeps
// its disposer for unload), which is what makes the adapter testable without
// booting the harness.

import type { PluginContextLike } from "../../src/index.ts";

interface ProvidedService {
  name: string;
  value: unknown;
}

export class FakeContext implements PluginContextLike {
  /** Every `provide` call, in order (assert "exactly one service"). */
  readonly provided: ProvidedService[] = [];
  /** Live services by name (what a consumer's `ctx.clockrDebug` would read). */
  readonly services = new Map<string, unknown>();
  /** Effect labels, in registration order. */
  readonly effectLabels: string[] = [];
  /** Disposers in registration order; disposal runs in reverse (Cordis order). */
  readonly disposers: Array<() => unknown> = [];

  provide(name: string, value: unknown): () => void {
    if (this.services.has(name)) throw new Error(`service "${name}" already provided`);

    this.provided.push({ name, value });
    this.services.set(name, value);

    const dispose = (): void => {
      this.services.delete(name);

      const index = this.disposers.indexOf(dispose);

      if (index >= 0) this.disposers.splice(index, 1);
    };

    this.disposers.push(dispose);

    return dispose;
  }

  effect(execute: () => () => void, label = "anonymous"): () => unknown {
    this.effectLabels.push(label);

    const disposer = execute();

    this.disposers.push(disposer);

    return disposer;
  }

  get(name: string): unknown {
    return this.services.get(name);
  }

  /** Unload in Cordis order: reverse registration, awaiting async disposers. */
  async disposeAll(): Promise<void> {
    const pending = [...this.disposers].reverse();

    this.disposers.length = 0;

    for (const dispose of pending) await dispose();
  }
}
