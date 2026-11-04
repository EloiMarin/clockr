# Denotational semantics of clockr

*What the engine means — precisely, and why that is useful.*

**Audience**: C++ developers. No prior exposure to denotational
semantics is assumed; `../learn/semantics.md` is the self-contained tutorial,
and this file is the reference it prepares you for.
**Status markers**: **[today]** = implemented and tested; **[planned]** =
described in `frp-decisions.md`, not yet built. The markers matter: this file
gives the semantics of the engine *as it is* and *as designed*, and says
which is which at every step.

## 1. The semantic domain: clocked streams

**Values.** The kernel's `value` variant denotes the set

```
V = 1 ⊎ Bool ⊎ ℤ₆₄ ⊎ 𝔻 ⊎ U
```

where `1 = {⋆}` is the unit set (`std::monostate`, the payload-free marker),
`ℤ₆₄` the exact 64-bit integers, and `𝔻` the doubles — which we idealize as
ℝ when reasoning (rounding is ignored, the standard convention; flag it and
move on).

**[today] `U`, the user values.** `U` is the open set of application structs:
`kernel::value` carries any `T` the shell admits as a value type (a class that
is not a scalar or `optional`). The kernel transports them opaquely; the
*semantic* rules below treat a `U`-value exactly like the others, with two
operational caveats that are deliberate: identity is by construction, not by
structural equality (two `user_ref`s are equal iff they share a cell; an
inline POD is equal iff same type and same bytes), and a replayed trace
therefore compares user values by their *fields* (the `describe<T>` schema is
what makes that possible — an E1 concern, not a kernel one). Small
trivially-copyable structs are stored inline in the variant; larger or
non-trivial ones live in a ref-counted pool cell whose lifetime the kernel
manages explicitly at tick boundaries (an operational discipline, below; it
does not change `⟦·⟧`).

**Clocks.** A clock `c` is a strictly increasing map `ℕ → Time`: `c(t)` is
the physical instant of tick `t`. Inside the semantics only the tick *index*
matters; physical time is needed only to relate one clock to another. A clock
is "when this region's `evaluate`/callback fires, counted."

**Signals.** A signal of type `A` on clock `c` is

```
Stream_c(A) = ℕ → A        -- one value per tick; an infinite sequence
```

**Events.** An event of type `A` on clock `c` is

```
Event_c(A) = ℕ → Maybe A   -- at each tick: an occurrence (Just v) or silence
```

Payload-free triggers use `A = 1`. ("Event as per-tick `Maybe`" rather than
Fran's occurrence list, because the engine is evaluated per tick; see §6.)

**Epochs.** A tick is more than an index: it is an **epoch** — the atomic unit
of observation. The domain assigns to epoch `t` a whole *map* (every input has
its value `inputsᵢ(t)` simultaneously), and nothing *inside* an epoch is
ordered. Two clauses, both of which the rest of this file relies on:

- *Intra-epoch order is unobservable.* The order in which the host sets an
  epoch's input ports, or in which same-epoch pushes reach one cross-thread
  channel, must not change what any consumer can see. What an epoch
  contributes is a map, not a sequence (same-epoch pushes coalesce; §3.2).
- *Inter-epoch order is the only observable order*, and it is fixed: epoch
  `t` precedes epoch `t+1` on each clock, and cross-region observation
  follows the resampler windows of §3.2.

**[today]** the input clause is testable now (permuting the order in which an
epoch's ports are set must not change any output); the link clause holds
for per-epoch coalescing, single-channel order, same-source consistency, and
cross-source atomic cuts, and is tested (the tear spec pins the cuts). Both
halves are law L9.

**[today] distinct clocks.** `engine::step(clock_tag)` ticks one region at a
time; clocks are genuinely distinct, and cross-region edges denote resamplers
with the equations of §3.2. Producer epochs are published as a unit
(versioned snapshots): the producer stamps one epoch across all its channels
and bumps its region generation after every channel is staged, while the
consumer snapshots one generation per producer per tick and reads every
channel at that cut — so two channels can no longer straddle a producer tick
mid-fill. Nothing in §2 changes either way — that is the point of choosing
this domain.

## 2. Meanings of the constructs that exist today

### 2.1 Nodes and tick functions

A `tick_fn` denotes a pure, total function `f : Vᵏ → V` (k = fan-in arity).
Purity and totality are **semantic requirements**, not style: §5.3 shows what
breaks without them.

A node `n` with tick function `f` and fan-in `(n₁,…,nₖ)` denotes the stream

```
⟦n⟧(t) = f( ⟦n₁⟧(t), …, ⟦nₖ⟧(t) )
```

— `f` lifted pointwise over ticks. Note what is *absent*: no `t−1`, no
history. Nodes are memoryless.

### 2.2 Inputs and outputs

The i-th `input` node denotes the i-th external input stream:
`⟦nᵢ⟧(t) = inputsᵢ(t)`. (Convention: "i-th" = order of appearance in the
evaluation order — positional ports. **[today]** `input<T>(name)` /
`output<T>(name)` make this explicit instead of conventional: the program's
port table indexes the streams by name, and routing by name is
order-independent — that is the property the named-port tests pin.)

An `output` node denotes the identity on its fan-in stream and additionally
*marks* that stream as one of the engine's outputs.

Because `inputsᵢ(t)` is indexed by tick alone, an epoch's inputs form a map,
and the order in which the host sets them is not an argument of `⟦G⟧`:
permuting the port-setting order within an epoch must leave every output
bit-identical (L9a). The named-port tests pin routing by name; a
port-setting permutation test is the natural companion, pinning the epoch.

**[today]** Storage follows the port type, so no parameter selects it. A plain
`input<T>` denotes a level: whatever was last set persists across ticks, i.e.
the host's sparse sets define a piecewise-constant `Stream_c(T)`. An
`input<optional<T>>` denotes an event: it fires for exactly the tick that was
set and reverts to disengaged afterwards (fire-and-forget), i.e. an
`Event_c(T)` with silence as the steady state. Persistence of a Maybe is never
implicit — keeping one is an explicit `scan` node with a visible seed
(`stepper`), keeping state-as-data total: the tick never invents memory
the graph does not show.

### 2.3 A whole graph

Because the graph is acyclic, the node equations have a **unique solution**,
so a graph denotes a stream function

```
⟦G⟧ : (ℕ → V)ᵐ → (ℕ → V)ᵏ       -- m input streams to k output streams
```

obtained by solving the equations (equivalently: by structural recursion over
any topological order).

**Today's headline fact.** With `delay` landed (§3.1), the engine denotes
*stateful* signal processing: `outputs(t)` may depend on `inputs(t−1)` and
earlier — but only through delay nodes, whose state is *data in the model*
(the `program`'s state slots), never hidden closures. Every other construct
remains memoryless. State-as-data is what makes replay (§2.7) and time-travel
(E3) a matter of restoring recorded values rather than re-executing anything.

### 2.4 `step()`

The central correctness statement — an obligation on every engine:

```
for every tick t:  outputs(t) = ⟦G⟧(inputs)(t)
```

Sufficient operational conditions: (a) each node is evaluated once per tick,
(b) only after all of its fan-in (i.e. the order is a topological
linearization), (c) tick functions are pure. `topology`'s Kahn order with
min-heap tie-breaking is one such linearization — deterministic by
construction. Ticks are per-region (`engine::step(clock_tag)`); the obligation
holds per region, with cross-region edges denoting the §3.2 resamplers.

### 2.5 The shell operators

```
⟦lift f⟧(s)(t)         = f(s(t))
⟦lift f⟧((s₁,…,sₙ))(t) = f(s₁(t), …, sₙ(t))    -- a pair-typed input is N fan-in edges, in zip order
⟦a >> b⟧               = ⟦b⟧ ∘ ⟦a⟧              -- function composition
⟦input<T>(tag)⟧        = the corresponding external input stream
⟦output<T>(tag)⟧(s)    = s                       -- identity + output marking
⟦parallel(a,…,z)⟧      = (⟦a⟧, …, ⟦z⟧)           -- product: a network with k outputs
⟦zip(a₁, …, aₙ)⟧       = (⟦a₁⟧, …, ⟦aₙ⟧)         -- pairing: N ports, in zip order (L8)
⟦share n⟧              = ⟦n⟧                       -- identity (!)
⟦delay(v₀)⟧(s)(t)      = ⟦delay v₀⟧(s)(t)         -- §3.1: the seed-then-shift primitive, exposed standalone
⟦scan(init, f)⟧(t)     = s(t), s(t) = f(s(t−1), in(t)), s(−1) = init   -- §3.1
⟦stepper(v₀)⟧(t)       = s(t), s(t) = s(t−1)  if in(t) = Nothing
                                           *in(t)     otherwise,            s(−1) = v₀
```

(`share`, `parallel`, `zip`/`lift` (N-ary), `delay`, `scan` and `stepper`
are landed; their equations are the acceptance criteria for that work — each
has a law below.)

`share`'s identity is the precise, technical sense in which it is an
**operational annotation**: it changes how the graph is built (instantiate
once, fan out), not what it means. The same holds for the port tags on
`input`/`output`: they name the clock a signal lives on — scheduling intent,
not value. This is not a deficiency of the semantics; it is a *discovery made
by writing the semantics*, and it pays off immediately (§5.2). `at<>` is a
clock change (§3.2): everything downstream of it lives on the target clock.
Its denotation is no longer the identity but a resampler — and cross-clock
composition is *invalid* except through one.

### 2.6 The typed shell over the erased kernel

`to_value : A → V` embeds shell types into the kernel; `from_value : V → A`
is its strict, partial left-inverse (a mismatch throws — it is a bug, never a
conversion). The shell's soundness condition: for every edge, the producer's
embedded type equals the consumer's projected type. The erased kernel
delegates all type-correctness to the shell, which is why the shell can be
rewritten (names, syntax) without touching the semantics.

**[today] user structs.** `A` may be a user struct (or an `optional` of one).
The kernel checks the *dynamic* type on the strict read — an inline value
carries its `user_type`, a cell points at one — and throws on a genuine
mismatch, exactly as `std::get<T>` does for scalars. The shell still decides
soundness at composition time; the runtime tag exists only because the erased
kernel has no type to compare.

### 2.7 `debug_engine`

`debug_engine` has the same denotation as `release_engine`, plus an
*observation map* `ω`: per tick, the ordered list of `(n, ⟦n⟧(t))` for every
node `n` — that is exactly what the JSON-lines trace records. The existing
debug/release parity test is therefore a semantic-preservation check: two
implementations, one denotation. Time-travel debugging is browsing `ω`; with
state as data (§3.1), replay becomes restoring recorded values rather than
re-executing anything.

## 3. Meanings of the constructs beyond today

### 3.1 `delay` / `scan` (state) — [today]

```
⟦delay v₀⟧(s)(t) =  v₀       if t = 0
                    s(t−1)   otherwise
```

The unit delay is the *only* primitive that looks backward in time, and it
makes feedback meaningful: cycles become legal **iff every cycle crosses at
least one delay** (guarded recursion). The semantic justification: under that
rule the node equations still have a unique solution, provable by induction
on `t` (the tick-0 values are the seeds; each later tick is determined by the
previous one). This is exactly the implemented topology rule — delay nodes
are Kahn seeds, so feedback through them never breaks the topological order,
while delay-*free* cycles are still rejected (L7) — and the semantics tells
us that rule is not ad-hoc: it is precisely the condition for the network
equations to remain well-defined.

`scan` is derived: a stateful operator is a pure step function fed
through `delay`. `stepper` is derived one step further — `scan` with the
step fixed to `value_or`, i.e. the absorb pattern for an event crossing made
level again — so it introduces no new law: parity with the hand-expanded
`scan` is its acceptance criterion. The repeat is the point for levels and
the disqualifier for edge-triggered consumers: a carried event re-fires every
silent tick, so `stepper` must never sit on an event channel — carry the
optional to the decision point there instead.

### 3.2 Multi-clock evaluation and resamplers — [today]

Each region ticks on its own clock; a cross-region edge must carry a
resampler. The two `link` modes denote the two canonical resamplers
between a producer clock `p` and a consumer clock `c`.

Status: fill (drain every inbound channel), per-region evaluate, settle, and
single publish per tick are implemented and tested — the resampler equations,
single-channel ordering, same-source consistency, per-epoch coalescing, and
the atomic multi-channel cut all hold (the tear spec pins the cuts). Each
link hands off independently *within* one shared epoch: the producer
stamps every channel of the tick with the same new epoch and publishes the
region generation once afterwards, while the consumer reads every inbound
channel at one snapshotted generation per producer — so a producer tick can
no longer straddle the consumer's fill.

**The link's input is an epoch stream, not a send log.** Semantically, a
link receives *at most one value per producer epoch*:

```
ŝ(i) = Just v    if the last send during epoch i carried v
       Nothing   if epoch i sent nothing         -- an Event stream (§1)
```

Multiple sends within one epoch **coalesce**: only the last remains; the
earlier ones leave no observable trace. This is not an implementation
shortcut — it is forced by the epoch property (L9). Observing two
same-epoch sends as two values would require ordering them, and there is no
intra-epoch order to observe. Two consequences, both design facts rather
than gaps:

- *Graph-driven flow never coalesces anything — but clean epochs send
  nothing.* A region pushes each boundary value once per epoch (§2.4 (a):
  each node is evaluated exactly once per tick — and `tick_fn` purity, §2.1,
  keeps everything else from sending), **except** an input-driven source in
  an epoch where it was not set: dirtiness is tracked per input port, and a
  clean input port contributes `Nothing`, not a monostate dud. So `ŝ(i)` is
  `Just v` for set or computed epochs and `Nothing` for untouched ones; a
  producer idling between events fills no queue. (Computed sources — lifts,
  delays — publish every tick they evaluate; those duplicates are valid epoch
  entries, merely wasteful. Gating them would need fan-in-OR dirtiness
  propagation; that tightening is available if the traffic ever matters.)
  Coalescing proper only bites for host-driven sends (`io.seek.fire(…)` twice
  between ticks), where "last" is host program order — sequential, hence
  well-defined.
- *A producer with a genuinely higher rate has no denotation.* `V` is closed
  (§1): no value *is* "three seeks", so batching k events into one send is
  not expressible. Restructuring — at most one value per epoch, or more
  channels — is the discipline the semantics names (the synchronous-
  dataflow rate rule, arriving as an obligation rather than a surprise).

**`latest`** — sample-and-hold, with initial value `v₀`:

```
⟦latest⟧(s)(u) = ŝ( max{ i : p(i) ≤ c(u), ŝ(i) defined } )   if any such i
                  v₀                                        otherwise
```

Read: "the most recent sent value as of the consumer's tick instant" — a
`stepper` over the epoch stream's occurrences (§2.5). Idealization: a
mathematically instantaneous hand-off. Operationally the hand-off race can
displace the observed cut by at most the race window (bounded staleness) —
but the race may never reorder or tear; staleness is slack in *where* the
cut lands, while ordering (below) is exact. Operationally the consumer holds
a short ring of recent epochs (eight) so a slightly stale snapshot still
finds its cut; a snapshot lapped by a longer producer lead re-snapshots
fresh and retries the fill, so the pair stays on one cut.

**[today] one scan-level refinement.** A per-slot seqlock makes each slot
read consistent, but a *scan across slots* is not atomic: the producer can
reuse a slot between two reads, letting the scan miss a newer entry (still at
or before the snapshot) while an older one survives — and two channels from
one producer could then disagree at one cut. The link therefore reports
"stale" when any slot it read advanced a full ring past the entry it
selected, and the fill re-snapshots. Only after repeated failures does the
engine's non-blocking fallback take the newest value per channel, which *can*
disagree across channels; that relaxation is counted (`program::lapped_fills`)
so it is visible rather than silent. Total blocking is deliberately not the
answer: the audio thread must not wait on a producer.

**`queued`** — order-preserving scalar stream:

```
Let d₁ < d₂ < … be the defined producer epochs (ŝ(dⱼ) defined).
⟦queued⟧(s)(u) = ŝ(dₖ)   where k = 1 + (values observed on ticks < u),
                         provided p(dₖ) ≤ c(u)
                 Nothing otherwise
```

Read: "the next unconsumed producer epoch, in epoch order, or silence." The
*k*-th defined epoch is observed at the *k*-th consumer tick that finds
something to show — a backlog of *k* producer epochs takes *k* consumer
ticks, not one batch. If you are used to thinking of a queue as "everything
since last time", think of the consumer here as sampling that queue once per
tick instead: nothing is lost or reordered, it just arrives over successive
ticks. The race may delay a value by a tick (it lands in the next tick's
sample); never further, and never out of order. **Precondition**: the backlog
depth never exceeds the queue capacity; otherwise `send()` fails and values
are dropped — i.e. the implementation only *refines* this denotation while
the precondition holds. Establishing the precondition (or choosing an
explicit overflow policy) is a proof obligation of the application, and the
denotation is what tells you there *is* such an obligation. Sizing counts
unconsumed set-events for input-driven sources (clean epochs publish
nothing), plus one entry per epoch for each computed source — the tightening
above would reduce even that to fresh values only.

Why scalar rather than a per-tick list? Two alternatives were considered.
*True intra-tick batch* (drain everything due into an array and evaluate
downstream once per item) matches the "everything since last time" intuition
and minimizes burst latency — but it breaks "each node once per tick", leaves
delay-settle and the trace ambiguous after *k* evaluations, and makes the
worst-case tick *k × graph*. *Explicit drain* (tick stays scalar; the host
pulls the backlog via `drain_queued(src, max, …)` on its own schedule) is the
most RT-controllable and closest to audio practice — but it splits the
contract across two entry points and weakens coverage to "...or drain
explicitly". Scalar-per-tick keeps O(1) work per tick, no batch buffer, and
the existing equations-plus-ordering clauses unchanged — at the price that a
burst drains over successive ticks, and uneven backlogs show `(value,
silence)` pairs until the quiet channel catches up.

**[today] overflow is counted, and still lossy.** A full queue drops the send
and the engine releases the reference it held, so nothing leaks, and the drop
is counted in `program::dropped_sends()`, making a broken precondition
observable instead of silent. The library takes no further action: there is
no error, substitution, or retry policy yet — reading the counter is the
host's job, and choosing what to do about it belongs to C3's open
evaluation-error policy. The equation above already tells us what L6's
precondition means: a dropped value is never observed, so the denotation
holds only while the capacity precondition does.

**Epoch ordering — L2 generalised across clocks.** The two equations pick,
for each consumer epoch `u`, a *cut* of producer history: everything as of
`c(u)`. The epoch property (L9b) requires that cut to be one consistent
observation:

- *Consistency.* At consumer epoch `u`, everything the region reads — its
  input ports and *every* inbound link together — reflects a single
  cut. A region must not observe epoch 7's value on one channel while
  epoch 6's is still in flight on another (a *tear*). L2 guarantees this
  within a clock; L9 extends the guarantee across clocks.
- *Monotonicity.* Each channel's visible producer epoch never moves
  backwards, and values from epochs `i < j` are never observed in the
  inverted order — on one channel or across two.
- *Coverage.* A region reads every inbound channel every epoch. Skipping a
  channel for an epoch would let a later value overtake an unread earlier
  one on another channel, inverting observed order. (This is the constraint
  any future "wake on link" region gating must respect.)

These clauses are obligations on the integration, and they constrain its
shape: independent per-link hand-offs are *not* sufficient — a consumer
reading channel W1 then W2 can be torn by a producer epoch whose pushes
straddle the two reads. The producer epoch must be published as a unit (a
versioned snapshot per region boundary for `latest`; epoch-stamped
acceptance for `queued`), so that all of an epoch's values become visible
together. The region-tick shape — fill (read every inbound channel), then
evaluate — is right; the epoch property is what the fill step must preserve.

These equations plus the epoch-ordering clauses are the acceptance tests for
the link integration: the multi-clock engine is correct iff every
region's outputs satisfy them at every crossing — and at every *pair*
of crossings (the tear test). The equations, single-channel ordering,
same-source consistency, coalescing, and the atomic multi-channel cut are
implemented and tested.

### 3.3 Dynamics — [planned]

Bounded alternation (a multiplexer among pre-instantiated branches) denotes
ordinary if-then-else on streams and mutates nothing. Unrestricted `switch`
is deliberately excluded: its denotation needs streams-of-streams, which the
static kernel does not have — the price of a statically scheduled,
preallocated engine, paid consciously.

## 4. Laws — the payoff

Each law is an equation (or a proof obligation) with a concrete use.

- **L1 Determinism.** All topological linearizations of a graph give the same
  outputs. *Proof*: induction on the order — every node's value is a pure
  function of already-computed fan-in values. *Buys*: implementation freedom,
  reproducible traces, legitimate parallel scheduling later.
- **L2 Synchrony (no glitches).** Within one tick, every consumer sees the
  same-tick values of its producers — including across diamond reconvergence,
  where a stale value would otherwise be observable. *Buys*: the property
  Sodium maintains with priority queues is here a *theorem about `evaluate`*,
  for free. L9 is the same guarantee, extended across clocks.
- **L3 Fusion.** `lift f >> lift g = lift (g ∘ f)`. *Proof*: §5.1. *Buys*: a
  legal graph-rewriting optimization (merge adjacent lifts).
- **L4 share-identity.** `⟦share n⟧ = ⟦n⟧`. *Corollary*: no test that reads
  only outputs can detect `share`; witnesses must be operational (node count,
  evaluation counters). *Buys*: it predicted the test design (§5.2).
- **L5 Parity.** `debug_engine` and `release_engine` preserve the same `⟦·⟧`.
  Already in the test suite.
- **L6 Capacity obligation.** The `queued` resampler equation holds only
  under its capacity precondition (backlog depth, not batch size — the drain
  is scalar). *Buys*: turns an intermittent production bug into a checkable
  design condition (size the queue from unconsumed set-events for
  input-driven sources, plus one entry per epoch for each computed source —
  clean epochs publish nothing, so idle tick rate no longer counts). A
  violated precondition is now observable: the drop is counted
  (`program::dropped_sends()`), though the library still has no policy for it
  (C3's evaluation-error decision).
- **L7 Causality.** Delay-guarded cycles ⇒ unique stream solution.
  *Implementation*: `topology` treats delay nodes as Kahn seeds, so a cycle
  is accepted iff every cycle crosses at least one delay; delay-free cycles
  still throw. *Buys*: feedback (IIR filters, integrators, playheads) without
  sacrificing determinism.
- **L8 Pairing (zip).** `⟦zip(a, b) >> lift f⟧(s)(t) = f(⟦a⟧(s)(t), ⟦b⟧(s)(t))`
  for a `lift` over the pair-typed wire.
  The pair is a *phantom* type: it never crosses the kernel as a value — its
  erasure is exactly "two fan-in edges, in zip order". *Corollary*: a test
  that reads only outputs cannot distinguish `zip(a, b) >> lift f` from a
  fused two-argument lift, so witnesses for the wiring must be operational
  (fan-in order, node count) — this is how the `a − b` order test is designed.
  Generalized to N branches: `⟦zip(a₁, …, aₙ) >> lift f⟧(s)(t) =
  f(⟦a₁⟧(s)(t), …, ⟦aₙ⟧(s)(t))` — the pair nest is left-associative and the
  leaves read left to right.
  *Buys*: the pairing equation is B1's acceptance criterion.
- **L9 Epoch atomicity.** A tick is an epoch — the atomic unit of
  observation. (a) *Intra-epoch order is unobservable*: the order in which an
  epoch's input ports are set, and the order of same-epoch pushes into one
  channel (which coalesce to the last), are not arguments of anything; an
  epoch contributes a map, not a sequence. (b) *Inter-epoch order is
  preserved and consistent*: values crossing regions are observed in
  producer-epoch order — never inverted, on one channel or across two —
  and a region's reads within one epoch (inputs and links together)
  form a single cut. *Proof (a)*: the domain indexes by tick alone (§1) —
  there is no intra-epoch order in the model to observe, exactly as L1
  makes evaluation order unobservable *inside* the engine; permuting the
  host's setting order permutes nothing `⟦G⟧` can see. *Obligation (b)*:
  across clocks, L9 does not follow from the single-clock kernel — it is
  §3.2's epoch-ordering acceptance criterion, and it constrains the
  integration design (epoch-scoped publication; per-epoch channel coverage).
  *Buys*: determinism that survives threads — a multi-clock program is
  still a function of per-epoch input maps plus a fixed inter-epoch order,
  so reproducible traces and time travel (E3) carry over to the
  multi-threaded engine. Testable now for inputs (port-order permutation);
  the link half is pinned by the C2 order/tear tests.

## 5. How to reason with the semantics — worked examples

### 5.1 A proof: fusion (L3)

```
⟦lift f >> lift g⟧(s)(t)
  = ⟦lift g⟧(⟦lift f⟧ s)(t)      -- definition of >>
  = g(⟦lift f⟧(s)(t))            -- definition of lift (outer)
  = g(f(s(t)))                   -- definition of lift (inner)
  = ⟦lift (g ∘ f)⟧(s)(t)         -- definition of lift, folded
```

Four lines, each justified by one semantic equation. That is the entire
technique: equational reasoning, the algebra you already know, made available
by purity (referential transparency: equals may be substituted for equals).

### 5.2 Predicting a test design: why `share` needed a side-effecting counter

From L4, `share` is denotationally invisible. A behavioural test compares
`⟦share n⟧` against `⟦n⟧` — equal *by definition*, so no outputs-based
assertion can ever distinguish correct from broken `share`. The only possible
witnesses are operational: the emitted graph's node count (structural) or an
evaluation counter (behavioural). The semantics told us, before any code was
written, what the test had to look like.

### 5.3 Discharging engine correctness

To claim "`release_engine` is correct" is to claim
`outputs(t) = ⟦G⟧(inputs)(t)` for every tick. By L1 it suffices to exhibit
one valid order and check the loop follows it — a code-inspection argument
over a small function — plus purity of user `tick_fn`s. Note how
the semantics *names the unenforceable*: C++ cannot force a lambda to be
pure, but the model identifies exactly where the contract lives, and a
replay/determinism check in `debug_engine` can detect violations empirically.

### 5.4 Designing a new operator

Checklist:

1. Write its equation using only `⟦parts⟧` (compositionality).
2. If you need graph internals to write it → the operator leaks; redesign.
3. If its equation is the identity → it is an annotation; document its
   operational contract instead of pretending it has semantic content.
4. State preconditions explicitly (capacity, totality, clock relations).
5. Turn each new equation into a property test.

## 6. Relation to classic FRP

Fran (Elliott & Hudak 1997): `Behavior a = Time → a` with continuous time;
events as occurrence lists. Sodium keeps that domain with a push-based
operational core. This engine's domain is **discrete, clock-indexed, and
multi-clock** — the synchronous-language domain (Lustre 1987; SCADE; rhine
2018 in FRP-land). The deviation is deliberate: it is what makes real-time
bounds and thread-awareness *expressible in the model at all* (clocks are
threads; resamplers are links), and it is arguably simpler than the
continuous-time original. One more deliberate absence: signals are not
first-class runtime values — the API manipulates stream *functions* (the
arrowized discipline), which is why whole classes of space leaks are absent
by construction rather than by discipline.

## 7. References

The papers behind the equations above — the original FRP semantics, the
synchronous-language lineage, and the closest published models — are listed,
with links and a wider bibliography, in
[Further reading](further-reading.md).
