# Your first network

Every snippet in this walkthrough is quoted from a real file:
`doc/examples/first_network/main.cpp`. Build it, run it, change it — the whole
example fits in one screenful and prints two lines. Build it the way the
other examples are built (see `doc/how-to/consuming.md` for out-of-tree):

```
cmake --preset linux_x64_debug
cmake --build --preset linux_x64_debug --target clockr_first_network
./_build/linux_x64_debug/doc/examples/clockr_first_network
```

## The idea in one paragraph

A clockr program is a graph of operators composed at compile time — a
*recipe*. The recipe does not run; it is a description of a subgraph, like a
function object. A host lowers the recipe into a `kernel::program` (ports,
regions, delay slots — all plain data), sets inputs on named ports, and calls
`step` on an engine. Each step evaluates every node exactly once, in a
deterministic order, and publishes the results on the output ports. Programming
it means writing down *what the graph computes*, not a loop that drives it.

## The recipe

The network reads an alarm level, trips on the value it sees, and doubles the
trip afterwards. Stateful, deliberately, so the second step has something new
to show:

```cpp
    const auto network {
        shell::input<double, kernel::clock_tag::ui>("alarm")
        >> shell::scan<double, double>(
            0.0, // the state's initial value: what the first tick sees
            [](const double& state, const double& value)
            {
                return state == 0.0 ? value : value * 2.0;
            })
        >> shell::output<double, kernel::clock_tag::ui>("tripped")
    };
```

`>>` composes fragments left to right, like a pipeline, and each composition
checks that the wire types line up — a mismatched wire is a compile-time
error, rejected before the program ever builds.

`input<double, kernel::clock_tag::ui>("alarm")` declares a boundary port. The
template is the wire type and the clock: an input anchors the clock of
everything downstream of it, until a crossing says otherwise. The string is
the port's name, and names are how the host exchanges values with the
program.

`scan(init, f)` is the stateful operator: it folds its own state across
ticks. On tick 0 the state is the seed `init`; on every later tick it is the
previous output: `state(t) = f(state(t−1), in(t))`, i.e. the new state
settles after the tick and is consumed on the next one.

The lambda receives `(previous state, current input)` and returns the next
state. This example's fold keeps a level latched: the state starts at zero,
trips with the first value, then doubles it.

## Lowering the recipe

```cpp
    kernel::program program { network };
```

Lowering turns a description into executable data: each operator becomes
plain object state — nodes, edges, delay slots; type tags for user-defined
POD types — all data, no closures on the ticking thread. This is what makes
the state *inspectable*: program state is data, the same as the delays it
owns. Setting the input and reading the output both go through named ports:

```cpp
    // A network is inert: nothing runs until the host steps it.
    program.set("alarm", kernel::value { 1.0 });
```

`set` writes a `kernel::value` onto the named input port. That port persists
across ticks until the host changes it (that is what a level-typed
**input** denotes). The set itself does not evaluate anything — a network
is inert.

## Stepping

```cpp
    kernel::release_engine engine;
    engine.step(program, kernel::clock_tag::ui);
```

One `step` drives one clock's region: it fills every inbound link, evaluates
the region's nodes once, settles the delay state, and publishes outputs. The
network above crossed no region boundary, so one clock tags the whole
program and one step runs everything in one pass. Crossings between clocks
(`at`) and resampling policies are the next layer of the model; for now,
accept that `engine.step(program, clock)` is the *only* call the host makes
to move time forward.

## Reading back

```cpp
    const double first { std::get<double>(program.get("tripped")) };
    std::cout << "tick 1 read 1.0 -> sees the seed, trips with " << first << '\n';
    if (first != 1.0)
        return 1;
```

Expected output, then the fold on the second step:

```text
tick 1 read 1.0 -> sees the seed, trips with 1
tick 2 read 1.0 -> folds state, trips with 2
```

The two lines show the fold. Tick 0 is the seed tick: `scan`'s state exits
as the value it saw, unchanged. Tick 1 is the fold tick: its state is the
previous output, so the doubling branch of the lambda fires. That is the
whole behavioural contract of `scan` — state as data, exactly one equation.

## Change it and predict

Rebuild with a folding lambda (`state + value`), or a different seed, and
write down the two ticks' outputs before running. Only the lambda changes —
the state's behaviour lives in the graph, not in a host loop.

## Where to go next

- `GLOSSARY.md` — the canonical vocabulary, including `scan`, `stepper`
  (event to level), `at` (crossings), and `clock` — keep it beside you.
- `doc/learn/semantics.md` — the walkthrough of reading an operator's
  equation is the next skill to pick up.
- `doc/learn/layering.md` — the mental model for the typed-shell ↔
  erased-kernel split this walkthrough has kept opaque.
- `doc/how-to/consuming.md` — building out-of-tree with the umbrella target
  or consumed as an installed CMake package.
- `doc/reference/denotational-semantics.md` — the full formal account
  (laws L1–L9), when the intuition needs a check.
- the roadmap section of the root `README.md` — what works today and what is
  still a plan (by phase).
