# GLOSSARY

*One canonical term per concept.*

These definitions are the project's vocabulary. Documentation and code use these terms.
The code map at the end identifies where each concept lives in the source.

**Keep these two levels separate.**
The *semantic* level says what a program means.
It includes `clock`, `epoch` / `step`, `stream`, and `value`.
The *operational* level says how the program runs.
It includes `recipe`, `descriptor`, `program`, `crossing`, `policy`, `link`, and `engine`.

The documentation follows the same split.
`doc/reference/denotational-semantics.md` is the semantic reference.
`doc/learn/layering.md` is the operational guide.
`doc/explanation/frp-decisions.md` and `doc/explanation/operator-sufficiency.md` give the rationale.
`doc/README.md` is the reading map for all documentation.

## Framework

### shell / kernel

The library has two halves and two top-level modules: `shell/` and `kernel/`.
The **shell** is the typed composition layer.
Its recipes have C++ types, and composition checks those types.
Recipes are immutable and constant-evaluable.

The **kernel** is the erased execution machinery.
It lowers recipes to programs.
It transports `kernel::value`, with no knowledge of C++ types.
The interface between the shell and the kernel is marshaling and the tick context.
Marshaling uses `to_value` and `from_value`.
Type errors are shell-time events, ideally at compile time.
They never become kernel lookups.

*Notes.*
"Erased" is the important word.
The kernel delegates all type correctness to the shell.
This separation lets the shell change names or syntax without changing the semantics.

`replay/` is a separate, opt-in module built only on the kernel.
It records boundary journals, replays them into fresh programs, and compares
runs; it is not a third semantic half and adds no evaluation semantics.
See [Replay](#replay).

### host

The host is application code that owns the threads and drives the library.
It builds a program, sets input ports, and reads output ports.
It calls `engine::step` from each clock's thread.
It sizes the region pools and chooses the policies it declares.

The library owns no threads and no event loop.
The host owns every effect at the boundary: I/O, scheduling, and error policy.
The host must keep the threading contract:
one tick at a time per region, and set-then-tick quiescence.

### network

A network is a composed reactive system as a whole.
The host builds and runs it.
A network is not a C++ type.
During composition, its code form is a recipe.
Between composition and execution, its form is a descriptor.
After construction, its form is a program.

A part of a network is a fragment of a recipe.
The lowered result is a program.

## Time

### clock

A clock is a tick source: a strictly increasing map `ℕ → Time`.
`c(t)` is the physical instant of tick `t`.
Only the tick *index* matters inside a clock.
Physical time relates clocks to each other.

In code, a **label** names a clock with `kernel::clock_tag`: `audio`, `ui`, or `worker`.
A clock's **region** is the set of nodes that tick on it.
`engine::step` steps one region.

*Notes.*
"Threads are clocks" is a slogan, not a definition.
One host thread usually drives one clock.
A clock is a tick source, not an OS thread.
One test drives all three labels from one thread.
`topology` partitions nodes by label into connected components.
`program` keeps one execution plane for each label.
So same-label components tick together today.

*Not this.*
An OS thread, a thread pool, or a callback.

### epoch / step

An **epoch** is one tick of one clock, used as an atomic unit of observation.
It supplies a *map* of values, not a sequence.
Order inside an epoch is unobservable (L9a).
Epochs have order between themselves (L9b).

A **step** is the engine's effectful evaluation of one epoch.
A step has four phases:
drain → evaluate → settle → publish.
The drain phase reads inbound ports and links.
The evaluate phase invokes every node's function once, in topological order.
The settle phase updates delay slots and output ports.
The publish phase stamps the new epoch and releases the region's sends.

*Notes.*
A node's **tick function** is the pure, total function `Vᵏ → V`.
A step invokes it once per epoch.
See `kernel/node.hpp` and `kernel/node_descriptor.hpp`.
The function receives a `tick_context`.
This context carries the evaluating region's home; see `value_home`.

Purity is a semantic requirement, not a style choice.
A tick function determines its next value and must touch nothing else.
It does no I/O, uses no clocks, and changes no shared mutable state.
Observable effects stay strictly outside the tick function.
The step can drain links, trace, publish, and reclaim around the ticks.
The host can do port I/O and scheduling around the step.

A tick has only one privileged operation.
It can construct a user value by allocating from its region's pool.
This operation creates the new value.
Nothing can observe it as an external effect.

*Not this.*
`kernel::tick(node, …)` invokes one node's tick function.
It is not the engine's step.

### stream (signal / event)

Streams are the semantic objects that flow between nodes.
They are not C++ types.
A **signal** of type `A` on clock `c` is `Stream_c(A) = ℕ → A`.
It has exactly one value per tick.
An **event** is `Event_c(A) = ℕ → Maybe A`.
It has an occurrence or silence on each tick.

The engine realizes both as `value` streams on edges.
The shell marks an event-like port with `std::optional<T>`.
A disengaged optional means "no event this tick".
An event input returns to disengaged after the tick in which it fires.

*Not this.*
`kernel::value` is the runtime *transport* for these streams.
`std::span` is a view.
Sodium's `Cell` and `Stream` are a rejected API; see the index.

## Data

### value

`kernel::value` is the only type that flows at runtime:
`V = 1 ⊎ Bool ⊎ ℤ₆₄ ⊎ 𝔻 ⊎ U`.
It contains the payload-free unit marker `monostate`.
It also contains booleans, 64-bit integers, doubles, and **user values** `U`.
The unit marker is the shell's void or no-event value.
User values are application structs.
The kernel transports user values opaquely and never interprets them.

*Notes.*
`kernel::value` must remain trivially copyable and 32 bytes.
The `latest` link copies its bytes word by word with atomics.
The trace queue transports it without locks.
The engine copies it for each fan-in edge.
So a user value is a raw handle, never a smart pointer.

A user value uses **inline** storage for a trivially copyable struct of ≤16 bytes.
This form has value semantics.
Larger or non-trivial payloads use a **cell** reference.
This form has identity semantics.
The owning `value_home` manages its lifetime.

`user_value_c` is the admission predicate for `U`.
It accepts any class except the kernel's own markers:
`monostate`, `nullopt`, and `optional<T>`.
`optional<T>` is the event spelling and is deliberately excluded.

`describe<T>` can supply a field schema as `kernel::user_type`.
Only the observer and debugger use this schema.
The kernel reads opaque bytes and never reads the schema.

An `optional<T>` member uses the optional path for scalar payloads.
An engaged value renders its payload.
A disengaged value reads as absent.
The struct stays positional in both cases.

`to_value`, `from_value`, and the pool-aware `to_kernel_value` form the shell-to-kernel marshaling boundary.
Input is permissive; output is strict.

### value_home

`value_home` is the owning context of user values.
There is one home per region.
The program owns it, and its address is stable.
It holds the region's `mem::pool`, retire queues, and grace connection.
A cell remembers its home.
So a value that escapes a tick can return to its source pool.
A value can escape through a port, a delay slot, or a link.

*Notes.*
Reclamation is **deferred**.
Releasing the last reference stamps the cell and pushes it onto the home's retire queue.
The home's thread destroys and frees it later.
It does so after every region has moved past the cell's retirement epoch.
This grace period makes lock-free `latest` reads safe.
`program::reclaim` drives that work in bounded batches.

Allocation failure throws `std::bad_alloc`.
It means the host made `program_options::region_pool_bytes` too small.
Pool sizing is the host's obligation.
A null home selects the heap development and test path.
Ticks receive the region's home explicitly through `tick_context`.
There is no ambient allocator.

*Not this.*
Sodium's `Cell`.
Also not `shell::cell`, the closure-storage buffer inside the recipe AST; see the index.

## Planes

### recipe

A recipe is the typed shell's description of a network.
Its code form is a `shell::recipe<A, B>` value.
It is a by-value arena of AST nodes.
Children are referenced by index, with no pointers and no heap.
So a recipe can be a constant expression.
Copying a recipe copies the description, never runtime state.
Operators build larger recipes from smaller recipes.
Nothing runs, and no engine representation exists until a program is constructed.

*Notes.*
A fragment is any sub-recipe.
A network is not a separate concept.
A recipe is a recipe at any size.

Building a program walks the recipe once at construction.
`instantiate` accumulates into a **draft**, which is a scratch descriptor.
`shell::build` lowers the draft to the structural `graph` form.
`shell::compile` uses `constexpr` to snapshot it to a descriptor.
`share` memoizes for each `(m_id, clock)`.
`m_id` comes from `std::source_location`.

### descriptor

A descriptor is the frozen description of a program.
It is plain data:
nodes, edges, fan-in tables, evaluation order, regions, crossings, port tables, and user types.
Each node contains a function pointer and constant `self` state.
A descriptor is produced once and never mutated.
The engine reads it through spans.
The engine is **producer-agnostic**: it never asks whether the descriptor was compiled or hand-written.

*Notes.*
`descriptor_storage` is the constant storage returned by `shell::compile`.
`descriptor_view` is the transient span view of any such storage.
`program::descriptor()` also exposes this view.

The mutable structural form is `kernel::graph`.
`shell::build` and tests provide access to it.
It exists for structural analysis; evaluation never uses it.
`topology` is the derived analysis computed from a descriptor.
It contains regions, crossings, and evaluation order.
`node_label` contains an operator name and a port name.
It is descriptive only.

### program

A program is the execution plane.
It owns everything the engine steps except user closures.
It owns a descriptor's plain data.
It also owns per-region value storage, value homes, the grace clock, and links between regions.
Per-region storage contains input and output ports and delay slots.

Two construction forms produce one runtime shape.
Construction from a recipe clones stateful closures; the recipe can then be destroyed.
Construction from a compiled descriptor copies plain data.
It borrows `self` pointers and port-name bytes, so the descriptor must outlive the program.
The two constructions evaluate identically.
This is the **parity** invariant.

*Notes.*
The code form is `kernel::program`.
`engine::step(program, clock_tag)` steps one region at a time.
A region can move between threads between steps.
Two threads must never step the same region at the same time.

## Structure

### node / edge (fan-in)

Nodes and edges are the kernel's structural units.
A **node** is a vertex with an id, a kind, a clock label, and a tick function.
The kinds are `input`, `output`, `internal`, and `delay`.
An **edge** is a directed connection `(from, to)`.
A node's **fan-in** is its ordered list of incoming edges.
The order is the zip order (L8).
Nodes and edges are plain data.
A descriptor holds arrays of nodes and edges.

*Notes.*
"Node" has more than one related meaning.
Recipe nodes are `shell::node` values; see `recipe`.
Kernel nodes appear as `kernel::node` in the mutable structural form.
They appear as `kernel::node_descriptor` in descriptor form.
`topology` is the derived analysis over nodes and edges.
It contains regions, crossings, and evaluation order.

## Crossing clocks

### crossing

A crossing is a place where data moves from one clock to another.
The shell writes the annotation with `at<Clock, policy>`.
Topology derives one crossing record for each cross-clock edge.
The record contains the source, destination, and policy.
A cross-clock edge without an `at` is a build error.
Moving between clocks is always an explicit act with a policy.

### policy

The user chooses the declared semantics of a crossing:

- **`latest`** — sample-and-hold.
  The consumer reads the most recent value as of its tick.
  Several sends in one producer epoch coalesce; the last value wins.
  A silent epoch changes nothing.
  The hand-off race can move the observed cut by a bounded window.
  It never reorders or tears values.
- **`queued`** — order-preserving transfer.
  The consumer receives the values of the producer epochs since its previous tick.
  Values arrive in epoch order, and none are lost.
  Capacity is a *precondition* (L6).
  A full link drops a value, and `dropped_sends` makes the drop observable.
  The application must size the queue from the clock-rate ratio.
  Today, the consumer drains one value per step.
  The batch denotation is the target.

### link

A link is the runtime object that implements one crossing:
`conc::link<value, policy>`.
A `latest` link is an epoch-stamped seqlock ring.
It uses per-slot references and `retain_if_live` for lock-free readers.
A `queued` link is a bounded SPSC queue.

The consumer drains every inbound link at one snapshotted producer epoch.
So all links from one producer share a single cut.
A producer publishes one stamped epoch after all its links are staged.
A cut can be so stale that the ring lapped it.
The engine retries such a cut and counts it in `lapped_fills`.

*Notes.*
`slot` and `spsc` are the underlying primitives.
`double_buffer` is standalone because the `value` payload exceeds its inlineable bound.
The semantic word *resampler* names what a link computes.
Use `policy` and `link` instead.

## Operators

The canonical word is **operator**.
`combinator` remains only as a code identifier where the C++ keyword `operator` prevents the canonical name.
The identifier is `kernel::node_label::combinator`.
Use "operator" in prose.

### transform

`lift` lifts a pure function into a one-node fragment.
It is N-ary over pair-shaped inputs, which `zip` wires.
The wire type decides the arity.
A binary lift is `lift` over a `std::pair` wire.
Adjacent transforms can be fused (L3).

### wiring

Wiring uses the structural operators.
None of these operators computes a value by itself:

- `>>` — composition.
  The middle type must match; composition checks it.
- `share` — fan-out.
  Instantiate once for each `(m_id, clock)`, then fan the result.
- `zip` — shared-upstream fan-in.
  It is a pair-typed phantom that the kernel realizes as N fan-in edges in zip order (L8).
  It accepts N ≥ 2 branches, including two.
- `parallel` — disjoint chains with N outputs and no shared upstream.

### state

`delay` is the kernel's unit state.
It outputs its seed on the first tick.
It then outputs the previous tick's input.
It is the only construct that looks backward in time.
Every feedback cycle must cross a delay (guarded recursion, L7).
Delay-free cycles are rejected.
State lives in the program's delay slots as data, never in closures.
This representation makes snapshots and time travel possible.

`scan` and `stepper` are shell sugar over `delay` plus a step function.
`scan` folds state with `scan(init, f)`.
`stepper` carries the last event firing with `scan(init, value_or)`.
Both were first named `hold` and `sample_hold`.
Old commits and sketch code still use those names.

*Notes.*
User-typed seeds are **materialized** at program construction into program-owned values.
This never occurs on a ticking thread.
The engine *settles* delay slots at the end of every step.
During tick `t`, a slot still contains tick `t−1`'s result.

### boundary

`input` and `output` are the ports where a network meets the host.
They are the only places where a clock is anchored.
An input **seeds** the clock of everything downstream until the next anchor.
An output **checks** that the chain arrived on the same clock.
A crossing needs an explicit `at`.
Ports are named and indexed by declaration order.
Hosts set and get them through the program.

## Execution

### engine

An engine is the interpreter that steps a program.
`release_engine` is lean.
`debug_engine` is observing.
Both have the same denotation and satisfy L5 parity.
One engine instance steps one region at a time.
Regions on different clocks can be stepped from different threads.
Engines own no threads.
The host calls `engine::step(program, clock_tag)` from the thread that owns that clock.

The engine also provides a between-tick extension seam.
A host can attach a `kernel::step_hook` with `engine::set_step_hook`.
The engine calls the hook just before and just after each step.
The debugger's command injection uses this seam.
This hook is *not* the `observer` below.
A hook acts at the boundary.
The observer is the step's reported trace.

### observer

The observer is the debugging face of execution.
For each epoch, it reports an ordered list.
The list contains every node's value and is the observation map ω.
The map is reported through `trace_sink` as `trace_event` values bracketed by region epoch.
The trace is an **observation stream**, not replay history.
It carries measured durations and a transport sequence, and a live sink may drop events by design.
Replay re-executes the program; it never rearranges or replays trace events.
`debug_engine` also captures a throwing node's fan-in for inspection.

## Replay

Replay is an opt-in capability, not part of the engine's core proposition.
It consumes the engine's invariants — fresh-program construction, state-as-data,
boundary-anchored clocks, explicit crossings, and determinism — but adds no
evaluation semantics of its own.
Link `clockr::replay` explicitly; the umbrella `clockr` target does not include it.

### boundary journal

A boundary journal is an ordered log of effective boundary operations, complete
from a known initialization:
initialization boundaries, input sets (grouped into per-clock epochs), region
steps, and application marks.
The journal records what entered the program and when it ticked.
It does not record the node-evaluation trace.
The replay core treats application marks as opaque.

### scenario

A scenario is an immutable boundary journal with identity.
It carries a `run_id`, a `branch_id`, an optional parent run, and a cut entry
for a branch.
Entry ids are monotonic within a run and never reused.
A scenario also carries **fidelity**: whether the journal is complete, how many
entries are missing, and why.
An incomplete scenario may replay, but its result must never be labelled exact.

*Notes.*
The code form is `clockr::replay::scenario`; `scenario_builder` assigns entry
ids and epoch groups.
Sets in one `epoch_group` form an unordered input map (L9a); a region step
closes the group.
The lossy live trace cannot substitute for a journal.

### run

A run is one execution of a scenario.
It records one `step_record` per logical region step: the step identity, its
display epoch, whether it threw, and the encoded outputs captured after the
step.
It may also carry a synchronously captured trace and the scenario's fidelity.
A run is a semantic comparison artifact, not a trace replay.

### branch

A branch is a revision of a scenario with a parent and a cut entry.
`branch_edit` inserts, removes, or replaces suffix entries; `apply_branch`
derives a new scenario and never mutates the base.
`replayer::replay_branch` replays the unchanged prefix, applies the edit, and
continues the suffix.

### logical step

A logical step is `(region, ordinal)` within a run: the region's clock and how
many times it has stepped.
It is stable across a throwing tick, which publishes no epoch.
An epoch is display and resampling metadata, never step identity.

### replay

Replay constructs a fresh `kernel::program` through a host `program_factory`,
applies the journal's boundary operations in order, and calls the ordinary
`engine::step(program, clock)` path.
It does not implement a second evaluator.
Journal user values travel as host-registered codec bytes and are decoded into
the target program's region pools; the journal never retains a cell owned by a
live or dead program.
Comparison is semantic: by step, port/node/output, ignoring durations,
sequence numbers, addresses, and `user_cell*` identity.

*Notes.*
The code forms are `clockr::replay::session` (recorder), `replayer`,
`codec_registry` / `value_codec`, and `compare`.
`native::recipe_factory<Recipe>` is the owning-program adapter.
Prefix replay is the first checkpoint; direct state snapshots remain future work.

## Code map

This table shows where each concept lives in the source.
Every name below is the canonical term in code form.

| Concept | In the code |
|---|---|
| `clock` | `kernel::clock_tag` (the label); `kernel::region` (the node set) |
| `epoch / step` | `conc::epoch_id`; `kernel::engine::step`; the step's phases `drain` / `evaluate_nodes` / `settle` / `publish` |
| `stream` | no type of its own: `kernel::value` on edges; `std::optional<T>` marks an event port |
| `value` | `kernel::value`; `user_inline` / `user_ref`; `user_type` / `describe<T>`; `to_value` / `from_value` |
| `value_home` | `kernel::value_home`; `user_cell`; `program::reclaim` |
| `recipe` | `shell::recipe<A, B>`; `shell::node` (recipe nodes); `node_ref`; `shell::draft` |
| `descriptor` | `shell::descriptor_storage` (constant storage); `kernel::descriptor_view` (span view); `kernel::graph` (structural form); `kernel::topology` |
| `program` | `kernel::program`; `program::descriptor()` returns its view |
| `node / edge` | `kernel::node` (structural) / `kernel::node_descriptor` (descriptor form); `kernel::edge` |
| `crossing` | `shell::at<Clock, Policy>`; `kernel::crossing` |
| `policy` | `conc::policy` (`latest` / `queued`) |
| `link` | `conc::link<value, policy>`; `slot` / `spsc`; `dropped_sends` / `lapped_fills` |
| `transform` | `shell::lift` |
| `wiring` | `shell::share`, `shell::zip`, `shell::parallel`, `shell::operator>>` |
| `state` | `shell::delay` / `scan` / `stepper`; `kernel::delay_state`; `materialize_fn` |
| `boundary` | `shell::input` / `shell::output` |
| `engine` | `kernel::engine`, `kernel::release_engine`; `kernel::step_hook` / `engine::set_step_hook` (the before/after seam, not the `observer`) |
| `observer` | `kernel::debug_engine`; `trace_event` / `trace_sink` |
| `boundary journal` | `clockr::replay::scenario`; `scenario_entry`; `session` |
| `scenario` | `clockr::replay::scenario`, `scenario_builder`, `branch_edit`, `apply_branch` |
| `run` | `clockr::replay::run`, `step_record`, `replayer` |
| `branch` | `clockr::replay::branch_id`, `branch_edit`, `apply_branch`, `replayer::replay_branch` |
| `logical step` | `clockr::replay::region_step_id` (`clock_tag`, ordinal) |
| `fidelity` | `clockr::replay::fidelity` (`complete`, `missing_entries`, `reason`) |
| `replay` | `clockr::replay::session` / `replayer` / `codec_registry` / `compare`; `native::recipe_factory` |
| `tick function` | `node_descriptor::tick_fn`, invoked as `kernel::tick(node, ctx, inputs)` |

## Rejected vocabulary

These terms name a rejected design or could collide with a canonical term.
Do not use them for new work.

- **`Cell`, `Stream`, `Behavior`** — classic continuous-time FRP and Sodium live-value vocabulary.
  See `doc/explanation/frp-decisions.md` §F1/Shape B.
  `clockr` is clocked synchronous FRP; the equivalents are `stream` and `state`.
- **`network` as a fragment** — a part of a network is a *fragment* of a recipe.
  Reserve `network` for the whole system.
- **`resampler`, `channel`** — without qualification, these terms hide the intended level.
  Use `policy` or `crossing` for the static annotation.
  Use `link` for the runtime object.
- **"trace replay", "replay log" for the observer trace** — the trace is an
  observation stream and may be lossy; it carries measured durations and
  transport sequence. Replay re-executes a boundary journal. Keep the live
  recorder and the journal separate in code, names, and protocol.
- **"history replay" (debug-client)** — that connect-time feature resends
  traces to a new viewer. It does not re-execute a program; use `replay` only
  for fresh-program scenario execution.
