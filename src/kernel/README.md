# clockr::kernel

The `kernel` module (target `clockr::kernel`) is the **erased execution
machinery** of the project: the types that describe a reactive system (nodes,
values, connections, the structural analysis) and the engines that execute
it. It is the lower half of the shell/kernel split — it knows nothing about
C++ user types beyond the erased `value`; the shell composes recipes and
lowers them into it. `mem` and `conc` supply its allocation and cross-thread
primitives.

## Graph types

`graph` contains the types that **describe** a reactive system: the nodes,
the values flowing between them, the connections between nodes, and the
structural analysis of that network. It is purely declarative — nothing
executes here. The engine consumes these types.

### `value.hpp`

`value` is the single data type that flows along edges:

```cpp
using value = std::variant<std::monostate, bool, std::int64_t, double,
                           user_inline, user_ref>;
```

`std::monostate` represents "no value" (e.g. an event/trigger with no
payload). The scalars are the fast path; user structs are either `user_inline`
(a small trivially-copyable struct stored by value) or `user_ref` (a
reference-counted pointer to a pool cell, for anything larger or
non-trivially-copyable). The variant must stay trivially copyable and 32
bytes — the `conc` primitives exchange values lock-free only when they are
small and trivially copyable — so a user value is a raw handle and its
lifetime is managed explicitly by the engine, never by the variant. See
`user_type.hpp` (`describe<T>`, the debug schema) and `user_cell.hpp`.

### `node.hpp`

`node` is an immutable descriptor of one computation:

- `node_id` — a stable `std::uint64_t`, equal to the node's index in the
  owning graph.
- `clock_tag` — which clock owns the node: `audio`, `ui`, or `worker`.
  This is the "thread-awareness" at the heart of the project's design: the
  engine is expected to schedule and allocate per-clock state based on it.
- `node_kind` — the node's role: `input` (receives values from the engine's
  input span), `output` (delivers values to the engine's output span), or
  `internal` (computes from its fan-in).
- `tick_fn` — the node's pure-ish computation: given the values of its
  fan-in, produce its own value. The dev `node` stores it as
  `std::function<value(const tick_context&, std::span<const value>)>`; the
  compiled plane uses a plain function pointer with the same shape. The
  context carries the evaluating region's value home, so a tick that builds a
  user value allocates from the right pool (scalar ticks ignore it).

### `graph.hpp`

`graph` is the mutable builder for the network: `add_node()` appends a node
and returns its `node_id`; `connect(from, to)` records an `edge` and updates
the fan-in adjacency list (`fan_in(id)`) used by the engines to gather a
node's inputs. Nodes and edges are exposed as stable spans; node ids are
simply indices into the internal vector, so graphs are cheap to build and
trivial to copy.

### `topology.hpp`

`topology` is a derived, immutable analysis of a graph, computed once in the
constructor. It is header-only and `constexpr`: the same code computes the
order and the regions during constant evaluation over plain-data node/edge
arrays (no `std::function` anywhere), which is what makes a network's
structure a compile-time fact — see `include/clockr/kernel/topology.hpp`. It
produces three views:

- **`regions`** — connected components of nodes that share a `clock_tag`,
  computed with union-find over same-thread edges. Each `region` is the unit
  of per-clock execution.
- **`crossings`** — the edges that cross clock boundaries. These are
  exactly where cross-clock communication (a `conc::link`) must be
  inserted, and where real-time guarantees are at stake.
- **`evaluation_order`** — a topological order computed with Kahn's algorithm,
  breaking ties towards the lowest ready node id, so the order is
  deterministic for a given graph. Construction throws `std::runtime_error`
  if the graph contains a delay-free cycle; at compile time such a cycle
  simply fails constant evaluation.

## Engine

The engine is the interpreter of the project: it ticks one `program` region
at a time — fill inbound links at one snapshotted producer epoch,
run the region's nodes in topological order, settle outputs/delay state,
stamp one epoch and publish. Allocation and cross-thread communication live
in `mem` and `conc` so the engine stays the evaluation core.

### `engine.hpp`

The abstract interface, deliberately minimal:

```cpp
class engine
{
public:
    virtual ~engine() = default;
    void step(program&, clock_tag);

protected:
    virtual void on_region_step_begin(clock_tag, conc::epoch_id);
    virtual void on_region_step_end(clock_tag, conc::epoch_id, bool threw);
    virtual void on_node_tick_begin(node_id);
    virtual void on_node_tick_end(node_id, value);
    virtual void on_node_tick_throw(node_id, std::span<const value>);
};
```

Ports are addressed by name (`program::set`/`get`); regions tick on their own
clocks and meet only at `conc::link` crossings. The hooks are no-ops
by default, so release evaluation pays almost nothing for them; `debug_engine`
overrides them. The region hooks bracket one tick of one region and carry the
region's `published_epoch`, so `(clock_tag, epoch)` names a tick. There is no
factory or engine-selection code yet — callers pick an implementation class
directly.

### `release_engine.hpp` / `release_engine.cpp`

The lean evaluator. It walks the region's slice of the topological order,
feeding each internal node the values of its fan-in, then settles output and
delay slots. This is the baseline every other engine must match —
correctness of `debug_engine` is tested as *parity* with this one. Known
non-RT residue lives here: per-tick value/fan-in vectors and throwing tick
functions (see C3).

### `debug_engine.hpp` / `debug_engine.cpp`

A drop-in replacement with identical evaluation semantics, plus observability:
every region tick emits a `region_step_begin` before the first node and a
`region_step_end` after the last (with `threw = true` when a node unwound,
and the whole tick's measured time in `duration_ns`),
and every node evaluation travels as a single message into a `trace_sink` —
`node_tick_end` with the produced value, or `node_tick_throw` — together with
the measured evaluation time in nanoseconds. The `on_node_tick_begin` hook
only arms that timer, so the measured interval covers the `tick_fn` call
itself and none of the sink overhead. (`node_tick_begin` stays in the enum
for the hook, but producers no longer emit it.) A throwing node is still
recoverable through `failure()` before the exception propagates. Each event
carries a monotonically increasing sequence number; one `debug_engine` traces
one ticking thread. This is the foundation for the project's debugging
ambitions (time-travel debugging of a running audio app), and it is how a
developer can *see* the engine's steps during development.

### `trace.hpp` / `trace.cpp`

The tracing facility:

- `trace_event` — the structured event (kind, thread tag, epoch, node id,
  sequence, payload, and measured duration in nanoseconds — set on
  `node_tick_end`, `node_tick_throw`, and `region_step_end`). Kinds are `region_step_begin`,
  `region_step_end`, `node_tick_begin`, `node_tick_end`, `node_tick_throw`.
  The struct is trivially copyable so it can cross a lock-free queue to a UI.
- `trace_sink` — the abstract destination; engines only know this interface.
- `text_trace_sink` — the provided implementation, appending one JSON line
  per event to a `std::string`, e.g.
  `{"kind":"node_tick_end","clock":"ui","epoch":1,"node":1,"seq":2,"value":true,"duration_ns":320}`.
- `is_present()` / `value_text()` / `thread_text()` — helpers for rendering
  values (inlined when they are small scalars) and thread tags.

## How it fits together

The engine completes the pipeline started in the graph types + `conc`: an
application builds a program (regions, ports, links precomputed), the host
sets a region's inputs, and each cycle ticks that region. `release_engine` is
the production path; `debug_engine` is the same path with a full trace, so
development can inspect exactly what ran, in what order, with which values.
Cross-clock crossings are `conc::link`s in the annotated mode:
`latest` holds the newest published epoch, `queued` releases one event per
consumer tick in epoch order (a scalarized batch — see semantics §3.2).

Tests live in `tests/` (executable `kernel_tests`), including parity between
the engines, the multi-clock order/tear specs, and value ownership.
