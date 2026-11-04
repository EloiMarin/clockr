# clockr::recipe

The `recipe` module (target `clockr::recipe`) is the **composition layer** of the
project: The user-facing API for describing reactive systems as *signal
networks* (see `doc/explanation/frp-decisions.md`, "Shape C"). It sits one level above
`graph`: where `graph` is the erased kernel ("nothing executes here"), `recipe`
is the typed shell that *emits* graphs. It depends on `graph` and nothing else;
the two meet only in application code, through a `kernel::graph`.

The pipeline mirrors the module structure:

```
recipe  (compose networks as values)  →  graph  (describe)  →  engine  (execute)
```

## Contents

### `recipe.hpp`: The typed shell

An `shell::recipe<A, B>` is an immutable **recipe** for a subgraph: a by-value arena
(a `std::array<node, N>` plus a root index) with the static types of its
input and output as parameters. Operators build bigger recipes from smaller ones:

- `lift<A, B>(f)`: A pure function `(const A&) -> B` becomes a one-node
  network. When `A` is a (nested) pair (as built by `zip`) `f` takes one
  argument per leaf, left to right, and the node gets one fan-in edge per leaf.
- `operator>>`: Composition; the middle type must match (compile time).
- `zip(a, b, …)`: N branches (N >= 2), one shared upstream; the output type
  nests pairs left-associatively (`zip(a, b, c)` : `pair<pair<A, B>, C>`),
  which the kernel realizes as one fan-in edge per leaf in zip order (L8).
- `input<T,Tag>(name)` / `output<T,Tag>(name)`: Named source/sink ports,
   each **anchored on a clock** (`Tag` is a `clock_tag` NTTP).
- `delay<T>(init)`: The unit delay, exposed standalone: outputs `init` on
  the first tick, then the previous tick's input (§3.1). Feedback loops must
  pass through `delay` (or `scan`), which is what keeps their cycles guarded (L7).
- `scan<In, State>(init, f)`: Stateful accumulator: a pure step function
  fed through a delay node (§3.1).
- `stepper<T>(init)`: The absorb pattern: `scan` with the step fixed to `value_or`,
  turning an `optional<T>` event crossing into a held `T` level.
- `share(network)`: fan-out. Instantiate once per (recipe, clock), memoized.
- `parallel(...)`: N independent chains, N outputs.

Threads are anchored at boundaries only: `input`/`output` name their clock,
interior nodes inherit the clock of the nearest upstream anchor, and there is
no ambient thread. `build(network)` derives the whole clock structure from
the ports (`at<>` resampling annotations will name crossings; C1).
`instantiate(context, graph, current, network)` walks the recipe once and
emits nodes/edges into a plain `kernel::graph`; `current` is the walker's
clock state (`std::nullopt` at the root).
After this the recipe can be discarded; evaluation is entirely the engine's job.

Value conversion between shell and kernel is **permissive in, strict out**:
`to_value` accepts any integral (→ `int64_t`) or floating-point (→ `double`)
type so natural literals work; `from_value<T>` is a strict `std::get<T>`.
Absence is spelled `std::optional<T>`: engaged converts its payload,
disengaged converts to the monostate "no event" marker
and reads back the same way, while genuine mismatches still throw.

### `ast.hpp`: The erased recipe AST

The AST is a tagged union (not a virtual hierarchy): `node` is a
`std::variant` of one alternative per operator (`lift_t`, `compose_t`,
`input_t`, `output_t`, `share_t`, `hold_t`, `delay_t`, `zip_t`, `parallel_t`),
and the single dispatch point is a `std::visit` walker (`first_anchor` / `instantiate`).
Nodes are plain value data with children referenced by `node_ref` indices into
the owning `recipe`'s flat by-value arena (no virtual calls, no pointers, no heap)
which is what lets a `recipe` be a compile-time constant.

Each operator's node type knows how to emit its fragment into a `draft`
(the descriptor-shaped accumulator): `instantiate` receives the walker's
current clock and the upstream node ids, adds nodes/edges to the draft, and
returns the ids of the fragment's output nodes. `first_anchor` is the
look-ahead that anchors interior nodes: the first input/output port in walk
order. When a fragment is reached with no clock established it seeds `current`
from that anchor; a fragment with no anchor anywhere is rejected. Composition
threads its first half's outputs into its second's inputs, passing the same
`current` so a chain stays on one clock; `parallel`/`zip` branches each work
on a copy of `current`, so branch-local crossings stay branch-local. The
output is a *span*, not a single id, because a fragment may have several
outputs: `zip` emits none of its own and returns one id per branch, in zip
order. The downstream multi-input node's fan-in order is exactly that span
(doc/reference/denotational-semantics.md, L8).

Because recipes are immutable by-value arenas, copying an `recipe` copies the
*description*, never runtime state. `share()` memoizes one instantiation per
(share id, clock) (L4) (the id is minted from `std::source_location`, so a
shared fragment reused in several places still dedups), `scan` seeds an engine
state slot via a delay node (§3.1), and `zip`/`lift` give multi-input
composition (L8). Ports are named and tagged with their clock.

## How it fits together

`recipe` is where doc/explanation/frp-decisions.md's "networks as values" lands: a widget's
animation network and a whole application's transport network are the same
kind of thing, composed rather than registered. Nothing in this module runs
on the real-time path: recipe building and `instantiate()` are startup work;
the emitted graph is what the engines evaluate per tick.

Tests live in `tests/recipe.test.cpp` (executable `sf_tests`), including parity
between an `recipe`-instantiated graph and its hand-built equivalent.
