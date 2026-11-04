// A tiny assertion harness for plain node ESM tests — no framework.
//
// Each test file imports this, runs its checks at top level (top-level await
// is allowed in ESM), and calls `finish()`; a failed check sets the process
// exit code, so `node test/<file>.test.ts` is the whole test runner.

import { inspect, isDeepStrictEqual } from "node:util";

let passed = 0;
let failed = 0;

const failures: string[] = [];

export function section(title: string): void {
  console.log(`\n${title}`);
}

/** Deep-strict equality check (bigint/NaN/undefined aware). */
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

/** Boolean check; `detail` is printed on failure. */
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

/** Poll `predicate` until it is true or `timeoutMs` elapses. */
export async function waitFor(
  what: string,
  predicate: () => boolean | Promise<boolean>,
  timeoutMs = 5_000,
  intervalMs = 50,
): Promise<boolean> {
  const deadline = Date.now() + timeoutMs;

  for (;;) {
    try {
      if (await predicate()) return true;
    } catch {
      // A predicate that throws is simply "not yet".
    }

    if (Date.now() >= deadline) {
      console.log(`FAIL ${what}\n  detail: timed out after ${timeoutMs} ms`);
      failed += 1;
      failures.push(what);
      return false;
    }

    await sleep(intervalMs);
  }
}

/** Print the summary and set the process exit code. */
export function finish(): void {
  console.log(
    `\n${failed === 0 ? "PASS" : "FAILED"}: ${passed} ok, ${failed} failed` +
      (failures.length === 0 ? "" : ` — ${failures.join("; ")}`),
  );

  process.exitCode = failed === 0 ? 0 : 1;
}

/** The number of failed checks so far (for tests that need to abort early). */
export function failuresSoFar(): number {
  return failed;
}
