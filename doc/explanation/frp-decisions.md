# FRP flavour & client API — decision analysis

Status: design rationale, 2026-08-04. §5's API names (`recipe`, `at<>`,
`hold`, `>>`, …) were illustrative when written; most have since been built
(`recipe`, `hold` — now `scan` —, `zip`/`lift` (N-ary), named ports, the
program object) — the remaining illustratives are the resampling pieces
`at<>` and the per-region tick API. Inside sketches, `hold` and
`sample_hold` read as today's `scan` and `stepper`.

## 0. TL;DR

1. **The scaffolding has already chosen a flavour, quietly.** A static graph,
   a deterministic topological order, cycle rejection, batch
   `evaluate(graph, inputs, outputs)`, a closed `value` type, `clock_tag`
   regions and bounded links — that is the operational model of
   **synchronous dataflow** (Lustre/SCADE lineage), not of Sodium-style push
   FRP. The open question is not "which flavour do I build" but "which
   surface semantics do I admit on top of the kernel I already have".
2. **Recommendation: clocked synchronous FRP** — arrowized in structure
   (signal networks as composable values, explicit delay-state, no
   first-class signals → no space leaks), synchronous in time (each region
   ticks as a unit; threads are clocks; links are resamplers). Rhine
   (Haskell) proves this exact composition is sound: it separates *clocking,
   scheduling and resampling* and rejects clock-incorrect programs at the
   type level. `clockr` would be that idea, in C++, with real-time
   primitives — which is precisely the talk's novel contribution.
3. **API: keep three shapes open, build Shape A first.** The decisive
   asymmetry: choosing Sodium semantics *forces* the "live reactive values"
   API; choosing clocked-synchronous semantics keeps *all* API shapes
   implementable over one kernel, so the central-vs-local decision can be
   deferred — and ultimately answered with "both, same mechanism".

## 1. Disentangling the decisions

"FRP flavour" and "API shape" are correlated but separable. Three independent
axes:

| Axis | Question | Examples of answers |
|---|---|---|
| **Denotational semantics** | What is a signal? Continuous? Discrete? Events vs. behaviors? | Classic FRP (continuous behaviors + events), discrete signals only, clocked signals |
| **Evaluation strategy** | When does computation happen, and what gets recomputed? | Push (change propagation, Sodium), pull (sampling, Fran/Reflex), push-pull, **batch per clock tick** (synchronous dataflow) |
| **Surface API** | What does user code look like? | Central graph asset, live cell/stream values, composed network values, equation DSL |

The key realization for this repo: **(2) is already decided by the scaffolding** —
`engine::evaluate` is a batch step over a static topology. Sodium's defining
machinery (priority-ranked propagation queues, transactions,
only-recompute-what-changed) does not exist here, and adding it would mean
throwing away `topology`, `regions`, and the deterministic order that makes
`debug_engine` and time-travel tractable.

## 2. What the existing code already implies

Reading the modules as they stand:

- **`kernel/topology`**: static network, Kahn's algorithm with deterministic
  tie-breaking, cycles rejected → a *statically scheduled synchronous
  system*. This is SCADE's whole trick (and why Lustre is certifiable for
  avionics): schedule once, then execution is a bounded straight-line pass.
- **`engine::evaluate(graph, inputs, outputs)`**: pull/batch evaluation — the
  caller ticks the engine. That is the audio-callback model, not the
  event-push model.
- **`value`** (closed variant at the time of writing:
  `monostate`/`bool`/`int64`/`double` — since extended with user structs; see
  §9): type-erased, trivially copyable, sized for lock-free exchange → an
  RT-friendly *kernel* type, over which any typed facade can later be layered
  without semantic change.
- **`clock_tag` + `topology::regions` + `crossings` +
  `link<latest|queued>`**: a multi-clock system where cross-clock
  communication is explicit and policy-tagged. In rhine's vocabulary: regions
  are clocks, crossings are resampling buffers, `latest`/`queued` are
  resampling policies (`hold`/sample vs. `collect`/FIFO).
- **`delay`/`hold` state**: the state model is a *unit delay* (`pre`): state
  is an edge that reads the previous tick's value; cycles are legal **iff**
  they pass through a delay. That relaxes "reject cycles" to "reject
  delay-free cycles" (exactly Lustre's causality rule, L7) and makes state
  *data*, i.e. snapshot-able — which is what time-travel debugging needs.
  Landed in A1/A2; note that `debug_engine`'s existing trace (a `value`
  event per node per step, with a sequence number) is *already* a time-travel
  log the moment state is explicit.

The scaffolding's silent choices: **discrete time, batch ticks, static
topology, explicit cross-thread resampling, explicit state (landed:
A1/A2)**. The flavour question reduces to: keep these, or replace the
evaluation core to get Sodium's semantics?

## 3. Domain constraints that should drive the choice

1. **Audio recomputes everything every block anyway.** Faust, SCADE, and
   every audio callback evaluate the whole graph per block/sample. Sodium's
   core optimization — propagate only what changed — buys almost nothing on
   the audio path. It buys a lot on the UI path (sparse in time), but UI
   paths have no RT constraints, so a full tick is affordable there too at UI
   rates.
2. **RT-safety wants predictability, not average speed.** Fixed schedule ⇒
   worst case = average case; no heap traffic, no locks, no priority queues
   on the audio thread. Push engines pay heap + queue + transaction
   bookkeeping *per event*; Sodium's reference implementations serialize
   updates through a global transaction critical section — a non-starter
   as-is for audio, meaning a from-scratch runtime would be needed anyway, at
   which point "API compatibility with Sodium" is the only thing kept.
3. **Debuggability (a stated core constraint) wants static topology +
   explicit state.** Diagrams need a graph that exists before it runs;
   time-travel needs state that is *values*, not closures captured inside
   operators. Elm abandoned FRP (0.17, 2016) largely over exactly this
   opacity.
4. **Adoption by non-FP C++ developers wants a small vocabulary and no arrow
   jargon.** `map / zip / hold / scan / merge / when / resample` is
   teachable; `arr / >>> / first / loop` is not. Arrowized *semantics* do not
   require arrow *syntax* — bearriver shows the Yampa API survives on an MSF
   kernel; vocabulary is a choice, not a consequence.

## 4. Flavour options

### F1 — Classic FRP, Sodium-style (`Cell<T>` / `Stream<T>`, push, transactions, `switch`)

- **Semantics**: behaviors and events as first-class, timeless values;
  glitch-free propagation via rank-ordered queues; `switch` for dynamics.
- **Pros**: the Blackheath–Jones book is the best FRP teaching material in
  existence; the talk's demo is already planned on sodium-typescript; small,
  memorable user vocabulary.
- **Cons for this project**: per-event allocation and listener bookkeeping;
  transactions serialize; space/time management depends on disciplined
  listener hygiene; dynamics (`switch`) mutate the graph at runtime →
  topology becomes a moving target → static diagrams, preallocation,
  deterministic order, and link placement all degrade. The C++ port
  (`SodiumFRP/sodium-cxx`) exists but the operational core would still need a
  full RT rewrite — so the project would carry Sodium's *runtime complexity*
  without reusing its code.
- **Verdict**: keep as the **demo's teaching semantics** (the talk already
  frames it this way); do not make it the engine.

### F2 — Arrowized FRP (Yampa → Dunai/MSF, bearriver)

- **Semantics**: signal *functions* (`SF a b` / `MSF m a b`) are first-class;
  signals are not. State is explicit via `loop`/`pre`; networks are values
  composed from ~5 primitives.
- **Pros**: kills the classic space-leak class *by construction* (no
  first-class signals to retain); tick-based, allocation-free evaluation is
  achievable; state being data makes snapshotting/time-travel natural (the
  Dunai literature even has a "Back to the Future: time travel in FRP"
  paper); networks-as-values directly answers the "local snippets that
  coalesce" question.
- **Cons**: dynamic switching is the hard part (needs bounding for RT); C++
  ergonomics of arrows are grim if exposed raw; smaller teaching corpus for a
  C++ audience.
- **Verdict**: adopt the **structural discipline** (values, explicit
  delay-state, bounded dynamics), hide the arrow vocabulary.

### F3 — Clocked synchronous dataflow (Lustre/Signal/SCADE; rhine in FRP-land)

- **Semantics**: signals are typed *by their clock*; each clock ticks as a
  unit; state via `pre`; cross-clock flow only through explicit resamplers;
  schedule is static.
- **Pros**: this is *literally what the engine already computes* —
  `clock_tag` regions are clocks, `crossings` are resampling sites,
  `link` modes are resampling policies. Determinism, bounded memory,
  static schedule: RT-safety falls out rather than being bolted on.
  Cross-thread data-flow semantics become *part of the language* (choosing
  `latest` vs `queued` is a semantic act the user must state) instead of an
  implementation detail. Rhine demonstrates the type-level discipline;
  Lustre/SCADE demonstrate 30 years of industrial scaling of exactly this
  model.
- **Cons**: least famous under the "FRP" brand; dynamic reconfiguration is
  limited (see §7); fewer books — but the talk is the venue that introduces
  it, and "clocked FRP for audio threads" is a *stronger* talk than "yet
  another FRP library".
- **Verdict**: **this is the kernel semantics.** The engine already is a
  synchronous dataflow machine; name it, and let the API speak in clocks and
  resamplers.

### F4 — Rejected without ceremony

- **Pull/continuous classic FRP (Fran, Reflex)**: sampling-based evaluation
  fights the audio callback model; Reflex's pull machinery is heavyweight
  even in Haskell.
- **Rx / observables (RxCpp, rpp)**: no glitch-free simultaneity semantics,
  heap-everywhere, scheduler abstraction hostile to RT. cpp.react (the
  closest serious C++ "reactive" library) is long unmaintained/deprecated —
  a useful cautionary data point for the talk's "where existing C++ options
  fall short" slide.

**Recommendation: F3 kernel + F2 structural discipline + an F1-flavoured
teaching vocabulary** (`hold`, `snapshot`-like operators where harmless) —
and Sodium stays in the demo as pedagogy, with the talk explicitly narrating
"these semantics are beautiful; here is why the *engine* evaluates
differently".

## 5. Client API shapes — four alternatives, one running example

Running example (from the talk): the UI thread owns a BPM slider, loop
points, and a seek action; the audio thread owns the block clock, advances a
playhead in beats, and a scheduler emits trigger events when the playhead
crosses loop boundaries. Cross-thread: `bpm` and `loops` as latest-value
snapshots; `seek` as a queued event.

A C++ note on spelling used below: C++ has no `>>>` or `&&&` operators, so
Haskell's arrow operators are spelled `>>` (compose) and `zipN`/`parallel`
(fan-out) in these sketches.

### Shape A — Central typed graph builder (the original sketch; superseded by Shape C)

One translation unit declares the whole reactive system; the app hooks
inputs/outputs. Typed handles (`sig<T>`, `evt<T>`) over the existing erased
kernel.

```cpp
// transport.cpp — the reactive system is a single declarative asset
transport_io build_transport(builder& g)
{
    auto clock = g.input<block, thread::audio>("clock");
    auto bpm   = g.input<double, thread::ui>("bpm");
    auto loops = g.input<loop_region, thread::ui>("loops");
    auto seek  = g.input<beat_t, thread::ui>("seek");

    auto bpm_a  = at<thread::audio, latest>(bpm);    // link: snapshot
    auto loops_a= at<thread::audio, latest>(loops);  // link: snapshot
    auto seek_a = at<thread::audio, queued>(seek);   // link: FIFO

    auto prev = g.delay(0.0_b, thread::audio);       // unit delay (state)
    auto play = g.lift(thread::audio, advance_playhead,
                       prev, clock, bpm_a, seek_a);
    prev.feed(play);          // cycle through delay: legal

    auto trig = g.lift(thread::audio, schedule_events, play, loops_a);
    return { bpm, seek, loops, clock, g.output(play, "playhead"),
             g.output(trig, "triggers") };
}

// app.cpp — hooks
kernel::release_engine eng{ g };        // topology, links, memory: precomputed
slider.onValueChange = [&](double v){ io.bpm.send(v); };   // ui thread
// audio callback:
eng.tick(thread::audio, clock_block);                        // audio thread
```

- **Pros**: maximal static analysis (diagrams, validation, preallocation);
  traceability; matches today's code with minimal churn; the
  `debug_engine`/time-travel story is strongest here.
- **Cons**: the "two worlds" tension — a corner-widget animation must "reach for the global definition";
  locality is poor.

### Shape B — Live reactive values, Sodium-style

Cells/streams are objects you create anywhere; operators mutate a runtime
registry; ownership is shared and implicit.

```cpp
class transport_bar : public juce::Component {
    cell<double>   bpm_  { 120.0 };        // reactive value lives next to the slider
    stream<beat_t> seek_;
public:
    explicit transport_bar(runtime& rt) {
        slider_.onValueChange = [&]{ bpm_.send(slider_.getValue()); };
        seek_btn_.onClick     = [&]{ seek_.send(position_beats()); };
        bpm_.bind_to(rt, thread::ui, "bpm"); // publishes into the engine's ui region
    }
};

// audio side, in a different class/file:
auto playhead = clock_.accum(0.0_b, advance_playhead,
                             bar_.bpm().at<thread::audio>(),
                             bar_.seek().queue<thread::audio>());
```

- **Pros**: unbeatable locality and progressive adoption — sprinkle FRP
  exactly where the problem is; familiar to anyone who read the Sodium book.
- **Cons**: the graph is *implicit and dynamic* — nodes are side effects of
  operator calls, lifetimes are reference-counted, update ordering is
  subtle, and static diagrams require a separate capture mode. Worst RT story
  of the four (allocation/lifetime discipline must be re-engineered onto
  pools, and dynamics fight preallocation). Also the worst "why did this fire
  twice?" debugging experience — the very failure mode this project exists to
  fix.

### Shape C — Signal networks as composable values (MSF-style)

#### C.1 The mental model, in one paragraph

A `recipe<A, B>` is **not a live connection to a runtime**. It is a *recipe* for
a subgraph — an immutable, by-value description of nodes and edges (a flat
`node` arena, not a shared pointer; see `include/clockr/shell/node.hpp` for
why that matters to constexpr construction, and why the copy cost was accepted).
Operators build bigger recipes from smaller ones. Nothing exists
in any engine until a recipe is *instantiated*: `engine eng{ app }` walks the
recipe once (at startup, off the real-time path), emits the flat
`kernel::graph` that already exists today, precomputes topology and links,
and from then on evaluation is **exactly the machine already implemented in
`release_engine`** — topological order, batch ticks, links at sharing
points. The recipe can be destroyed after construction; the runtime never
sees `recipe` values at all.

This is the MSF insight (Dunai: "an MSF is a time-varying transformation
applied to a series of inputs as they come along, one by one"), with a
C++-specific reframing: because our runtime representation is an explicit
graph rather than a closure chain, "network as value" means "network as
*buildable description*" — and instantiation is where the description becomes
the static kernel of §2.

#### C.2 User side, worked example

Clocks are anchored at boundaries, and only there: `input` and `output`
ports name their clock, `at<>` names crossings, and every interior node —
everything between two anchors — inherits the clock of the nearest upstream
anchor. There is no "ambient" thread: `engine eng{ app }` derives the whole
clock structure from the ports (no build parameter, no `on` operator). An
output's tag is a *check* (a chain ending on a different clock fails to
build), an input's tag is a *seed* (it establishes the clock of everything
after it, until the next anchor).

```cpp
// ---------------------------------------------------------------- leaves
// input<T,Tag>(name) : recipe<void, T> — a source port living on clock Tag.
auto clock = input<block, thread::audio>("clock"); // frames+rate for this tick
auto bpm   = input<double, thread::ui>("bpm");
auto loops = input<loop_region, thread::ui>("loops");
auto seek  = input<beat_t, thread::ui>("seek");

// -------------------------------------------------- the audio-side network
// at<clock, policy>(src) : the value of a source as seen on *this* clock.
// This is where cross-thread flow — and its policy — is *declared*;
// the link is born here, not hidden in a runtime.
auto bpm_a  = at<thread::audio, latest>(bpm);   // snapshot: current slider value
auto loops_a= at<thread::audio, latest>(loops); // snapshot: current loop region
auto seek_a = at<thread::audio, queued>(seek);  // FIFO: no seek may be lost

// hold(init, f) : stateful network.
//   f : (state, in) -> state  — a *pure* function; the state lives in the
//                               engine's state slots, never in the closure
//   out each tick = the new state.
struct transport_in { block clock; double bpm; loop_region loops; maybe<beat_t> seek; };

auto playhead = hold(0.0_b, [](beat_t prev, const transport_in& in) -> beat_t {
    if (in.seek) return *in.seek;                  // seek wins this tick
    return prev + beats_in(in.clock, in.bpm);      // else advance by the block
});

// zipN : (recipe<void,A>, recipe<void,B>, ...) -> recipe<void, tuple-ish>
// >>   : recipe<A,B> then recipe<B,C>                     (C++ spelling of Haskell's >>>)
auto transport = zip4(clock, bpm_a, loops_a, seek_a) >> playhead;   // recipe<void, beat_t>

// ------------------------------------------------ a corner-widget network
// Same kind of value, smaller scale. Defined next to the widget, not in a
// central file. The widget owns no runtime state — only a recipe.
auto blink = ui_clock
         >> every(250ms)                          // recipe<duration, evt<>>
         >> toggle()                              // recipe<evt<>, bool>  (hold(!prev) sugar)
         >> arr([](bool on) { return on ? 1.f : .2f; });

// ------------------------------------------------------- composing the app
// share(recipe) : instantiate once, fan out. Without it, each *use* of a network
// value instantiates its own copy — including its own state slots (two
// independent playheads!). In Shape B, sharing is implicit reference
// semantics; here it is an explicit operator, so state duplication is
// *visible in the code*, not discovered at runtime.
auto t = share(transport);

auto app = parallel(
    t >> scheduler{} >> output<evt_batch, thread::audio>("triggers"),
    t                 >> output<beat_t, thread::audio>("playhead"),
    blink              >> output<float, thread::ui>("blink_alpha"),
    controls
);

engine eng{ app };      // instantiate + freeze (startup; non-RT).
                        // `app` and every recipe value may now be destroyed.
```

Driving the engine from the application (the engine owns no threads — each
owning thread ticks its own region, which matches the project's
"callers pick a class directly" philosophy):

```cpp
// ui thread (message loop / timer):
eng.in<double> ("bpm")  .send(132.0);
eng.in<beat_t> ("seek") .fire(16.0_b);
eng.tick(thread::ui);                    // evaluates the ui region once

// audio callback (real-time thread):
eng.tick(thread::audio, clock_block);    // drains links, evaluates audio region
auto batch = eng.out<evt_batch>("triggers");
```

#### C.3 Engine internals — three phases

**Phase 1 — instantiate (startup, non-RT).** A recursive walk of the recipe
tree. Each operator knows how to emit its fragment into a
`builder` — today's `graph`, plus named ports and delay nodes:

| recipe node | instantiation emits |
|---|---|
| `input<T,Tag>(name)` | one `node_kind::input` node tagged `Tag`; port-table entry `name → id`. *Seeds* the walker's clock (`Tag`) for everything downstream |
| `arr(f)` / `lift` | one internal node on the walker's current clock; `tick_fn` = `value`-adapting wrapper of `f` |
| `a >> b` | fragment(a); fragment(b); edge a.out → b.in — both walk on the same `clock`, so a chain stays on one clock |
| `zipN(...)` | one fragment per branch, each walking on a *copy* of `clock` (branch-local crossings stay branch-local); the branch outputs become the fragment's output span, in branch order — **no gather node**: the erased kernel cannot carry a pair-valued edge, so "zip" is a *wiring* notion (the downstream node ends up with N fan-in edges in zip order, L8) |
| `hold(init, f)` | one **delay node** (owns a state slot seeded with `init`) + one lift node for `f`, on the walker's current clock; edges delay→lift and lift→delay (feedback); consumers wired to the lift node |
| `at<tag, mode>(src)` | boundary marker: the edge leaving `src`'s region is annotated with `mode` and the walker's clock re-anchors to `tag` for the rest of the chain (found later by `topology::crossings`) |
| `output<T,Tag>(name)` | one `node_kind::output` node tagged `Tag`; port-table entry. *Checks* the walker's clock: a chain arriving on a different `clock` fails to build (cross-clock flow needs an explicit `at<>`) |
| `share(recipe)` | instantiate once per (recipe, clock-at-entry), memoize; further uses = fan-out edges from the same output node |

The walker's clock is `std::nullopt` at the root: any fragment
reached with no clock established seeds itself from its **first anchor** — the
leftmost `input` in walk order. A fragment with no anchor anywhere (nothing
feeds it) is a build error, not a guess.

**Phase 2 — freeze (startup, non-RT).** Everything the current code already
does, plus the new allocations:

- `topology` construction: regions, crossings, deterministic evaluation
  order — with the delay rule: **cycles are rejected only when delay-free**;
  delay nodes are *seeds* in Kahn's algorithm (their tick-`t` value is
  already known at tick start: it is tick `t−1`'s result), so feedback
  through them never breaks the topological order.
- one `conc::link` per crossing, in the annotated mode
  (`latest` → `double_buffer`, `queued` → `spsc`);
- the value array and the delay state slots, allocated from `mem::pool`;
- the port table resolved to plain indices.

After freeze, the engine is arrays and function pointers. The recipe AST is
garbage and may be freed.

**Phase 3 — run (real-time).** A region tick, e.g. `tick(thread::audio, block)`:

1. **Fill**: for each crossing entering the region, drain the link —
   `queued`: pop every pending value (bounded, non-blocking; the example's
   `maybe<beat_t> seek` is "last seek this tick, if any"); `latest`: one
   atomic load. Full queues fail `send()` at the *producer*, never block the
   consumer — the policy (drop/merge) stays at the sending side.
2. **Evaluate**: walk the evaluation order exactly like today's
   `release_engine::evaluate`. Delay nodes read their state slot (tick `t−1`);
   all other nodes compute from fan-in; no allocation — `tick_fn`s are
   *called*, never *created*.
3. **Settle**: write the newly computed values of delay-node inputs into the
   state slots (now tick `t`'s state); push the region's outputs into the
   outgoing links and output ports.

What the transport example looks like after instantiation — *this is the
existing `graph` type, nothing new*:

```
 ui region (clock: ui)               audio region (clock: audio)
 ┌────────────┐                     ┌───────────────────────────────────────┐
 │ [bpm_in]   │ ══ latest ══▶ [bpm_rx]  ─┐                                │
 │ [loops_in] │ ══ latest ══▶ [loops_rx] ┤                                │
 │ [seek_in]  │ ══ queued ══▶ [seek_rx]  ┤                                │
 └────────────┘                [clock_in] ─┴─▶ [advance] ─┬─▶ [sched] ──▶ [trig_out]
                                              ▲        │   └────────────▶ [playhead_out]
                                              │        ▼
                                            [delay] ◀──┘   state slot (init 0.0);
                                              feedback edge advance → delay,
                                              legal because it passes through delay
```

(`══` = link at a crossing)

Two properties fall out of this phase design for free:

- **Time-travel**: all state lives in the value array and the delay slots.
  `debug_engine`'s per-node value trace *is* the replay log; scrubbing =
  restoring slots + re-ticking. No closure archeology.
- **Determinism checking**: purity of user `tick_fn`s cannot be *enforced* in
  C++, but it can be *detected*: tick twice from identical slots+inputs and
  compare — a natural future `debug_engine` feature.

#### C.4 Where the types go: typed shell, erased kernel

`recipe<A, B>` type-checks composition at compile time; the kernel stays exactly
the erased `kernel::value` / `tick_fn` machine of today:

```cpp
template<typename A, typename B, typename skeleton_t, std::size_t N>
class recipe {
    std::array<node, N> m_arena; // by-value type-erased AST; a constant
    node_ref m_root;                 // index of the root node into m_arena
public:
    template<typename C>
    friend recipe<A, C> operator>>(recipe lhs, recipe<B, C> rhs); // B must match: compile time
};

template<typename A, typename B>
recipe<A, B> arr(std::function<B(const A&)> f)
{
    return recipe<A, B>{ detail::make_lift(
        [f = std::move(f)](std::span<const kernel::value> in) -> kernel::value
        {
            return detail::to_value(f(detail::from_value<A>(in.front())));
        })};
}
```

The `std::function<value(std::span<const value>)>` already in `kernel::node`
*is* the erased representation; `recipe` adds compile-time checking at
composition and value conversion at the edges. Type errors surface where they
belong — in user code, at compile time, while writing `a >> b` — never
inside the engine at runtime. Keeping the kernel erased also keeps template
depth (and therefore error messages and compile times) shallow: recipes are
one node per operator, not expression-template towers.

#### C.5 What `switch` would mean (deferred)

MSF-style `switch` replaces a running sub-network mid-stream. Over a static
kernel that has exactly two honest realizations:

- **Bounded alternation** — all branches instantiated up front, a multiplexer
  node selects the active one (both tick, or the inactive branch is gated).
  RT-safe because everything is preallocated; this covers mode switches,
  preset changes, enable/disable.
- **Rebuild-and-swap** — a new recipe is instantiated on a non-RT thread and
  the topology is swapped at a region boundary (double-buffered). Covers
  structural changes (track added, plugin loaded).

True unbounded `switch` on the audio clock is explicitly out of scope — that
is the honest cost of F3, and polyphony shows why it rarely matters: voice
allocation is conventionally a *fixed pool of preallocated voices* selected by
values, which is bounded alternation, not dynamic graph growth.

#### C.6 Ergonomics notes

- `>>` (right-shift) is the C++ spelling of composition; a variadic
  `pipe(a, b, c)` helper can flatten long chains if preferred.
- `recipe` values are immutable, by-value recipes: copying copies the
  *description*, never runtime state (state exists only post-instantiation,
  inside the engine). The by-value arena means composition deep-copies child
  arenas (O(n²) in copies) rather than bumping a shared pointer — the accepted
  cost of making the recipe a compile-time constant.
- Recipe building is startup work (`prepareToPlay` time in plugin terms);
  nothing in Shape C runs on the RT path except Phase 3, which is the same
  straight-line array walk the engine already performs.

**Summary of Shape C**: it is Shape A where subgraphs are first-class values.
The locality of B, the explicitness of A, one kernel for both — and the
"central vs. local" dilemma dissolves, because a widget's animation network
and the whole application's transport network are the *same kind of thing*,
composed rather than registered.

### Shape D — Clocked-equation DSL (Lustre-like surface; deferred)

```cpp
network transport {
    in  bpm  : signal<double> @ ui;
    in  seek : event<beat>    @ ui;
    out trig : event<note_on> @ audio;

    playhead : signal<beat> @ audio
        = pre(0.0) + clock.beats(bpm @-> audio / latest)
          reset_by (seek @-> audio / queued);

    trig = crossings(playhead, loops @-> audio / latest) @ audio;
}
```

- **Pros**: the most readable possible statement of the domain — clocks and
  resampling policies are *in the syntax*; ideal for codegen, diagrams,
  documentation.
- **Cons**: tooling cost (parser/codegen, or a macro-heavy fluent API);
  premature today. It is a *surface* over the same kernel — Shape A's builder
  can later gain this syntax without semantic change.

### How the shapes relate

B requires an F1-style implicit runtime — it is the outlier. **A, C and D are
three surfaces over the F3 kernel**; C is best understood as "A where
subgraphs are first-class values", which is what dissolves the
central-vs-local tension. Build **A first** (minimal delta from current
code), grow **C's value-composition** as a library of named sub-networks,
keep **D** as a possible talk-worthy future surface.

## 6. How the decisions scale

### Flavour × {maintenance, performance}

| | **F1 Sodium** | **F2 Arrowized/MSF** | **F3 Clocked synchronous** |
|---|---|---|---|
| Runtime complexity | High: transactions, rank queues, listener GC, `switch` re-wiring | Low: ~5 operators, tick loop | Lowest: static schedule + resamplers |
| Space/time leaks | Managed by discipline (listeners, keep-alive); leak classes exist | First-class-signal leaks impossible; user-accumulated state still possible | Same as F2, plus static bounds enforceable at build time |
| RT-safety | Requires full rewrite of operational core; worst case unbounded (dynamics) | Achievable: allocation-free ticks; dynamics must be bounded | Native: worst case = average case; memory pre-planned (`mem::pool::snapshot()` becomes a *semantic* guarantee) |
| Audio-rate perf | Push machinery wasted — audio recomputes all per block anyway | O(region) per tick, cache-friendly layout possible | Same, plus schedule/layout fully controllable (arrays per region) |
| UI-rate perf | Wins when graphs are huge and updates sparse | Tick cost even when idle | Tick cost even when idle (acceptable at UI rates; "wake on link" can gate idle regions later) |
| Debuggability | Implicit graph, closure-hidden state — poor | Explicit state; structure inspectable with effort | Static topology + explicit state — best (diagrams, replay, trace = time-travel log) |
| Maintenance burden for the library author | Own a subtle runtime forever (sodium-cxx is not RT; semantics would be forked) | Own a small kernel; complexity concentrated in bounded `switch` | Own the least machinery; complexity concentrated in the resampler library |

### API shape × {maintenance, adoption}

| | **A central builder** | **B live values** | **C network values** | **D equation DSL** |
|---|---|---|---|---|
| Locality | Poor | Best | Good | Medium |
| Static analysis / diagrams | Best | Worst (needs capture mode) | Good (values are inspectable) | Best |
| Progressive adoption into existing OO codebases | Hard (big-bang) | Easy | Medium (wrap a class per network) | Hard |
| Error messages / C++ ergonomics | Good | Good | Template noise (mitigated by erased kernel) | Tooling-dependent |
| Refactor scaling (big apps) | Central file becomes a bottleneck; needs namespacing discipline | Runtime coupling grows invisibly | Composes: app = tree of named networks | Composes |
| Time-travel / trace | Trivial | Hard | Trivial | Trivial |

### The interaction — the decisive point

- **F1 forces B**: live cells/streams *are* the Sodium model; the graph
  cannot be pre-declared because operators mutate it. Picking the flavour
  everyone knows *removes* both the API options and the debuggability story
  simultaneously.
- **F3 keeps A, C, D open** on one kernel. It can be answered *empirically*,
  by shipping A and observing where locality hurts, without ever invalidating the kernel.
- On the "two worlds" worry: under F3/C the pure-reactive world is just
  values and pure functions; impurity lives only at region boundaries
  (inputs, outputs, links) — which is **exactly the boundary C++ audio
  developers already accept** (the audio callback). The split follows their
  existing mental model instead of fighting it. That is the best available
  answer to the Elm-adoption failure mode: the mental friction is confined to
  a place where it buys something already wanted (thread safety,
  debuggability).

## 7. Concrete path, mapped to the modules

1. **Kernel (graph)**: implement state as **unit-delay nodes**
   (`g.delay(init, thread)` + `delay.feed(x)`); relax topology to reject only
   delay-*free* cycles, with delay nodes as seeds in Kahn's algorithm. This
   closes the `// TODO Hold state` in the integration test with the
   semantically right primitive.
2. **Clocks**: formalize `clock_tag` regions as clocks in documentation and
   API names (`at<clock, policy>` at boundaries instead of bare `connect`
   across threads); `topology::crossings` already finds where the
   links go.
3. **Engine**: per-region `tick` entry points (audio callback ticks the audio
   region; the UI loop ticks the ui region), links drained/filled at
   region entry — the multi-threaded evaluation the README already announces.
4. **Facade**: typed handles (`sig<T>`, `evt<T>`, then `recipe<A,B>` recipes)
   over the erased `value` kernel; keep `value` as the RT wire format. Defer
   static typing of the kernel itself.
5. **Operator vocabulary**: `arr/map`, `zipN`, `lift`, `hold` (delay+feed
   sugar), `scan/accum`, `filter/when`, `merge`, `at` (resample at a
   boundary), `share` — no arrow names. Threads are not part of the
   vocabulary beyond ports and `at<>`; there is no `on`-style placement
   operator. This is Shape A now, and each named sub-graph is already a
   value, so Shape C emerges by *naming and returning subgraphs* rather than
   by building a second system.
6. **Dynamics (deferred, bounded)**: polyphony/track-count changes = the hard
   case. The synchronous answer is a **preallocated fixed voice pool**
   (static graph, voices enabled/disabled by values) plus **off-audio-thread
   graph rebuild with double-buffered topology swap** for rare structural
   changes. Do not promise Sodium-style `switch` on the audio thread.
7. **Debug story**: `debug_engine`'s per-node value trace + explicit delay
   state = replayable execution; `graph` serialization → diagram export. Both
   are small steps from current code *only because* the kernel is static and
   state is explicit.

## 8. What would change this recommendation

- If progressive, sprinkle-it-anywhere adoption turns out to be *the*
  make-or-break requirement (e.g. the library must sell into a large existing
  JUCE codebase piecemeal), Shape B's pull is strong — but the honest answer
  then is lager-style **unidirectional value flow** (central model + pure
  update functions + cursors; time-travel "for free"; proven in the C++ audio
  industry) rather than Sodium FRP. It is the same lesson as Elm's post-0.17
  architecture, arrived at independently in C++.
- If the demo's pedagogy demands Sodium semantics *in the product* (not just
  the web demo), F1 becomes tempting — resist by remembering the engine would
  carry the runtime cost while reusing none of sodium-cxx's code.

## 9. User value types — decision record

The kernel began with a closed value variant (`monostate`/`bool`/`int64`/
`double`). Applications need their own structs (loop regions, event batches,
presets). The decisions, and the options rejected:

- **Hybrid representation, not "everything pool-allocated".** Small
  trivially-copyable structs are stored inline in the variant; only large or
  non-trivially-copyable ones take a ref-counted pool cell. *Rejected:* routing
  every struct through the pool — it makes the common audio case (a two-double
  snapshot) pay allocation, atomic refcounting, and the `latest` race for
  nothing. *Rejected:* capping structs to inline-only (no pool) — it cannot
  carry an owning type at all.
- **A raw handle, not a smart pointer, in the variant.** `kernel::value` must
  stay trivially copyable (the `latest` link copies bytes with atomics, the
  trace travels a lock-free queue, the engine copies per fan-in edge), so a
  `shared_ptr` member is out. Ownership is an explicit retain/release discipline
  at the points a value escapes a tick — the same shape C++ audio code already
  uses for reference-counted buffers. The variant stays 32 bytes.
- **An explicit `tick_context`, not a thread-local pool.** A tick that builds a
  user value needs an allocator; the engine is the only thing that knows which
  region is running. Threading a context through `tick_fn` is the same
  anti-ambient stance that removed the ambient `clock_tag`/`shell::on` (FRP §5,
  Shape C).
- **`latest` needs grace periods, not prompt frees.** A lock-free `latest`
  reader can be mid-`retain` when the producer overwrites and releases; the
  sound design (G5) retains inside the seqlock read and reclaims only after
  every consumer region has passed the release epoch. `queued` is a plain
  ownership transfer and needs none of it.
- **Pure-data metadata, not user callbacks.** `describe<T>` yields an
  offset+kind field table walked by the debug codec on the server thread; the
  user's code never runs off the RT thread to serialize. A type with no
  `describe` renders as an anonymous `struct` — the fallback is the zero-
  metadata case of the same path, not a separate feature.

Status: representation, SF conversion, and the recipe/program plumbing are
landed (ROADMAP Phase G1–G3); engine ownership and cross-thread transport
(G4–G5) are the open remainder.

## Appendix — verified landscape notes (Aug 2026)

- **SodiumFRP/sodium-cxx**: the official C++ port, under the multi-language
  Sodium org. Fine as a semantics reference; its operational core
  (shared-ptr graph, transactional updates) is not RT-oriented.
- **cpp.react (snakster)**: C++14 push-reactive library, long
  unmaintained/deprecated; good cautionary cite for the talk.
- **Dunai / bearriver / Yampa** (ivanperez-keera): MSF kernel; bearriver
  re-implements the Yampa API on MSFs — evidence that arrowized semantics can
  hide behind a friendly vocabulary. The Dunai literature includes
  time-travel and testing frameworks for FRP.
- **Rhine** (turion): "clock-safe library for synchronous and asynchronous
  FRP; separates clocking, scheduling and resampling; clock safety at the
  type level" — the direct academic precedent for "threads as clocks,
  links as resamplers". Cite it.
- **Lager** (arximboldi): Elm/Redux-style unidirectional value flow for
  C++17, time-travel debugger included — the strongest non-FRP competitor for
  the adoption argument, worth benchmarking DX claims against.
