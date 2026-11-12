# clockr

`clockr` is a C++20 library for audio applications. It supplies a real-time-safe, thread-aware functional engine.
The engine makes the functional graph aware of threads. It identifies the graph parts that belong to audio, UI, or worker threads.
Bounded-time, non-blocking primitives control the data flow between these parts.

The project is a pure library with the namespace `clockr`. Its executables are tests, host examples, and optional debugging tools.
The host examples are in `doc/examples/host` and `doc/examples/first_network`.
The newcomer walkthrough `doc/learn/your-first-network.md` explains `first_network`.
The optional debugging tools are in `tools/web-debugger`.
The independent packages `tools/debug-client`, `tools/debug-mcp`, and `tools/debug-dsh-plugin` support these tools.
Linux, macOS, and Windows are design targets. Mobile platforms are a future target.
The build uses clang with CMake presets and Ninja.
Refer to `CONTRIBUTING.md` for build and test instructions.
Refer to `doc/how-to/consuming.md` to integrate the library into another CMake project.
That project is the host that creates and runs a program.

## Module layout

`include/clockr/` contains the public headers for four modules.
These headers use paths such as `"clockr/kernel/..."`.
Each module has a CMake target: `clockr::mem`, `clockr::conc`, `clockr::kernel`, or `clockr::shell`.
The umbrella `clockr` target combines these targets.
The matching `.cpp` sources and tests are in `src/`:

- **`shell/`** — the user-facing composition layer. Refer to "Shape C" in
  `doc/explanation/frp-decisions.md`.
  A `shell::recipe<A, B>` is an immutable, by-value recipe for a subgraph.
  The operators are `lift`, `operator>>`, `input`, `output`, `share`, `parallel`, `zip`, `delay`, `scan`, and `stepper`.
  The `lift` operator is N-ary over pair wires.
  These operators combine smaller recipes into larger recipes.
  Composition checks the types. At runtime, the kernel stays erased and uses `kernel::value`.
  `kernel::program{recipe}` lowers the recipe and owns the result.
  `shell::compile` makes a constant descriptor of the same lowering.
  The same engine evaluates both forms.

- **`kernel/`** — the description of the reactive system and its execution plane.
  `kernel::graph` is the mutable builder. It contains nodes and directed edges.
  Each node has a `clock_tag` and a `node_kind`.
  The clock tags are audio, ui, and worker.
  The node kinds are input, output, internal, and delay.
  `kernel::topology` derives same-clock regions and cross-clock edges as crossings.
  It also derives a deterministic `evaluation_order`. The topology result is immutable.
  Topology rejects cycles unless they pass through a delay node. Delay nodes are Kahn seeds.
  `kernel::program` owns the run state.
  Node descriptors contain plain data. Their closures are cloned from the recipe or borrowed from a static descriptor.
  The program also owns named port tables, per-region runtimes, and links between regions.
  Each runtime contains values, delay state, and a scratch pool.
  User structs flow as `kernel::value`. Refer to `value.hpp` and `user_type.hpp`.
  The program type registry carries the debug schema for each described type.
  `kernel::engine` executes a program.
  `step(program, clock_tag)` evaluates one region on the calling thread.
  The region can be audio, ui, or worker.
  Each call evaluates the region exactly once in the precomputed topological order.
  `release_engine` and `debug_engine` have identical semantics.
  The `release_engine` is the lean evaluator.
  The `debug_engine` also emits a per-step trace as JSON lines through the `trace` facility.
  No factory or selection code is available yet. Callers instantiate a class directly.

- **`conc/`** — header-only communication primitives.
  `spsc` is a bounded FIFO based on
  [moodycamel::ReaderWriterQueue](https://github.com/cameron314/readerwriterqueue).
  The `double_buffer` and `slot` primitives provide single atomic slots.
  Seqlocks validate payloads that exceed the lock-free inline bound.
  `mpsc_stack` is an intrusive multi-producer/single-consumer retire stack.
  `link` is the project's named primitive for value exchange across clocks.
  A link operates at explicit synchronization points and uses the `latest` or `queued` policy.

- **`mem/`** — allocation for real-time paths.
  `arena` owns a large, aligned slab of memory.
  `pool` implements O(1) TLSF allocation inside that slab. TLSF means two-level segregated fit.
  `pool_allocator` adapts the pool to the standard allocator interface.
  STL containers can so use the same memory.

## How the pieces fit together

Refer to `doc/learn/layering.md` for a mental model of these layers.
It explains the location of each layer and the interface between the typed shell and the erased kernel.
A typical flow follows:

1. Compose a `shell::recipe<A, B>` network.
   The `input` and `output` operators anchor the clocks.
   Other operators build the signal processing between these anchors.
   A recipe is an ordinary value. You can build it anywhere, including during constant evaluation.
2. Construct a `kernel::program` from the recipe.
   Construction lowers the recipe to node descriptors and edges.
   It also derives the topology and per-region runtimes, and sizes the memory pools.
   A borrowed program uses a `shell::compile` descriptor.
   This form skips the lowering and points to the constant data.
   You can destroy the recipe after the program exists.
3. Step each region from its own thread with `engine.step(program, clock_tag)`.
   The call drains the inbound links of the region at one producer cut.
   It evaluates the nodes in order, settles delay state and output ports, and publishes one epoch.
   Cross-clock edges use `conc::link` at the crossings found by topology.
   Each link uses the `latest` or `queued` policy.

During steady-state audio processing, the audio thread performs no heap allocation.
The tick scratch buffers and user-value cells come from the region's `mem::pool`.
The host chooses the pool size. Refer to **Sizing the memory pools** below.
The `mem` and `conc` modules keep the critical audio path independent of the system allocator and blocking synchronization.

## Debugger and MCP

`tools/web-debugger` builds a native debug server and a small `debugger_demo` host.
`CLOCKR_BUILD_DEBUGGER` controls this build and is ON by default.
A browser displays the live network and its per-tick evaluation trace.
The display includes a timeline scrubber and the values produced by each node.
The TypeScript tools are separate packages.
`tools/debug-client` speaks the WebSocket protocol.
`tools/debug-mcp` exposes the protocol to agents over MCP.
`tools/debug-dsh-plugin` is a preview Cordis adapter.
An agent, rather than a human, can attach to a running host.
The agent can observe the trace, set an input at a tick boundary, and read the effect.
Refer to [Debugging a running app with an agent](doc/how-to/agent-live-debug.md).

## Website

`tools/website/` builds the landing page and publishes the design docs under `/docs`.
It also stages an in-browser DAWmulator and debugger pair.
The demo page defaults to the in-browser pair and switches to a live host when the
build is given one with `DEBUGGER_URL` and `DAW_URL`.
Refer to [`tools/website/README.md`](tools/website/README.md).
`site.config.json` contains the public name and copy, and the source-forge URLs.
The website build is forge-neutral. The GitHub workflow is only a thin driver.
Hosting and deployment configuration are intentionally kept out of this repository.
`tools/website/lab/` contains the throwaway wasm animation experiments.
The lab is not part of the published site.

## Sizing the memory pools

User structs travel in reference-counted cells. Each region's `mem::pool` allocates these cells.
A pool is a fixed-size arena.
The library does not grow the pool, use the system allocator for a cell, or block.
If the pool is exhausted, allocation throws `std::bad_alloc` on the ticking thread.
This explicit failure on a real-time path prevents corruption and use-after-free.
The host must size the pools when it hosts the library.
Set `program_options::region_pool_bytes` to the selected size. All regions get the same size.
The 64 KiB default is sufficient for demos, but not for a real instrument.

Budget this amount for each region:

```
pool_bytes = 2 x ( scratch
                 + residential
                 + grace window
                 + queued backlog
                 + debug reserve )
```

- **`scratch`** — the per-tick evaluation buffers come from the same pool.
  Budget `sizeof(kernel::value) * node_count` for the tick value array.
  This is 32 bytes per node.
  Add one fan-in vector per node, with its arity times 32 bytes.
  Typical patches need a few hundred bytes.

- **`residential`** — cells held for a tick or longer:
  - one cell for each user-typed input and output port;
  - one cell for each `delay` or `scan` state, with size `sizeof(user_cell)` + alignment + `sizeof(T)`;
  - one cell for each user-typed `delay` or `scan` seed, materialized at program construction;
  - up to `link<latest>::history` (8) cells for each `latest` channel sourced in the region;
  - each ring slot owns its value until that slot is reused;
  - one cell for each user-typed output from the previous tick.

- **`scan` churn** — a stateful `scan` node can allocate on every tick.
  This occurs when its state is larger than the 16-byte inline budget.
  The node allocates a new pool cell for the new state and retires the previous cell.
  A fast region so allocates, retires, and reclaims each tick, in addition to the state's residency.
  Keep the state small enough to store inline, or tick the owning region less frequently.
  This cost comes from value-semantic state. No in-place mutation path is available.

- **`grace window`** — releasing a cell does not immediately free it.
  The owning region puts the cell on its retire queue.
  The library frees the cell only after every region leaves the read window where it could observe the cell.
  Refer to the EBR rules in `doc/reference/denotational-semantics.md` §3.2.
  If the scheduler preempts a consumer tick, its window retains every cell retired since the tick began.
  Budget one consumer tick's worth of this region's cell production:
  `cells produced per tick x (longest consumer tick / this tick period)`.
  Budget at least a few cells.

- **`queued backlog`** — a `queued` crossing owns one reference for each backlogged value.
  A queue at its capacity of 1024 entries holds that many cells.
  For each queued channel sourced in the region, budget
  `1024 x (sizeof(user_cell) + alignment + sizeof(T))`.
  The application must meet the capacity precondition in law L6 of `doc/reference/denotational-semantics.md`.
  `program::dropped_sends()` counts violations, but the library takes no action on them.

- **`debug reserve`** — the trace recorder retains traced structs while the debug UI is attached.
  It retains them until the server encodes them.
  In steady state, budget for the user-typed trace events in one drain window of approximately 33 ms.
  The recorder queue has a hard bound of 16k events per thread.
  If the pool is tight, budget a fraction of this capacity or lower the capacity.

Each live cell uses `sizeof(user_cell)`, which is 40 bytes of header.
It also uses alignment padding and the allocator's per-block overhead.
Budget approximately `sizeof(T) + 80 B` for each cell, and confirm the result with `pool::snapshot()`.
`kernel::value` stores trivially copyable structs of 16 bytes or less inline.
These structs never use a pool. Use small PODs at boundaries.

Procedure:

1. List every user-typed port, `delay` or `scan` state, and crossing for each region.
   The debugger's graph view shows these items.
2. Sum `residential`.
   Add one grace window and the queued backlog.
   If debugging, add the debug reserve.
3. Double the result.
   Set `program_options::region_pool_bytes`.
   Run the worst-case workload.
   Use the largest structs, the fastest rates, and a paused or slow consumer in this workload.
4. Watch `program.region(tag).home->pool.snapshot()`.
   Compare `used_bytes` with `capacity()` after the worst case.
   If `used_bytes` settles back to a steady value, reclamation keeps up.
   A slowly growing `used_bytes` means that a region's reclamation is starved.
   Refer to the grace window above.
   `overhead_bytes` is the allocator's bookkeeping.
5. If a region stops ticking, call `program::reclaim(tag)` from its maintenance path.
   Ticking regions reclaim at most `program_options::reclaim_batch` (64) cells per tick on their own thread.

Worked example — the debugger demo:
One `latest` crossing carries a 24-byte struct from ui to audio. The network has seven nodes.
UI residential use is two cells: one for the port and one for a ring slot.
The audio side has no user-typed cells. Add one grace-window cell and approximately 224 bytes of scratch.
With 2x headroom, the total is a few kilobytes. The 64 KiB default is so generous.
Suppose the audio region republishes a 256-byte struct on every tick, and the UI consumer stalls for seconds.
The required size then changes by orders of magnitude. The host must therefore choose this number.

## Build and test

```bash
direnv allow                    # nix shell with clang, cmake, ninja
./build.sh                      # configure + build + test (host platform, release)
./build.sh --type debug
./build.sh --platform wasm --type debug
./ci/ci.sh linux_x64_debug      # the CI entry point, same presets locally
ctest --test-dir _build/linux_x64_debug -R "kernel/"   # by test name
```

Tests use Catch2 v3. CTest registers each test case individually.
Select tests by name with `-R "kernel/"`, not by executable.
The test executables are `mem_tests`, `conc_tests`, `kernel_tests`, `shell_tests`, `integration_tests`, and `debugger_tests`.
The last executable is available when the debugger is enabled.
`CONTRIBUTING.md` covers sanitizers, WebAssembly, the debugger, and the debugging setup.
`doc/how-to/ci.md` covers CI and the Codeberg migration.

## Consuming

Another CMake project can use clockr as the host that creates and runs a program.
Use `FetchContent` on the source, or install the library and call `find_package(clockr)`.
Both modes provide the `clockr::clockr` umbrella target.
Refer to `doc/how-to/consuming.md` for both modes, the installed layout, and the build options.
`doc/examples/host/main.cpp` is the smallest working host.

## Roadmap

The project includes the core engine and composition layer.
This section replaces the former `roadmap/` directory.
It records landed work, open work, and known refinements that do not block progress.
Each landed phase has acceptance specs in the source tree.

**Landed.**
`mem` includes the arena, TLSF pool, and `pool_allocator`.
`conc` includes `double_buffer`, `slot`, `spsc`, and `link` under the `latest` and `queued` policies.
The structural graph includes topology, deterministic evaluation order, and delay-guarded cycles.
`release_engine` and `debug_engine` include trace and parity.
The shell operators are `lift`, `>>`, `input`, `output`, `share`, `parallel`, `zip`, `delay`, `scan`, and `stepper`.
Boundary-anchored clocks and explicit resampling annotations are complete.
The single `kernel::program` plane supports an owning recipe and a borrowing descriptor.
Named ports and user value types are complete.
User value types include inline PODs or pool cells, with debug schemas.
`engine::step` is allocation-free. The live debugger and its write-back path are complete.
`clockr::replay` is an opt-in module: immutable scenarios, boundary journaling,
host-registered value codecs, fresh-program replay, branch edits, and semantic
run comparison. The DAW and the jam both replay recorded sessions through the
ordinary engine.
`demo/jam` exercises that substrate: a bounded score/patch model, a fixed
editable network, a session/action log, deterministic metrics, a native
conductor core, and a wasm shell/page.

**Open.**

- **C3 evaluation-error policy.** Allocation-free evaluation is complete, but a failing tick still throws.
  The candidates are sticky flags with downstream substitution, debug-throws/release-records, or a fallible tick result.
- **C4 common-clock sync.** This feature is on demand only.
  Regions tick their own epochs, and absolute time stays a host concern.
  Latency offsets can travel on `latest` ports.
- **E1 determinism/replay check.** A replay-based check exists now:
  replay the same scenario twice and `clockr::replay::compare` the runs.
  Wiring this as a `debug_engine` convenience option remains open.
- **E2 graph export.** This feature is a DOT or diagram writer over graph, regions, and sharing points.
  Node labels already travel through the descriptor.
- **E3 time-travel replay.** Prefix replay and immutable branches have landed in
  `clockr::replay`. Direct checkpoints that restore state slots for long
  histories remain open; a checkpoint is an optimization, never new semantics.
- **F1 bounded alternation.** This feature is a multiplexer over pre-instantiated branches.
- **F2 structural rebuild.** Rebuild away from the audio thread and use a double-buffered topology swap.
  A stress test covers this feature instead of a unit spec.
- **F3 runtime program plane.** This feature is deferred.
  It waits until a real application needs mutation that the F2 swap cannot express.
- **H authoring sugar.** This feature includes named bundles over `zip` and `lift`, flat leaf spelling, NTTP deduction, and pipeline helpers.
  It is a surface-only layer and never a new kernel construct.
- **I undo/snapshots.** Undo as "replay to an entry" is available through
  `clockr::replay`. Direct snapshots that retain and restore plain tick state at
  a tick boundary remain open and share E3's checkpoint path.

**Known refinements.**
A `queued` drain currently consumes one event per consumer step instead of a batch.
The raw queued primitive does not coalesce same-epoch repeats.
The engine coalesces them through input overwrite and one send per tick.
An empty `parallel()` returns an empty output span.
The `recipe<void, void>` terminal marker for `parallel` breaks the `std::monostate` convention.
`topology::evaluation_order` is O(N·E) at construction time.
The `kernel::program` recipe constructor still includes shell headers. This condition remains until F3.

## Documentation

The `doc/` directory contains design and reference material.
`doc/README.md` provides a reading map organized by genre:

- `GLOSSARY.md` — canonical vocabulary and the alias index.
- `doc/learn/` — `layering.md`, the shell-to-kernel mental model.
- `doc/explanation/` — `frp-decisions.md` explains this FRP flavor and API shape.
  `operator-sufficiency.md` explains the coverage of the operator set.
- `doc/reference/denotational-semantics.md` — meanings, laws L1–L9, and acceptance criteria.
- `doc/reference/further-reading.md` — the linked bibliography behind the design.
- `doc/how-to/` — `consuming.md` covers `FetchContent` or installation with `find_package`.
  `ci.md` covers CI jobs and the Codeberg migration.
- `doc/examples/` — the small host programs `host` and `first_network`.
- `doc/archive/` — delivered talks and historical artifacts, including the
  [ADC 2026 talk](doc/archive/adc-2026/index.md) and its
  [slides](doc/archive/adc-2026/slides.pdf).
- `CONTRIBUTING.md` — build, test, style, and commit conventions.
