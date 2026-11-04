// A tiny assertion harness for plain node ESM tests — no framework.
//
// Each test file imports this, runs its checks at top level (top-level await
// is allowed in ESM), and calls `finish()`; a failed check sets the process
// exit code, so `node test/<file>.test.ts` is the whole test runner.

import { inspect, isDeepStrictEqual } from "node:util";

let passed = 0;
let failed = 0;

export function section(title: string): void {
  console.log(`\n${title}`);
}

/** Deep-strict equality check (bigint/NaN/undefined aware). */
export function check(what: string, got: unknown, want: unknown): void {
  if (isDeepStrictEqual(got, want)) {
    passed += 1;
    console.log(`ok ${what}`);
    return;
  }

  failed += 1;
  console.log(`FAIL ${what}\n  got:  ${inspect(got, { depth: 6 })}\n  want: ${inspect(want, { depth: 6 })}`);
}

/** Boolean check; `detail` is printed on failure. */
export function ok(what: string, condition: boolean, detail?: unknown): void {
  if (condition) {
    passed += 1;
    console.log(`ok ${what}`);
    return;
  }

  failed += 1;
  console.log(`FAIL ${what}${detail === undefined ? "" : `\n  detail: ${inspect(detail, { depth: 6 })}`}`);
}

/**
 * Await a promise that must reject; checks the error's `name` (and optionally
 * a predicate). Returns the error so callers can inspect it further.
 */
export async function rejects(
  what: string,
  promise: Promise<unknown>,
  expectedName: string,
  predicate: (error: Error) => boolean = () => true,
): Promise<Error | null> {
  try {
    await promise;
  } catch (error) {
    const typed = error as Error;

    if (typed.name !== expectedName) {
      failed += 1;
      console.log(`FAIL ${what}\n  rejected with ${typed.name}: ${typed.message}\n  want: ${expectedName}`);
      return null;
    }

    if (!predicate(typed)) {
      failed += 1;
      console.log(`FAIL ${what}\n  predicate rejected the ${expectedName}: ${typed.message}`);
      return null;
    }

    passed += 1;
    console.log(`ok ${what} (${typed.name})`);
    return typed;
  }

  failed += 1;
  console.log(`FAIL ${what}: promise resolved where a rejection was expected`);
  return null;
}

/** Async catch helper: returns the rejection or null when it resolved. */
export async function settle<T>(promise: Promise<T>): Promise<{ value?: T; error?: Error }> {
  try {
    return { value: await promise };
  } catch (error) {
    return { error: error as Error };
  }
}

export function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

/** Poll `predicate` until it is true (or throw after `timeoutMs`). */
export async function waitFor(
  what: string,
  predicate: () => boolean | Promise<boolean>,
  options: { timeoutMs?: number; intervalMs?: number } = {},
): Promise<void> {
  const timeoutMs = options.timeoutMs ?? 2000;
  const intervalMs = options.intervalMs ?? 5;
  const deadline = Date.now() + timeoutMs;

  while (Date.now() < deadline) {
    if (await predicate()) {
      passed += 1;
      console.log(`ok ${what}`);
      return;
    }

    await sleep(intervalMs);
  }

  failed += 1;
  console.log(`FAIL ${what}: not true within ${timeoutMs} ms`);
}

/** Print the summary and set the exit code when a check failed. */
export function finish(fileName: string): void {
  if (failed > 0) {
    console.log(`\n${fileName}: ${failed} check(s) failed, ${passed} passed`);
    process.exitCode = 1;
    return;
  }

  console.log(`\n${fileName}: all ${passed} checks passed`);
}
