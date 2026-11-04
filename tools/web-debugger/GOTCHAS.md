# Debugger development gotchas

Problems hit while building this tool, and how each was solved. Read this
before touching the wire pipeline, the CMake plumbing, or the page.

## C++ / build

- **New files must end with a newline.** The project builds with
  `-Werror,-Wnewline-eof`; every file created from scratch failed the build
  until a trailing newline was appended. Compile new files immediately.
- **Sequence numbers shift when the vocabulary changes.** Collapsing the
  per-node triple to a begin/end pair renumbered every pinned `seq` in the
  trace tests — region events consume sequence numbers too, so recompute them
  from the new emission order (begin, node pairs, then region end).
- **`region_step_end` needs its own `threw` field.** Trying to reuse the
  value payload for the abort flag conflated "threw" with "produced `true`";
  the text format now renders `"threw":true/false` and the proto carries it
  as a bool.
- **`std::string_view{nullptr}` segfaults.** Boundary port names are null for
  interior nodes; the label parity test crashed wrapping one. Guard nulls
  before wrapping — and the UI serializer must do the same.
- **Adding a field to `descriptor` breaks every literal.** Positional
  initializers fail to convert; designated ones trip
  `-Wmissing-designated-field-initializers`. The struct is constructed
  literally in many tests (`engine`, `graph`, `recipe`), so update all of them in
  the same commit.
- **Protobuf tags are not what you expect.** The "29.x" line is tagged
  `v5.29.x` (libprotobuf 5.x ships libprotoc 29.x). This tool ended on
  `v6.31.1` for the abseil fix below.
- **Abseil emits broken `-Xarch` flags on single-arch macOS Ninja builds.**
  Its copts file unconditionally passes `-Xarch_x86_64 -maes -msse4.1` (and
  the arm64 counterparts) on APPLE+Clang; with a single `-arch arm64` command
  line, clang sees the other arch's flags unguarded and errors out. Clearing
  `CMAKE_OSX_ARCHITECTURES` does nothing — that branch never reads it. The
  fix was a newer abseil (its flags use `SHELL:` grouping), pulled in via
  protobuf v6.31.1.
- **Removed CMake options persist in the cache.** Deleting the
  `protobuf_BUILD_LIBUPB=OFF` line left the cached `OFF` behind, and libprotoc
  needs libupb (its bootstrap `descriptor.upb.h` include comes from there).
  Set it explicitly `ON` and wipe the dependency's build directory.
  FetchContent subprojects share the parent cache — a removed `set()` is not
  a reset.
- **Third-party headers vs `-Weverything -Werror`.** abseil, protobuf,
  uWebSockets, and the generated `*.pb.h` are all not warning-clean. Mark
  their include dirs system via `INTERFACE_SYSTEM_INCLUDE_DIRECTORIES` — on
  the real targets, not the aliases (`set_target_properties` refuses
  `protobuf::libprotobuf`; use `libprotobuf`), plus the generated dir.
- **`-Wswitch-enum` *and* `-Wswitch-default` are both enforced.** List every
  enumerator *and* keep a `default:` label; the unreachable default is fine
  because `-Wcovered-switch-default` is suppressed. (Protobuf enums also
  carry `INT_MIN/MAX_SENTINEL_DO_NOT_USE_` values.)
- **uWebSockets/uSockets ship no CMake.** Following the tlsf precedent, both
  are populated manually and wrapped: uSockets as a static lib of the `src/`
  C files plus one eventing backend with `LIBUS_NO_SSL` (no crypto, no other
  backends) — epoll/kqueue on Linux/macOS, libuv on Windows (`cmake/libuv.cmake`
  fetches it) — and uWebSockets as a header-only INTERFACE target. Pinned to
  uWebSockets v20.80.0 + uSockets v0.8.8.
- **`uWS::Loop::get()` returns `uWS::Loop*`, not `us_loop_t*`.** Convert with
  `reinterpret_cast` (`static_cast` is rejected; C-style casts trip
  `-Wold-style-cast`).
- **`FetchContent_Populate` is deprecated (CMP0169) with new CMake.** Our
  tlsf-style manual wrapping triggers a dev warning; harmless, but a future
  CMake will require `FetchContent_MakeAvailable`.

## Network / semantics

- **One `debug_engine` per ticking thread.** Its sequence counter and tick
  context are plain members; the recorder's per-tag SPSC queues assume one
  producer per tag. The demo constructs one engine per clock sharing a
  recorder.
- **SF `share()` discipline bites the demo too.** Reusing a plain fragment
  for two consumers emits its source (and port) twice:
  `Input port name already exists`. `share()` every fan-out fragment.
- **Publish before consumers run.** A consumer ticking before its producer
  publishes stages monostate through a partial lift (`bad_variant_access`
  spam). The demo publishes once on the producer clock before starting the
  other threads, and keeps holds on the producer clock behind `latest`
  crossings so a consumer never sees an unset input.
- **`program::graph()` carries empty regions/sharing-point spans.** Serialize
  structure from `program.topology()`, never from `graph()` alone.

## The page (`web/`)

- **proto3 omits default-valued fields.** Node id `0`, empty strings, and
  `threw=false` are simply absent on the wire; the JS decoder must default
  every scalar. The node-run smoke test pins this.
- **proto3 packs repeated scalar fields.** `repeated uint64 fan_in` arrives
  as *one* wire-2 field holding varint after varint — not as repeated
  varints. The hand-rolled reader first decoded every fan-in as `null`,
  which crashed the layout (`computeDepth` on `undefined`), left
  `state.layout` null, and blanked *both* the graph and the event list.
  `decodePackedVarints` handles the packed form and the decoder still
  accepts the unpacked form; the smoke test now covers the packed encoding
  the real serializer emits.
- **A `1fr` grid track grows with its content.** The tick strip kept widening
  the whole window. The fix is `grid-template-columns: minmax(0, 1fr) auto`
  plus `min-width: 0` on the timeline and lanes, with `overflow-x: auto` on
  the lane scroller — and incremental DOM appends with live-only auto-scroll
  instead of rebuilding thousands of tick cells at 30 Hz.
- **jsdom probe artifact: never stomp `global.Event`.** Overriding it with
  jsdom's class breaks node's own undici WebSocket
  (`must be an instance of Event. Received an instance of Event`). Only set
  the globals the page actually needs.
- **Measured durations are near the noise floor for trivial nodes.** A
  `steady_clock::now()` pair costs tens of nanoseconds via vDSO, so a
  passthrough tick genuinely reports e.g. `40 ns` — that is signal, not a
  bug. The UI formats sub-microsecond times as ns so the floor reads
  honestly. Related test lesson: durations can never be pinned (they are
  timing-dependent), so the trace tests match each line only up to the
  `duration_ns` field and assert its presence instead of its value.
- **Equal-sized command and reply queues are NOT lossless by construction.**
  The write-back path has two SPSC queues per region sized alike, but their
  producers/consumers are paced differently: a tick boundary can consume a
  whole command queue and post that many replies, while the uWS loop ships
  replies only on its 33ms drain timer — and a hot region (the demo's audio
  thread ticks every ~3ms) runs many boundaries between two reply drains. The
  "one reply per command, so capacity can never be outstripped" argument is
  wrong; it ignored the pacing, and a 3000-command WS burst aborted the debug
  build (`reply queue overflow` assert) while release silently lost Acks. The
  fix is tick-side backpressure with an *exact* reserved-slot counter
  (`replies_outstanding`: tick thread increments before posting, loop thread
  decrements after popping — never an approximate `size()` probe, which can
  over-apply): `command_drain` consumes at most `capacity - outstanding`
  commands and leaves the rest queued for a later boundary. Corollary: never
  put a client-drivable `assert` on a lossy-queue path — an abort in a
  debugging tool is worse than a late reply.
- **A throwing tick publishes no epoch — the Ack epoch must come from the
  step hook, not from `published_epoch`.** `engine::step` publishes the new
  epoch with a release store *after* its try block, so a node that throws
  leaves `published_epoch` at the old value even though the trace's
  `REGION_EVAL_END` names the aborted tick (with `threw = true`).
  `step_hook::after_step` therefore hands the hook the attempted epoch on that
  path (and the published one on the clean path); `command_finish` takes it as
  a parameter and must run from the hook's after path, which the engine calls
  exactly once even when the tick throws. Reading `published_epoch` after a
  `--throw-every` tick would ack the previous tick and break an agent's trace
  correlation.
- **The boundary seam is a `step_hook`, never a "step observer".** GLOSSARY
  reserves `observer` for the trace's observation map ω (`debug_engine`,
  reported through `trace_sink`); naming the attachable before/after seam an
  observer conflates the two. The kernel type is `kernel::step_hook` /
  `engine::set_step_hook`, and the debugger's adapter is
  `debug::command_hook`.
- **A step hook must not throw.** `before_step` runs before the tick's own
  guards and `after_step` runs on the unwind path of a throwing tick, so a
  throw there compounds the exception already in flight (the engine
  deliberately does not wrap the calls). Keep hooks cheap and non-throwing —
  the debugger's command path counts lost replies instead of aborting the
  host.
- **Whole-tick time rides the existing region bracket.** Measuring the tick
  needed no new hook or event: `debug_engine` stamps `steady_clock` in
  `on_region_step_begin` and reports the elapsed time on the `region_step_end`
  it already emitted, reusing `trace_event.duration_ns` (and the proto field,
  and the text `"duration_ns"` rendering). One subtlety this preserves: the
  throwing path also reports a tick time, since the end hook fires from the
  unwind path — a slow abort is as visible as a slow success.
- **ELK's layout partitions do not guarantee disjoint clock bands.** They
  order bands along the layout direction, which holds only while no edge runs
  against that order. Here every `ui → audio` edge does (audio is the top
  band but consumes from the lane below it), and a delay-guarded cycle or an
  isolated input lets a partition's nodes bleed into another's band — measured
  on real graphs as one `ui` node sitting in the `audio` band. `elk.layered.
  feedbackEdges` and cycle-breaking strategy do not fix it. The page therefore
  uses ELK only for ordering (layering and crossing-minimised columns) and
  imposes the lane geometry itself in `web/layout.js`; the layout smoke test
  pins the bands as disjoint.
- **Per-child layout direction does not work for swimlanes either.** The
  natural idea — lane compounds with `elk.direction: RIGHT` inside a
  `DOWN` root (`elk.hierarchyHandling: INCLUDE_CHILDREN`) — is ignored: each
  compound keeps the root's direction, so "lanes" come out as nested vertical
  stacks, not left-to-right bands.
- **Port tabs are placed, not laid out.** Inputs/outputs render as small tabs
  centered on their band's left/right edges — inputs left, outputs right,
  each hanging half outside the region like a patch-bay jack — rather than
  as flow nodes. (They used to straddle the top/bottom borders, which put two
  tab rows plus two value-chip rows in every gutter; moving values inline and
  rails to the sides emptied the gutters, so the lane gap is purely visual
  again.) Each rail shares one center x across all bands; columns start past
  the widest input tab half and the viewport extends past the widest output
  half, so hanging tabs never overlap content or clip. Tabs on a rail are
  swept apart vertically. They are excluded from row packing (so they never
  stretch a band) but included in the overlap and viewport checks. Dragging
  locks a tab's x to its rail. A clock with no internal nodes still gets a
  band sized by its rails alone, so its ports attach as tabs (the DAW's ui
  lane is exactly this: five inputs, no compute); without it those ports
  silently fell back to full-size flow nodes.
- **Edges are strictly directional, even the backwards ones.** Every route
  leaves its source from the right edge and enters its target through the
  left, so inputs are always left and outputs always right. Delay feedback
  (the only genuinely backwards edge) loops around the right side and comes
  back in on the left, with the horizontal return running in the row gap
  above the target — rows are a uniform grid, so that y cuts no box. The
  smoke test pins both properties (axis-aligned segments; first point on the
  source's right edge, last point on the target's left edge).
- **Port values live in the tab, not in a chip.** A separate value chip next
  to a border tab would share the gutter with the opposite border's tabs and
  chips — four competing rows in ~40px. Rendering `name value` inline in the
  tab (truncated to fit, full text in the inspector) removes the whole class
  of collision, at the cost of sizing tabs for name plus a short scalar.
- **An asynchronous layout can repaint a graph that is gone.** `elk.layout()`
  returns a promise, and a fast example switch (or reconnect) can deliver a
  new `GraphDescription` while the previous layout is still in flight. A
  monotonically increasing request id gates the `.then`, so a stale layout is
  dropped instead of painting boxes over the new network.
- **jsdom cannot do pointer capture or size elements.** `svg.setPointer
  Capture` is absent (guard it with a try/catch) and `getBoundingClientRect`
  returns zeros, so a probe must stub the graph element's rect before
  exercising pan/zoom. The vendored ELK bundle is a UMD file: load it in node
  with `createRequire` and assign it to `globalThis.ELK` before importing the
  page (jsdom does not fetch the `<script>` tag).
- **Vendored browser deps carry their own licence.** ELK is EPL-2.0 OR
  GPL-3.0-or-later, not MIT: `web/vendor/` holds the unmodified bundle, the
  upstream licence text, the pinned version and SHA-256, and the repository
  `LICENSE.md` carries the third-party notice. Never hand-edit a vendored file;
  re-download and re-record the hash.
