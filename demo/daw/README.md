# DAWmulator — a complex client against the closed FRP core

*Proving the generic library can express an arbitrary real-world program,
without letting the example leak into the library.*

**Status:** P1 (audio-only, §7) implemented here and green; P2/P3 are future.
The real code is `daw/transport.hpp` (pure step functions) and
`daw/network.hpp` (`recipe` wiring), both covered by `tests/`.

**Audience:** a C++ developer who has never seen FRP. No prior FRP
vocabulary is assumed; every piece of jargon is defined where it first
appears.

**The one separation that matters.** The library (`src/`, installed as `clockr`) is the
**closed generic core**: no DAW-named types, no song/clip/MIDI vocabulary in
the kernel. This demo is an **external-style client** that uses only public
headers (`recipe/*`, `kernel/*`, `kernel/*`). A core change is justified only if a
*class* of clients needs it; a DAW-shaped need that only the DAW has belongs
here. Filter examples, so future readers can re-apply it: `loop_region` as a
kernel variant → *rejected* (the demo carries a LoopRegion *user struct*
instead, §5 — the kernel never names it); voice-steal policy → a demo-local
pure function, never a operator.

Companion docs: `doc/reference/denotational-semantics.md` (what the engine means, laws
L1–L9), `doc/explanation/frp-decisions.md` (why clocked synchronous FRP + Shape C),
`doc/learn/layering.md` (typed shell ↔ erased kernel ↔ engine), `roadmap/README.md` (phases
A–F).

---

## 1. What the demo is (and is not)

**Is:** a proof that a typically complex real-world program — transport with
play/pause/seek/loops/tempo, sample-accurate MIDI triggers, voice allocation
— can live *almost entirely* inside the FRP engine, leaving the outer
imperative code (UI callbacks, audio callback, sample copying) as dumb as
possible: `set()` ports, `tick()` a region, `get()` outputs.

**Is not:** a product DAW, an audio renderer, a plugin, or a UI toolkit.
Actual sample copying/interpolation, file parsing, decoding, and widgets stay
outside the engine — deliberately: FRP decides *what/when* at block rate; the
dumb player moves samples.

**Threads (clocks).** Three in the full plan: `ui` (message loop — owns
*intent*), `audio` (real-time callback — owns *time*), `worker` (background —
owns *bulk decoding*). "Clock" never means "wall-time-periodic timer": a
clock is a strictly increasing map `ℕ → Time`, and inside the semantics only
the tick index matters. A signal on clock `c` is `Stream_c(A) = ℕ → A` (one
value per tick); an event is `Event_c(A) = ℕ → Maybe A` (per tick: an
occurrence or silence). A tick is an **epoch**, the atomic unit of
observation: an epoch's inputs form a *map* (intra-epoch set order is
unobservable, L9a — two seeks in one epoch keep the last); only inter-epoch
order is observable. P1 exercises `ui` and `audio`; `worker` arrives with P2.

**Region diagram (P1 — the whole demo on one page):**

```
ui region                          audio region
[playing] ──latest──▶ [playing_rx] ─┐
[loop] ──latest──▶ [loop_rx] ───────┤
[bpm] ──latest──▶ [tempo] ──────────┤
[master] ──latest──▶ [gain] ────────┤
[seek] ──queued──▶ [seek_rx] ───────┤
                               [block] ┴─▶ [transport scan] ─┬─▶ [note sched] ─▶ on/off counts + offset out
                                                              ├─▶ [voice gate] ─▶ gain out
                                                              └─▶ [playhead out] ──latest──▶ ui [playhead_ui out]
(P2 adds: [prefetch] ──queued──▶ worker [decode cursor] ──latest──▶ watermark ──▶ audio gate)
```

Comparative diagrams (`frp_vs_classic.drawio`, opens in diagrams.net):
page 1 draws this network with its dumb shell, page 2 Tracktion Engine's
actual transport/playback design scoped to the demo's subset (ValueTree
state, posted atomics, rebuilt node graph), page 3 a from-scratch non-FRP
"mailbox" design that answers page 2's failure modes one by one, page 4
traces three scenarios through the FRP and Tracktion versions to show
where the logic lives in each.

---

## 2. Run it, and the shell contract

```bash
./build.sh --type debug
ctest --test-dir _build/linux_x64_debug -R daw --output-on-failure
```

The dumb shell around the network (the *entire* imperative contract — if a
behaviour needs an `if` in the shell, the demo has failed that behaviour):

```cpp
// ui thread, on user action only:
prog.set("playing", true);
prog.set<daw::LoopRegion>("loop", 0.0, 4.0, true);
prog.set("seek", shell::to_value(std::optional<double>{2.5})); // queued, never lost
eng.step(prog, clock_tag::ui);

// audio callback, per block only:
prog.set<daw::BlockDesc>("block", k * 24000, 24000, 48000.0);
eng.step(prog, clock_tag::audio);
auto playhead = get<double>(prog, "playhead");
auto count    = get<std::int64_t>(prog, "midi_count");
auto offset   = get<std::int64_t>(prog, "midi_offset"); // -1 when silent
player.render(count, offset); // dumb memcpy/interpolate, outside the engine
```

In this repo the tests *are* the shell: `tests/demo.test.cpp` drives exactly
this contract, `tests/transport.test.cpp` pins the pure math with zero engine
work. Or watch it tick instead of asserting it: `dawmulator_native`'s embedded
debug server serves the debugger UI over the same live program
(`tools/web-debugger/README.md`, and the hosted website uses exactly this
pairing). The recipe, score and defaults are shared via `daw/dawmulator.hpp`;
the debugger presses play and ticks endlessly where the page waits for
gestures. The demo proper is a library (`daw_demo`) plus tests; native builds
additionally get one tool, `dawmulator_native`, described next.

### Native (Linux/macOS/Windows) — the live process

The same page, driven by real OS threads instead of a wasm worker. Build the
normal native tree and run the host:

```bash
./build.sh --type debug
_build/linux_x64_debug/demo/daw/native/dawmulator_native --open
# without --open, open the printed http://127.0.0.1:9003/ yourself
```

- `native/host.hpp` owns the program and runs the **ui region on its own
  thread** (60 Hz) and the **audio region in miniaudio's device callback**.
  The browser is connected over WebSocket to the same page; `app.js` only
  swaps its Worker facade for a WebSocket facade (`?native=1`, or the
  `CLOCKR_NATIVE` marker the native server injects).
- `block` is set and `step(audio)` runs inside the real callback; the dumb
  player in `native/synth.cpp` turns the scheduling outputs into a short
  note. If no device can be opened (CI, headless), miniaudio's **null
  backend** drives the same callback silently; `--null-audio` forces it.
- The uWebSockets loop only enqueues commands and reads lock-free snapshots;
  it never touches the program. Every set lands at a tick boundary on the
  owning region's thread — the same discipline the debugger uses.
- `rebuild engine` stops the device, joins the ui thread, reconstructs the
  program from `daw/dawmulator.hpp`, and restarts both. Block size/sample
  rate controls reconfigure the device off the callback thread.

`--selftest` is the headless deterministic acceptance run (null audio, manual
steps, the same §6.1 script as `tests/demo.test.cpp`); CTest registers it as
`daw.native.smoke` in native builds:

```bash
ctest --test-dir _build/linux_x64_debug -R daw.native.smoke --output-on-failure
```

Options: `--host`, `--port` (default 9003), `--web-root` (defaults to the
build-staged page), `--null-audio`, `--open`, `--block`, `--rate`,
`--pool-bytes`, `--no-debug`, `--debug-host`, `--debug-port` (default 9002),
`--debug-web-root`, `--selftest`. The target is gated by
`CLOCKR_BUILD_DEMO_NATIVE` (default ON when top-level) and pulls miniaudio (public
domain/MIT-0) plus the uWebSockets dependency the debugger already uses.
macOS/Windows take the same code path (std threads, kqueue/libuv, CoreAudio/
WASAPI); only Linux is exercised in CI today.

#### Live agent debugging (MCP)

When the debugger is built (the default), the native host also embeds the
debugger's `debug::server`, attaches its `command_hook` to the ui and audio
engines, and records the same trace the debugger web UI shows. The page stays
on `:9003`; the agent endpoint is the ordinary debug WebSocket on `:9002`:

```
clockr debugger listening on http://127.0.0.1:9002 (web root .../tools/web-debugger/web)
[dawmulator] MCP debugger on ws://127.0.0.1:9002/ws (example dawmulator)
```

Add the MCP server to the agent's config exactly as in
[`doc/how-to/agent-live-debug.md`](../../doc/how-to/agent-live-debug.md),
pointing at the same URL:

```json
"mcp": {
  "clockr": {
    "command": "node",
    "args": ["tools/debug-mcp/src/main.ts", "--url", "ws://127.0.0.1:9002/ws"]
  }
}
```

The agent can then use `graph_get` (example `dawmulator`), watch
`events_tail`/`events_watch`, `input_set` the scalar ports (`playing`, `seek`,
`bpm`, `master`), and read `input_get`/`output_get` — all at tick boundaries,
on the real threads. Struct ports (`loop`, `block`) are readable but not
injectable through the scalar MCP path; the page can still set them (and its
echo makes them visible to every client).

The page is a **controlled view of the host**, not an optimistic local model:
gesture handlers only send commands, and `state` messages carry the host's
boundary state (`playing`, `bpm`, `master`, `loop`, block config and the auto
toggles). The native server emits a state snapshot whenever any of those
change, so an MCP `input_set` moves the Play button, tempo and master controls
(and the network-panel values) live, while a page command with the ui clock
stopped is still echoed from the drained command queue. Only the seek form,
the drag preview, the timeline window and log/focus state stay page-local.
`--no-debug` disables the bridge, and a build with
`CLOCKR_BUILD_DEBUGGER=OFF` serves the page without an MCP endpoint.
Rebuilding the engine restarts the debug server; the MCP client reconnects.

### Web (WASM) — the interactive site

The same contract over JavaScript, via Emscripten (`cmake/emscripten.cmake`,
`wasm32_*` presets — single-threaded, no pthreads, no COOP/COEP):

```bash
emcmake cmake --preset wasm32_debug
cmake --build _build/wasm32_debug
ctest --test-dir _build/wasm32_debug --output-on-failure  # runs under node
python3 -m http.server -d _build/wasm32_debug/demo/daw/shell 8000
# then open http://localhost:8000 — serve it, do not open file:// (the
# worker fetches the module over http)
```

`shell/` is a full DAW-shaped client, not a token page:

- **The engine lives in a Web worker** (`engine_worker.js`) — the main
  thread is pure UI, the split a real host has. The worker owns both
  clocks: an **auto ui loop** (a 60 Hz message loop) and an **auto audio
  loop** — a wall-clock-locked block scheduler that ticks as many blocks as
  wall time owes, resyncing like a sound card if the tab was throttled.
  Both regions can also be stepped by hand (buttons), with or without the
  auto loops — pausing the ui loop freezes the timeline cursor while the
  audio playhead ghost keeps moving, which *is* the `latest` crossing made
  visible.
- **The debugger works against this worker too.** On a same-origin
  `clockr.debug.local` BroadcastChannel it broadcasts the graph description
  and per-tick event batches in the same shape the native debug server sends.
  Capture starts when a local debugger says hello; the shell then ticks
  `debug_engine` instances and drains a `trace_recorder`, and returns to the
  release engines when the debugger pauses. The hosted `/demo/` page serves
  it as `/debugger/?local=1`, with no WebSocket, server or protobuf in the
  path.
- **The DAW panel** renders a beat timeline with the loop band (drag its
  edges or body to set `loop`), the scripted note as a block from its start
  to its end (it flashes when it fires and the meter opens, and releases
  when its off crossing lands), a gain meter for the voice, and the
  transport cursor — driven by `playhead_ui`, the playhead crossed back onto
  the ui clock (§1). Clicking the timeline feeds `seek` (snapped to the
  grid); play/pause, bpm, master and loop toggle feed their ports; space
  toggles play.
- **The network panel** is the raw shell: type-matched widgets for every
  input port (bool checkbox, seek as a one-shot event button, the struct
  ports as field groups, block size/sample-rate selects), current values
  and per-port arrival counts for every output, per-region step/auto
  controls, and a live output log printing every arrival as it is posted.
- **The embind surface** (`Dawmulator`) mirrors the C++ contract call for
  call — the setters, `setBlock` + `tickUi`/`tickAudio`, and the getters
  (numbers cross as doubles/bools only, no BigInt friction; a not-yet-
  produced output reads NaN, never throws). The worker constructs it with
  the same set-then-tick ordering the C++ tests assume.
- **Node tests** keep the path green: `daw.shell.smoke` (the embind surface
  driving §7), `daw.shell.worker` (the worker protocol against the real
  module, auto loops included), `daw.shell.bootstrap` (the importScripts
  path a real browser worker takes, over a throwaway http server).

The build stages the whole site (`index.html`, `app.js`, `style.css`,
`engine_worker.js` + module) into `_build/wasm32_debug/demo/daw/shell/` —
serve that directory statically, no bundler involved.

---

## 3. The minimum FRP to read this demo

An `recipe<A, B>` is **not a live connection**. It is an immutable by-value
*recipe* for a subgraph. Nothing runs until instantiation, which emits a
plain `graph`; evaluation is the existing topological batch pass. Clocks are
anchored at boundaries only: `input`/`output` ports name their clock, `at<>`
names crossings, interior nodes inherit the nearest upstream anchor. There is
no ambient thread.

| Operator | Denotation | Role in the demo |
|---|---|---|
| `lift f` (N-ary over pair wires) | `f` pointwise per tick | All gating/wrap/crossing math (pure) |
| `zip` (N branches) | Pair nest = one fan-in edge per leaf in order (L8) | Assembling leaves into struct bundles |
| `scan(init, f)` | `s(t) = f(s(t−1), in(t))`, `s(−1) = init` | Transport pair, voice latch (struct state) |
| `delay` / `stepper` | Unit state / event-made-level | — (P1 needs neither: the pair state carries prev, the bundle carries seek) |
| `input<T,Tag>` / `output<T,Tag>` | Boundary ports; tags seed/check clocks | Every shell touchpoint (scalars + structs) |
| `at<Tag,mode>` | Clock change + resampling policy | Every cross-thread edge |
| `share` | Identity denotationally (L4); instantiate-once fan-out | One playhead feeding all consumers |
| `parallel` | Disjoint outputs | Composing the app from sub-networks |

Purity/totality of `tick_fn`s is a **semantic requirement**, not style:
impure lambdas break determinism, replay, and time-travel by definition.

The two crossings (producer clock `p`, consumer clock `c`): **`latest`**
(bpm, loop, playing, master) — most-recent-sent as of the consumer tick,
holds when the producer is quiet; **`queued`** (seeks) — every producer
epoch's value since the consumer's last tick, in order, none lost, one entry
per consumer tick. Laws accepting the work: L1 determinism · L2 synchrony ·
L4 share-identity · L5 debug/release parity · L6 capacity · L7 guarded cycles
(feedback legal iff through `delay`/`scan`) · L8 pairing · L9 epochs.

---

## 4. Cohesive struct wires (not scalar discipline)

The wires carry structs wherever a bundle crosses a clock or persists in
state — the paying use of kernel user values (G), and exactly the cases a
phantom pair cannot serve. `LoopRegion` crosses ui→audio whole;
`BlockDesc` is set per block whole; one `scan` over `TransportIn` holds the
whole tick bundle; `PosPair` and `Voice` persist as struct state. Small
structs (`PosPair`, `Voice`: 16 bytes) ride inline with value semantics; the
rest ride ref-counted pool cells, allocated per tick from the region pool and
reclaimed in batches — bounded bump allocation, no heap on the ticking
thread. `describe` schemas cover every wire struct — including the optional
`seek`, which renders its beats when engaged and absent otherwise — so the
debugger shows values inline with their fields.

That is what dissolved the old scalar discipline — and with it the rebase
decomposition (three single-input scans plus stateless lifts emulating one
struct scan, because an absolute seek of an integrator had no struct wire).
The transport is a single equation again, wired directly into one scan:
`daw::transport_step` is its executable spec, and the engine test proves the
wire agrees with it tick for tick.

What stays phantom is purely authoring-level bundling: the bundle lift still
takes five leaves, the scheduler triple three/three/five, and those leaves
ride positional pair nests. Structs-everywhere would trade per-tick cell churn for
spelling comfort; the architecture prices that explicitly (ROADMAP Phase H:
surface sugar over phantoms, zero kernel cost). The remaining nests are
shallow, named, and every lambda parameter is read — they are H's (smaller)
forcing function, not this demo's problem to solve.

---

## 5. The P1 network (`daw/network.hpp`, `daw/transport.hpp`)

- **Transport.** One bundle lift assembles `TransportIn` where it is
  consumed; ONE joint scan keeps `{prev, cur}` (the step is `transport_step`
  directly), shared as the single playhead (L4). The pair state gives
  schedulers the crossing interval `(prev, cur]` with no second delay tap.
- **Tempo.** Single BPM; `advance` is exact. A K-point curve stays a bounded
  walk over closure-captured control points in the lift body — same wires, no
  new operators.
- **Loop + ports.** Struct ports, set whole (`set<LoopRegion>`,
  `set<BlockDesc>`); `playing`/`seek`/`bpm`/`master` stay scalar (`seek` is
  `queued`: event, never lost). All sources are created once and shared — a
  missing `share` either throws (duplicate port name) or duplicates the node,
  and the structure test counts 6 inputs to pin it.
- **Schedulers.** One audio tick per block; sub-tick precision is computed
  *inside* the pure step function — no per-sample ticks. The count twin reads
  `(block_start, pair, loop)`, the offset twin additionally `(block, bpm)`,
  over a shared block-start; the firing interval is half-open, so a note on
  a block boundary fires once, on the later block; a block spanning the loop
  end splits into `(prev, end] ∪ (start, cur]`. Outputs are the on/off
  counts plus the first offset (P1's stand-in for the future batch-handle
  outputs).
- **Notes.** A score note is `(start beat, length, id)`: the off crossing is
  `len` beats after the on, mapped onto the loop (`note_end_beat`), so a
  note longer than the loop's remainder ends on the next lap — an absolute
  end beat past the loop end would never be reached by the transport.
  Degenerate lengths (0, or exactly the loop length) fold the off crossing
  onto the on: the note blips inside one block and the gate treats it as
  over.
- **Voice.** The twin counts fold into one gate event per block
  (`voice_gate`: on, off, both→off — a note entirely inside one block has
  no sub-block envelope in P1) driving a `Voice` scan: the gate opens the
  note id, the off closes it, silence sustains, and an off with nothing
  sounding is a no-op — which is what makes the wrapped end beat harmless
  when crossed before any note. Gain is voice-active-then-master.
  Monophonic: the N-voice pool needs array states (future work), and the
  steal policy waits with it — the gate scan is its single-voice shape.

---

## 6. Port map (pinned names)

UI→audio: `playing:latest bool · seek:queued optional<double> ·
loop:latest LoopRegion · bpm:latest double · master:latest double`. Audio
inputs (per block): `block:BlockDesc`. Audio→shell: `playhead:double ·
midi_count:int64 · midi_offset:int64 (−1 when silent) ·
midi_off_count:int64 · voice_gain:double`. Audio→ui (the display feedback):
`playhead_ui:latest double` — the same playhead node crossed back, sampled
at ui cadence, holding while audio is quiet; the interactive shell's
timeline cursor reads it.

---

## 7. Acceptance and edge cases

First cut (§6.1 test): playing, 120 bpm, loop (0,4] on, 1-beat blocks. Five
blocks give playheads 1, 2, 3, 0 (wrap with remainder), 1; the scripted note
at 3.5 — one beat long, so its off crossing wraps to 0.5 — fires exactly on
the spanning block with offset 12000, the voice opens there and releases on
the next lap's early block (the off crossing also fires on block 0, before
any note: a no-op on the already-silent voice); debug and release agree on
every output, every block (L5). Further engine tests: pause freezes two
blocks; seek-while-paused jumps absolutely; resume advances from the landing
(the scan carries the jump directly); same-epoch double seek keeps the last
(L9a); the playhead crossed back to ui samples the newest audio value at ui
cadence and holds while audio is quiet (latest, not queued); owning and
borrowing programs agree on struct wires.

Pure-function table (`tests/transport.test.cpp`, no engine): seek wins over
advance · seek while paused · wrap with remainder, never clamp · multi-wrap
termination · degenerate/disabled loop passes through · seek past the end
(wrap once with loop on, pinned policy with loop off) · half-open firing
boundaries · loop-split firing · exact offsets · defensive offset clamp ·
note end through the loop wrap (degenerate lengths fold it onto the on) ·
voice gate open/close/sustain, both-in-one-block is over.

---

## 8. What's next (P2/P3 and core work)

- **P2** — worker decode loop: `prefetch` as `queued` events audio→worker, a
  decode-cursor scan on `worker`, a versioned ring out-of-band, `watermark` as
  `latest` worker→audio; schedule only at/below the watermark, underrun →
  silence + flag port (audio never blocks, worker may). Phased with scripted
  slow-decoder fault injection first.
- **P3** — three-clock tear/stress (audio↔worker concurrent ticks,
  pair-consistency à la C2 L9b).
- **Core proposals** (for the library, not the demo; tracked in `roadmap/README.md`):
  a generic batch-handle wire + descriptor protocol + epoch pools (the P1
  count/offset outputs become handle twins); K-point tempo tables; array
  states for polyphonic voices. Already landed underneath P1: user values (G)
  — struct wires, pool cells, `set<T>` — plus N-ary `zip`/`lift` and
  per-region ticks + links carrying both crossings.
- **Automation** — app-level, and both flavors already exist here under other
  names: discrete automation *is* the note scheduler (queued semantics,
  sample offsets in-engine, dumb player executes); continuous automation is
  the §5 tempo-walk pattern with the shared playhead as the lookup key
  (`lift(lookup, playhead, lane)`). Cross the lane whole, evaluate on the
  consuming region (a §4 struct wire; `latest` publishes only when dirty, and
  lane edits are rare gestures); intra-block ramps belong to the dumb player
  like every other sub-tick concern. A `lane` spelling helper would be Phase
  H surface, not kernel (tracked in `roadmap/README.md`, Phase H).
- **Non-goals** (explicitly out): sample rendering through the graph ·
  file/network stacks in-engine · widgets · unbounded `switch` on the audio
  clock · worker SLAs in the denotation.
