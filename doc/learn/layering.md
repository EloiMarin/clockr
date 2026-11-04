# clockr's layers: a mental model

*How to think about the codebase so you can read it and extend it without
getting lost.*

This file is the *orientation* companion to the design docs:
`denotational-semantics.md` says **what the engine means**; `frp-decisions.md`
says **why the design is what it is**; this file says **how the modules map
onto ideas** — the bridge between the maths and the C++.

**Audience**: developers who know C++ but are new to FRP and to type
erasure. The tone is deliberately informal; the precise statements live in
the other two docs. This repository doubles as an introduction to FRP, and
the goal of this file is to make the first reading of the code pleasant.

## 1. The pipeline at a glance

A network lives three phases, each with its own home:

| phase | happens | module | data lives in |
|---|---|---|---|
| **compose** | build a *recipe* | `shell` (types + node arena) | none |
| **build** | emit the recipe as a description | `shell` → `kernel` | `draft`/`build_context` (mutable scratch) |
| **step** | run the program, once per epoch | `kernel` (`engine`) | `region_runtime` (values, delay slots) |

Everything confusing about this codebase sits on the boundary between
compose and build — that is where *types* meet *erased kernel artifacts*,
and it is the only boundary that C++'s type system actively fights you on.

## 2. Two vocabularies, one translator

The `shell` module translates between two vocabularies:

- the **typed** world: `int64_t`, `double`, `std::pair<A, B>`, and your C++
  lambdas;
- the **erased** world: node ids, `kernel::value` variants, tick function
  pointers.

The translators are the *operator functions* in `recipe.hpp` (`lift`, `zip`,
`scan`, …). If you are confused about something in `shell`, you are at
the seam of this translation — step back and ask which side you are on.

## 3. `shell::recipe<A, B>` is an arrow, not a value

Do not read `recipe<A, B>` as "a value of type B flowing out of an A". Read it
as an **arrow** from A to B: a *description of a subgraph*, like a function
object. Nothing runs when you build it.

- `>>` is arrow composition (`recipe<A, B> >> recipe<B, C>` = `recipe<A, C>`);
- `lift f` is "arr" — lift a pure function into an arrow;
- `zip` is the *product* (pairing / fan-in);
- `share` is fan-out;
- `parallel` is the disjoint sum;
- `input` / `output` are boundary ports.

This is exactly Haskell's Arrow calculus; `frp-decisions.md` spells the
vocabulary (`lift`, `zip`, `scan`). Arrows are **composed**, never
executed — composition is a construction-time activity.

## 4. A recipe tree plus a compile-time contract

A `recipe<A, B>` is two things taped together:

- a by-value **recipe arena** — the **structure**: a flat
  `std::array<shell::node, N>` (plus a root index) describing what to build
  when instantiated. No pointers, no heap: `shell::node` is a `std::variant`
  of the operator node types, and children are referenced by arena *index*.
  This is what lets a recipe be a compile-time constant;
- two type parameters A and B — the **contract**: what must enter and leave
  the subgraph.

When you write `zip(a, b) >> lift(f)` (a lift over the pair-typed wire), the
compiler checks the contract (the middle type must match: `pair<A, B>` on both
sides) and builds a bigger arena. **No graph exists yet.** Types are checked
at composition time — this is the one place the shell earns its keep over an
untyped kernel.

## 5. Three phases, three homes for data

- **Recipes** are immutable, by-value arenas: copying one copies the
  *description* — including, for large networks, a deep copy of the arena
  (composition is O(n²) in arena copies). That is the price of the recipe
  being a constant; it is paid once at composition, never on the real-time
  path.
- **`draft` / `build_context`** is the only mutable state during the build
  walk: the port tables, the `share` memoization, the delay ids, the crossing
  map. It is *scratch*, scoped to one build.
- **`kernel::graph`** (via `shell::build`) and **`descriptor_storage` /
  `descriptor_view`** (via `shell::compile`) are the emitted artifacts: pure
  descriptions, cheap to copy, safe to hold `const&`.
- **`kernel::engine`** consumes a descriptor and never knows `shell` exists.

If you know which phase you are in, you know where data may live.

## 6. Type erasure, concretely — two erasures

- **Value erasure.** Every typed signal becomes a `kernel::value`: the scalars
  (`monostate` / `bool` / `int64` / `double`) plus *user structs* — small
  trivially-copyable ones stored inline, larger or owning ones as a
  reference-counted pointer to a pool cell. `to_value` and `from_value` are
  the marshalling shims: permissive in, strict out (a user struct's runtime
  tag is checked on the strict read). The kernel stays trivially copyable and
  size-bounded because a user value is a handle, never a smart pointer; where
  its lifetime is owned and released is an engine concern (see the ownership
  notes in `denotational-semantics.md`).
- **Interface erasure.** `recipe<A, B>` is a template, but `kernel::graph`
  and `descriptor_view` are not. The `shell::node` variant, visited via
  `std::visit`, hides the template: the node walked across the boundary knows
  nothing about A and B. The templates die at the edge of the operator
  function bodies.

## 7. The bridge moment — and the phantom pair

`lift` and `scan` are where a *typed* lambda becomes an *erased* tick
function:

```cpp
return to_value(fn(from_value<A>(in[0]), from_value<B>(in[1])));
```

The operator bakes the type knowledge into the tick function at **creation**
time. That is why the kernel can be type-blind: safety is enforced at
composition, never at evaluation.

The `std::pair` in `zip` / `lift` is a **phantom type**: it is never
constructed, never stored, never marshalled. Its only jobs are to make `>>`
type-check and to encode *arity* — "a pair" means "two fan-in edges, in zip
order" at the erased level. That is the zip design in one sentence: the shell
says `pair<A, B>`, the kernel sees a node with two fan-in edges, and
`kernel::value` never carries a pair (law L8).

## 8. The dissection tool

To understand any operator, answer three questions:

1. What `recipe<A, B>` does it produce or consume? (the **contract**)
2. What `shell::node` does it construct? (the **structure**)
3. What does that node emit into the graph, and what does its tick function
   compute? (the **behaviour**)

If you can answer all three, you own the operator. When you are lost, trace
`instantiate`: the types decide *whether* two fragments may be wired
together; the recipe decides *how* the descriptor looks.

## 9. Where to go next

- `denotational-semantics.md` — the precise meanings and the laws (L1–L9);
  §1 is a self-contained ten-minute introduction to the method.
- `frp-decisions.md` — why the flavour (clocked synchronous FRP) and the API
  shape were chosen.
- the roadmap section of the root `README.md` — what is built and what comes
  next.
- `src/shell/README.md` — the module's own guide.
