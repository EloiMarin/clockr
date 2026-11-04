# FRP sufficiency & future work — what the operator set covers, and what comes next

*The minimal synchronous core is sufficient for orchestration; this file maps
the boundary and turns the follow-ups into buildable proposals.*

**Status:** plan / design note (not implemented unless marked otherwise).
**Audience:** a C++ developer new to FRP; jargon is defined inline.
**Companions:** `denotational-semantics.md` (meanings, laws L1–L9),
`frp-decisions.md` (why clocked synchronous FRP + Shape C), `layering.md`
(shell ↔ kernel ↔ engine), and the roadmap section of the root `README.md`
(phases A–I and current state). Section numbers like "§4.6" below refer to the
old plan's Part A; the roadmap maps each to its current state.

---

## 1. The operator set and the sufficiency claim

Denotationally the core is four things; the rest is load-bearing engineering:

| Operator | Denotation | Why it stays |
|---|---|---|
| `liftN f` | `f` pointwise per tick | All combination logic |
| `a >> b` | composition (`⟦b⟧ ∘ ⟦a⟧`) | Plumbing |
| `zipN` | product; phantom tuple = N fan-in edges in order (L8) | Bundling inputs |
| `scan(init, f)` | `s(t) = f(s(t-1), in(t))` — the *only* state primitive | All memory |
| `share` | identity denotationally (L4); instantiate-once fan-out operationally | One state, many consumers (else silent duplication) |
| `at<Tag,mode>` | clock change + `latest`/`queued` resampling (§4.2) | The only cross-clock statement |
| `input`/`output` | boundary ports; tags seed/check clocks | Where purity ends |
| `parallel` | disjoint systems | Readable app composition |

**Claim.** This set suffices for every **first-order, static-topology, causal
synchronous stream function with bounded state**: any finite-state causal
(Mealy) machine over the value domain factorizes into pointwise next-state +
pointwise output + unit delay — i.e. into `lift` + `scan` wiring. The DAW
(transport, schedulers, voices, decode cursor) is four such machines, which is
why a "complex event interaction" collapsed into pure step functions over
`(prev, cur)` plus tables, with sample-accurate offsets as *data computed at
block rate*. Complexity lived in `f`, not in the graph.

---

## 2. Future work (prioritized, concrete)

Ordered by payoff-to-risk for an orchestration library. Each item names its
home (core vs demo-client), its shape, and its acceptance.

### 2.1 F1 bounded alternation + F2 rebuild-and-swap (dynamics with RT intact)

*Home: core (`shell`), demo uses it for mute/solo/track-enable.*
Bounded `select` over pre-instantiated branches (denotes if-then-else, §4.3);
all branches tick or the inactive one is gated — worst-case = average-case.
Structural edits (add track, load plugin) go through off-RT rebuild +
double-buffered topology swap at a region boundary, never mid-tick mutation.

```cpp
// F1 sketch (the acceptance test travels with the future F1 implementation):
auto out = shell::select(cond,   // recipe<In, bool>
                      then_,   // recipe<In, Out> (pre-instantiated)
                      else_);  // recipe<In, Out> (pre-instantiated)
// out: recipe<In, Out>. Graph contains BOTH branches (node-count test).
// F2 sketch: engine.swap_topology(staged_graph) — audio ticks old until the
// boundary, then new; link + delay-slot migration specified per phase.
```

*Accepts:* F1 routing + both-branches-present tests; F2 swap tear-free test.

### 2.2 `delayN` latency sugar + the documented lookahead pattern

*Home: core sugar + guide; no new primitive, causality (L7) untouched.*
True online lookahead (`out(t)` needs `in(t+1)`) is impossible in real time;
what music calls lookahead is always causal: score preview (future is a table
present at `t=0`), pipeline delay compensation, or early-intent/late-commit
across `latest` + delay. Provide:

```cpp
// delayN: bounded shift register, state-as-data, RT-safe.
// out(t) = t < N ? init : in(t-N). Denotation: N composed unit delays.
template<class T> auto delayN(std::size_t n, T init);
// Pattern doc: "lookahead = publish early on ui, commit late on audio":
//   ui publishes intent with target_beat; audio scan commits when
//   transport crosses it, with pipeline latency N compensated by delayN(N)
//   on the sibling outputs. Test: count-in and anticipatory mute land on
//   the intended sample despite a 2-block watermark lag.
```

### 2.3 `liftN` / `zipN` (B2) — landed

*Home: core shell-only; erasure unchanged (N fan-in edges, L8 generalised).*
Landed as variadic `zip` (output type nests pairs left-associatively) +
unified `lift` (a pair wire is one argument per leaf, via pair-wire
flattening); a binary lift is just `lift` over a `std::pair` wire.
*Accepts:* generalised L8 equation + fan-in-order test (the B1 `a−b` test
extended to 3+ args) — both in `shell/tests/recipe.test.cpp`.

### 2.4 Generic `ref` handle + descriptor protocol + epoch pools

*Home: core (one generic alternative, zero domain names).*
One new variant member `ref{type, pool, offset, size}` (16 B, trivially
copyable, literal) so multi-event batches cross as **one epoch value** (data
order inside, no intra-epoch order needed). Descriptors keep it observable:

```cpp
struct value_descriptor {
    std::string_view name;
    void (*format)(const void*, std::size_t, std::string&); // debug path only
    bool (*equals)(const void*, const void*, std::size_t);
};
template<class T> constexpr value_descriptor describe(); // customization point
#define CLOCKR_DESCRIBE(T, ...) /* hand specialization today */
// Two program-owned pools (mem::pool-backed, preallocated, noexcept on tick):
//   scratch.reset_epoch(); scratch.bump(n) -> slice | overflow;
//   ring (versioned) for bulk with watermark discipline.
// Overflow/underrun always surfaces on a sibling bool port (L6).
```

Hand-write descriptors now (macro or PFR aggregates); C++26 P2996 `std::meta`
later specializes the *same* `describe<T>` point — reflection as descriptor
codegen, never a runtime dependency (toolchain pins C++20 today).
*Accepts:* L5 parity with refs (content equality), "dark graph" test
(undescribed type fails the debug build), trace golden test, pool overflow
prefix test.

### 2.5 Growable bulk pools (background growth, audio never waits)

*Home: core pools; demo tunes thresholds.*
Fixed link queues stay fixed (their capacity *is* the L6 contract).
Bulk pools (scratch, decode ring) gain soft-cap + grow-ahead: audio bumps
generation `g` (bounded, flagged on overflow); a high-water `latest` mark
asks the worker to grow; the worker allocates off-RT and publishes `(g+1,
base)` as a versioned snapshot at an epoch boundary (same atomic-publish fix
as the L9b tear work); handles carry `(generation, offset, size)` with two
generations live across handover. L6 becomes "grow-ahead + hard ceiling +
flag." Ship fixed pools first; this is the named follow-up.

### 2.6 Async bridge — ticks drive the loop, the shell owns the processes (concrete proposal)

*Home: demo-client pattern first (no core change); graduate to a
`future_input` port type only once two clients need it. This section is the
proposal; rationale-against-core-consumption lives in §3.5.*

**Contract.** Three coroutine/future/sender sources, **one uniform completion
event** observed by the graph. The graph never names `std::future`,
awaitables, or senders; it sees ports:

```cpp
namespace async_client
{

using SpawnId = std::int64_t;              // shell-minted, monotonic per bridge

enum class CompletionStatus : std::uint8_t { ok, error, cancelled };

struct AsyncReq                              // audio/ui -> worker (queued)
{
    SpawnId id = 0;
    std::int64_t op = 0;                     // demo-defined opcode (load, fetch, …)
    std::int64_t arg0 = 0, arg1 = 0;         // small args; bulk via pools (§2.4)
};

struct AsyncResult                            // worker -> audio (queued)
{
    SpawnId id = 0;
    CompletionStatus status = CompletionStatus::ok;
    std::int64_t code = 0;                   // error code / byte count
    // payload bytes (if any) travel in the scratch pool behind a ref twin,
    // exactly like trigger batches — never inline here.
};

struct AsyncCancel                            // any -> worker (queued)
{
    SpawnId id = 0;
};

// Descriptor registrations (name + format/equals) so trace/lldb show
// req{load, id=7} / result{id=7, ok} instead of addresses.

} // namespace async_client
```

**Bridge (shell-owned, off-graph).** Owns the process table; the graph owns
only the *observed* values. One instance per direction (or one shared):

```cpp
class AsyncBridge {
public:
    struct Config { std::size_t max_inflight = 64; std::size_t pump_per_wake = 8; };
    explicit AsyncBridge(Config c = {});

    // Called from the shell when the graph emits requests (after audio tick):
    // non-blocking, bounded; returns false (counted) when full — surfaces on
    // the existing overflow port, same L6 discipline as §2.4.
    bool submit(async_client::AsyncReq r) noexcept;
    void cancel(async_client::AsyncCancel c) noexcept;

    // Source adapters (three thin shims, uniform result):
    //   std::future<T>:        poll (non-blocking wait_for(0)); on ready, store
    //                          completion {id, ok|error, code}.
    //   awaitable/coroutine:   launch on worker executor; on co_return/co_throw,
    //                          store completion. Never resumed on audio.
    //   sender (P2300):        connect + start_detached on worker scheduler with
    //                          set_value/set_error/set_stopped receivers that
    //                          store the same completion. stop_token <- cancel.
    template<class F> void submit_future(async_client::SpawnId, F&& fut);
    template<class Awaitable> void submit_awaitable(async_client::SpawnId, Awaitable);
    template<class Sender> void submit_sender(async_client::SpawnId, Sender&&);

    // Called ONCE per worker tick with a bounded budget. Pumps ready
    // continuations/polls (at most pump_per_wake), moves finished items to the
    // outbound queue. Never blocks: not-ready stays tabled.
    void pump() noexcept;

    // Called from the shell before the audio tick to publish completions:
    // drains at most K per tick into program.set("async_result", …) ticks
    // (one queued entry per epoch each — the scalar sub-tick discipline).
    bool poll_result(async_client::AsyncResult&) noexcept;
    std::size_t pending() const noexcept;
};
```

**Wiring (graph side — ports only):**

```cpp
// audio: requests out (queued), completions in (queued), pending as a scan.
auto req_out  = /* … lift builds AsyncReq … */
    >> shell::output<async_client::AsyncReq, clock_tag::audio>("async_req");
auto result_in =
    shell::at<clock_tag::audio, policy::queued>(
        shell::input<async_client::AsyncResult, clock_tag::worker>("async_result"));
auto pending =
    shell::share(result_in /* + req_out counts */)
    >> shell::scan<std::int64_t, std::int64_t>(0,
           [](std::int64_t p, std::int64_t delta) {
               return std::max<std::int64_t>(0, p + delta); // +1 submit, -1 completion
           });
// timeout policy is a pure tick-counting scan beside it:
//   age[id] = age+1 per audio tick; fire cancel when age > budget.
```

**Driver (dumb shell — the whole contract):**

```cpp
// audio thread, after eng.step(prog, audio):
//   drain "async_req" port(s) -> bridge.submit(req)   (bounded, counted)
// worker thread, per wake:
//   bridge.pump();                                     (bounded steps)
//   while (bridge.poll_result(r)) { prog.set("async_result", r);
//       eng.step(prog, worker); }                      (one epoch per result)
// cancel path: shell maps a timeout-scan / user action -> bridge.cancel({id})
//   AND prog.set("async_cancel", …) for the graph-visible half.
```

**Per-source notes (adapters, not core):**

- `std::future`: poll-only (`wait_for(0)`), never `get()` on audio; `get()`
  exactly once on the worker when ready; exception → `error` + code.
- Coroutine awaitables: created/suspended on the worker executor; the
  coroutine frame lives in the bridge's table (opaque to the graph, like
  sample bytes); resume budget counted in `pump()`; destroy on cancel via the
  coroutine handle (shell-side, never in `tick_fn`).
- Senders/receivers: `schedule_from` the worker scheduler; receivers write
  completions; cancellation wires the sender `stop_token` to `AsyncCancel`.
  Structured-concurrency scopes belong to the bridge lifetime, not the graph.

*Accepts:* completion-order-by-observation test (two racing sources → whatever
is polled first wins, deterministically per run-log); cancel-before-completion
test (no late completion leaks); timeout-scan test (cancel fires after N
ticks); pending-scan test; overflow test (inflight cap → counted drop, never
reorder); replay test (log of completions re-ticked → identical outputs).

### 2.7 Sparse-region gating ("wake on link") + observability tail

*Home: core optimisation + E1/E3.* Skip evaluating idle regions when no
inbound changed (must still respect coverage — read every inbound every
*evaluated* epoch — and publish atomically when waking). Extends E1
determinism checks and E3 replay to refs/pools/generations and the async
completion log.

---

## 3. Directions deliberately NOT taken (with reasons)

- **Core consuming futures/awaitables/senders as tick inputs.** Processes are
  one-shot, cancellable, nondeterministically ordered; streams are total,
  ordered, every-tick. Consuming them inside `tick_fn` breaks purity (§3.1),
  determinism (L1), and replay (coroutine frames aren't state-as-data). §2.6
  is the interface instead: the loop is tick-driven, the processes stay
  behind ports.
- **Intra-tick fixpoints on the block clock.** Needed for sample-rate DSP
  synthesis, explicitly out of scope for an orchestration engine (would break
  the static straight-line pass and RT bounds). Sample-rate DSP, if ever,
  is a separate `sample` clock region — not a fixpoint operator.
- **Unbounded `switch` on the audio clock.** Carries a dynamic runtime
  (allocation, reordering, unbounded worst case) that forfeits static
  schedule, diagrams, and time-travel. F1/F2 cover the musical needs.
- **Observable intra-epoch order.** Would require ordering same-tick pushes,
  contradicting the epoch property (L9a) that makes coalescing, replay, and
  tear-freedom possible. Batches cross as one value instead.
- **Nondeterministic cross-thread merge.** L1/L2/L9b promise cuts, not races;
  "first finisher wins" lives in the bridge's poll order (§2.6), observed
  deterministically per log — never as a graph race.
- **Unbounded core resources.** Every core queue/pool is user-sized with a
  visible flag (L6). Background growth (§2.5) is scoped to bulk pools with
  generational handover — never silent unboundedness in the kernel.
- **A true online-lookahead primitive.** Non-causal in real time; breaks L7.
  Score preview, latency compensation, and early-intent/late-commit cover
  the musical cases causally; `delayN` + pattern doc is the deliverable.
- **Domain types in the kernel** (`loop_region`, `note`, `clip`, …). A
  layering leak: each new client would demand its own. Fixed structs travel
  as scalar wiring (L8), variable bulk as generic `ref` (§2.4).
- **C++26 reflection as a runtime dependency.** Reflection generates
  descriptors; it never runs on the tick path and never gates the core
  (toolchain pins C++20). The `describe<T>` point is designed so `std::meta`
  slots in later without redesign.
