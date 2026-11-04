// A tiny assertion harness for the plugin test — no framework.
//
// The test runs its checks at top level (ESM top-level await), then calls
// `finish()`; a failed check sets the process exit code.

import { inspect, isDeepStrictEqual } from "node:util";

let passed = 0;
let failed = 0;

const failures: string[] = [];

export function section(title: string): void {
  console.log(`\n${title}`);
}

export function check(what: string, got: unknown, want: unknown): boolean {
  if (isDeepStrictEqual(got, want)) {
    passed += 1;
    console.log(`ok ${what}`);
    return true;
  }

  failed += 1;
  failures.push(what);
  console.log(`FAIL ${what}\n  got:  ${inspect(got, { depth: 8 })}\n  want: ${inspect(want, { depth: 8 })}`);
  return false;
}

export function ok(what: string, condition: boolean, detail?: unknown): boolean {
  if (condition) {
    passed += 1;
    console.log(`ok ${what}`);
    return true;
  }

  failed += 1;
  failures.push(what);
  console.log(`FAIL ${what}${detail === undefined ? "" : `\n  detail: ${inspect(detail, { depth: 8 })}`}`);
  return false;
}

export function sleep(ms: number): Promise<void> {
  return new Promise((resolveSleep) => setTimeout(resolveSleep, ms));
}

/** Await a promise that must reject; returns the error (or null on failure). */
export async function rejects(what: string, promise: Promise<unknown>): Promise<Error | null> {
  try {
    const value = await promise;

    failed += 1;
    failures.push(what);
    console.log(`FAIL ${what}: resolved where a rejection was expected\n  value: ${inspect(value)}`);
    return null;
  } catch (error) {
    passed += 1;
    console.log(`ok ${what}`);
    return error as Error;
  }
}

export function finish(): void {
  console.log(
    `\n${failed === 0 ? "PASS" : "FAILED"}: ${passed} ok, ${failed} failed` +
      (failures.length === 0 ? "" : ` — ${failures.join("; ")}`),
  );

  process.exitCode = failed === 0 ? 0 : 1;
}
