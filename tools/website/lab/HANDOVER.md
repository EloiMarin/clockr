# Animation lab handover

This is the agent-facing handover for `tools/website/lab/`. `README.md` is the
quick-start; this file records the current state, the contracts, the
sketches and the traps found while building them.

## What this is

A throwaway playground for **animated graphics driven by a wasm build of the
library**. The point is fast visual iteration before anything is allowed near
the real landing page:

- one standalone Emscripten project, pulling the library in as a subdirectory;
- every `animations/*.cpp` is one translation unit in a single embind module
  (`lab_shell.js` / `lab_shell.wasm`, exported as `LabShell`);
- every `web/*.js` is a renderer with the same `mount(canvas, lib)` contract;
- `lab.js` builds a card per sketch and loads the shared module once;
- `lab-common.js` owns the canvas/DPR/resize/visibility/reduced-motion/pointer
  plumbing and the `requestAnimationFrame` loop.

**Not deployed as a page.** The real site is `tools/website/`; the lab is
deliberately outside that build's pages, but the site build now compiles
`lab_shell` and stages the sketches as the **404 gallery**. A native-build
source tarball excludes `tools/website/` entirely.

## Run / iterate

```bash
cd tools/website/lab
./build.sh                 # configure + build lab_shell, stage _site/
node serve.mjs             # http://127.0.0.1:4180/
```

If `emcmake` is not on `PATH`:

```bash
nix shell nixpkgs#emscripten -c ./build.sh
```

`serve.mjs` serves **source files directly** (`index.html`, `lab.js`,
`lab-common.js`, `web/*.js`) and only the generated `lab_shell.js` /
`lab_shell.wasm` from `_build/`. So:

- JS/CSS/HTML edits: just refresh the browser.
- C++ edits: rerun `./build.sh` (incremental; Ninja does the right thing).
- `_site/` is an optional self-contained staged copy, not what `serve.mjs`
  reads. Do not edit `_site/`.
- `_build/` and `_site/` are generated and gitignored.

## Current sketches

All twelve compile and link in the same module (`ball_hold.hpp` is a helper
header shared by the fling sketches; the glob only picks `*.cpp`). `lab.js`
lists the eleven card sketches in this order; `cursor-clocks` has left the
grid and is mounted page-wide by `web/page-experience.js` (see "Page-level
arrangement" below).

| id | class | C++ | JS | What it shows |
| --- | --- | --- | --- | --- |
| `signal-bloom` | `SignalBloom` | `animations/signal_bloom.cpp` | `web/signal-bloom.js` | Harmonograph with feedback, delay trail and a latest-sampled ghost |
| `clock-loom` | `ClockLoom` | `animations/clock_loom.cpp` | `web/clock-loom.js` | Three clock lanes, latest fibres, queued beads |
| `cursor-clocks` | `CursorClocks` | `animations/cursor_clocks.cpp` | `web/cursor-clocks.js` | Clock Loom as three concentric clock faces around the smoothed pointer |
| `queued-ripples` | `QueuedRipples` | `animations/queued_ripples.cpp` | `web/queued-ripples.js` | Queued events as expanding rings; latest pointer as a live ring |
| `shape-of-hold` | `ShapeOfHold` | `animations/shape_of_hold.cpp` | `web/shape-of-hold.js` | A spring state in `scan`; raw pointer vs held point |
| `three-clocks-one-brush` | `ThreeClocksOneBrush` | `animations/three_clocks_one_brush.cpp` | `web/three-clocks-one-brush.js` | Three brushes at different rates; one visibly stepped by latest |
| `deterministic-replay` | `DeterministicReplay` | `animations/deterministic_replay.cpp` | `web/deterministic-replay.js` | Same recipe + reset redraws the same figure |
| `flow-field` | `FlowField` | `animations/flow_field.cpp` | `web/flow-field.js` | Wasm-computed field parameters + JS particle renderer |
| `fling` | `Fling` | `animations/fling.cpp` | `web/fling.js` | Grab and throw a ball; the whole mechanics live in two inline-struct `scan`s |
| `ink-echoes` | `InkEchoes` | `animations/ink_echoes.cpp` | `web/ink-echoes.js` | Drawing with the pointer; four delay lanes repaint the gesture at different depths |
| `knock-stamps` | `KnockStamps` | `animations/knock_stamps.cpp` | `web/knock-stamps.js` | Clicks as optional events; same-step stamping of the orbit plus a knock boost |
| `stirred-field` | `StirredField` | `animations/stirred_field.cpp` | `web/stirred-field.js` | The fling ball over a slow faint field; its stir quickens and swirls the flow |

### Signal Bloom

- Methods: `tick`, `setPointer`, `reset`, `headX/Y`, `feedX/Y`, `ghostX/Y`,
  `phase`.
- Recipe: ui `dt` and a smoothed pointer drive two phase `scan`s; two-term
  sin/cos gives the head; eight chained `delay<double>` units give the feedback
  strand; the worker clock samples the head through `latest` every fourth
  frame (the stepped ghost).
- Renderer: ring buffers for head/feed/ghost, additive amber/teal, pointer
  bends amplitude/rate. Self-animating; no auto pointer.

### Clock Loom

- Methods: `tick`, `reset`, `aPhase`, `uPhase`, `wPhase`, `uSample`,
  `wSample`, `pulse`.
- Recipe: one phase `scan` per clock. Audio ticks 3× per frame, ui 1×, worker
  every 4th. `latest` crosses audio→ui and ui→worker; a `queued` pulse crosses
  ui→worker and drains one event per worker tick. `pulse()` is NaN when a
  worker tick has no event.
- Renderer: three labelled lanes, moving markers, dashed latest fibres, amber
  beads for queued events. Self-animating.

### Cursor Clocks

- Methods: `push`, `tick`, `reset`, `aPhase`, `uPhase`, `wPhase`, `uSample`,
  `wSample`, `hopU`, `hopW`.
- Recipe: Clock Loom's crossing topology with a gentler tempo (audio ~3.2 s,
  ui ~7 s, worker ~12.5 s per revolution; rates 0.105/0.145/0.320) plus a
  click value that hops ring to ring: `input<optional<double>, audio>("click")`
  crossed queued audio→ui (`hopU`), then relayed by the host into
  `input<optional<double>, ui>("relay")` and crossed queued ui→worker
  (`hopW`). Optional (fire-and-forget) inputs keep each hop to exactly one
  epoch — an in-recipe `at<worker, queued>(at<ui, queued>(...))` chain would
  flood the worker's queue with empty-drain monostates (a crossing source
  that is not an input port publishes on *every* epoch), which is why the
  relay lives in `tick` (the ui seam, one extra frame, one line).
- The dial centre is not in the network at all: the rings pin exactly under
  the cursor, renderer-side (no drift, no smoothing, no idle animation).
- Renderer: ring versions of the three lanes (inner audio spinner, middle ui
  hand, outer worker dial), dashed fibres bowing inward for the latest
  crossings with a hollow echo circle at the consumer's sampled phase (the
  outer one hops every fourth frame).
  The dial is deliberately **big but subtle** — the intended landing-page
  use is a companion that follows the visitor all the time, so the radii are
  capped in pixels (`ringRadii`, unit ≤ 600 → ~78/150/222 px), dots are
  spaced proportionally to the radius, strokes are thin, alphas low, glows
  soft, and there is no legend/label text. Keep it airy when tuning; do not
  let it fill the viewport.
  It is also deliberately **not a reticle**: the rings are a faint continuous
  circumference (α 0.08 — without it the marks read as specks floating
  around the cursor) under soft dots (`setLineDash([0, radius·0.12])`,
  α 0.20, per-ring `lineDashOffset`), with no radial tick marks, no straight
  centre-to-head "hand" lines, no centre crosshair dot, and no straight
  radial spoke for the hop (that was the gunsight look). Heads are glowing
  motes with arc trails; the waiting value breathes slowly. Colours are
  local pastel variants (apricot/sage/lilac), and the canvas is cleared with
  `clearRect` (transparent) rather than
  filled, so it can float over the live page.
  The click hop is staged as a hand-off: the value flies in from the cursor
  tip in the last movement direction, sticks onto the inner ring (ray/ring
  stick-angle math, centre-fallback). The fly-in is expressed in ring-local
  coordinates — radius 0 at the tip → rAudio at the stick angle around the
  *current* centre — so the ball rides the dial when the pointer moves
  mid-flight; never store the trajectory in absolute pixels. Then it must be
  *touched* by each ring's
  own hand before it travels to the next ring
  (fly → waitAudio → toMid → waitMid → toOut → waitOut → exit; each wait is
  gated by the hopU/hopW event AND the hand's touch threshold — thresholds
  must clear the per-tick angle advance so the head can never jump the
  window: audio ≈0.033 rad/frame (0.011/tick), ui ≈0.015/tick, worker
  ≈0.034/tick). Reaching the
  outer dial, the value exits outward and fades. Click ids pair with hop
  events 1:1 by dense counter (`pushSeq` in JS mirrors the wasm counter).
  Reset clears everything.
- Page-wide use: `mount(canvas, lib, options)`; `options.windowPointer`
  attaches the pointer listeners to `window` (the overlay canvas is
  `pointer-events: none`) and ignores clicks on interactive elements. The
  controller also exposes `balls()` — the launched values in canvas pixels
  with a `strength` — which is how the ambient field layer consumes them.

### Page-level arrangement (cursor dial + ambient field)

The lab now prototypes the landing-page shape rather than keeping Cursor
Clocks in a card:

- `index.html` has two fixed full-viewport canvases: `#page-field`
  (z-index 0, bottom) and `#page-dial` (z-index 1). Both are **behind the
  page content** (header/main/footer at z-index 2) — the dial is deliberately
  not overlaid on the cards. Both are `pointer-events: none`.
- `lab.js` mounts `web/page-experience.js` once the wasm module is ready:
  the dial via `mount(dialCanvas, lib, { windowPointer: true })` and the
  field via `page-field.js`; the field calls `dial.balls()` every frame.
- `web/page-field.js` is a blob-free ambient flow field: no cursor obstacle
  and no wasm state — only the launched balls push/swirl the particles.
  Count scales with area (`clamp(w·h / 1600, 320, 1000)`); physics is one
  pass. Trails are an **explicit bounded history** (`TRAIL` frames per
  particle) recomposed every frame after a full `clearRect`, drawn as
  grouped strokes per (age, tint) with a quadratic alpha that reaches zero,
  plus three grouped dot fills for the calm "stars". Do **not** go back to a
  persistent `destination-out` wash: its alpha multiplies asymptotically and
  parks at 1/255 in 8-bit alpha, leaving permanent ghost trails behind slow
  particles (that was the user-reported bug). Calm segments shorter than
  `TRAIL_MIN` are skipped, so a static field costs only dot fills.
  Tuning knobs at the top: `BALL_R`, `PUSH`, `SWIRL`, `DRIFT`, `TRAIL`,
  `TRAIL_ALPHA`, `TRAIL_MIN`, `DOT_ALPHA`, `COLORS`. Measured: a ball
  makes nearby particles move ~19× faster and leaves visible trails —
  clearly felt, cheap to run.
  The **wrap seam is a kept feature**: when a ball pushes particles across a
  viewport edge, the segment from the old position to the wrapped one spans
  the screen, producing the long streaks seen during interaction (user
  explicitly likes them). Do not "fix" it by breaking the trail at the edge;
  ask first. Seams are length-detected (`SEAM_LEN` × min(w, h)) and drawn in
  their own pass with `SEAM_ALPHA_SCALE` (default 0.35) and `SEAM_WIDTH`
  (0.6 px), so the glitch can be dimmed *without* dimming the field's dots
  and ordinary trails. Set the scale to 0 to make the streaks effectively
  invisible; raise it toward 1 for the old look.
- Nothing about the field touches FRP state: it is deliberately a pure
  consumer of the dial's renderer state. Port it to the site with the same
  split (field behind content, dial overlay above).

### Queued Ripples

- Methods: `push`, `setPointer`, `tick`, `reset`, `eventX/Y`, `latestX/Y`,
  `count`, `drained`, `queueDepth`.
- Recipe: paired `queued` `x`/`y` crossings ui→worker, plus a `latest` pointer.
  `push` appends to a pending vector; `tick` sets both values and steps ui once
  per pending click, then steps worker once to drain one queued event. Both
  coordinates are set before every producer step; they stay paired.
- Renderer: expanding rings for drained events, a live latest ring, event and
  backlog counters. It auto-queues a wandering event roughly every second so
  the sketch is alive before the first click; a real pointer takes over the
  live ring for five seconds and clicks add more events.

### The Shape of a Hold

- Methods: `tick`, `setPointer`, `reset`, `rawX/Y`, `heldX/Y`.
- Recipe: raw pointer inputs on ui; two critically damped springs over a small
  inline `spring_state` user struct (position + velocity) in `scan`s. No pool
  allocation for that state.
- Renderer: faint raw ghost + dashed tether, solid green held point with a
  short trail. It auto-orbits before the first gesture and resumes five seconds
  after the last pointer event.

### Three Clocks, One Brush

- Methods: `tick`, `reset`, `x0/y0`, `x1/y1`, `x2/y2`.
- Recipe: audio phase pair (3 audio ticks/frame), ui phase pair (1/frame), and
  a worker phase pair sampled onto ui through `latest` (worker ticks every 4th
  frame). `x1/y1` is the deliberately chunked/stepped brush.
- Renderer: persistent canvas with a very slow fade, three translucent brushes
  and heads; reset clears the painting.

### Deterministic Replay

- Methods: `tick`, `reset`, `x/y`, `x2/y2`, `phase`.
- Recipe: two ui phase `scan`s with fixed rates (1.2, 3.6), deterministic
  sin/cos point + secondary point. No randomness. `reset` reconstructs the
  program (`std::optional<kernel::program>::emplace(recipe())`) and warms it
  with a fixed step.
- Renderer: ring-buffer trail with a bright head and a secondary point; the
  shared restart button clears the canvas and replays the same figure.

### Flow Field

- Methods: `tick`, `setPointer`, `reset`, `angle`, `curl`, `scale`, `speed`,
  `warpX`, `warpY`.
- Recipe: ui `dt` plus a smoothed pointer feed six phase `scan`s / `lift`s;
  the wasm side exposes field parameters, not per-particle state.
- Renderer: 260 seeded particles move through the parameterised field with a
  pointer pull, drawn with persistent fade trails. Reset reseeds and clears.

### Fling

- Methods: `tick`, `setPointer(x, y, down)`, `reset`, `ballX/Y`, `velX/Y`,
  `targetX/Y`.
- Recipe: two scans, one per axis. Each scan's state is a small inline user
  struct (`ball_state { float x, v; }`, 8 bytes); the input is
  `ball_in { float target, dt; bool grabbed; }` (12 bytes), so the entire
  simulation stays on the `user_inline` path — no pool cell ever. The fold is
  a critically damped spring whose `omega` depends on `grabbed` (tight while
  pressed, slack free), followed by a wall bounce with restitution, so the
  velocity held across release *is* the throw.
- Renderer: maps `[0,1]` onto the canvas (walls at 0.02/0.98, framed), draws
  target ring and tether while grabbed, a teal ball with trail and a
  velocity-proportional streak, plus amber ripples where an axis's fast
  velocity flips sign (a bounce). Idle > 5 s wanders the target so the slack
  spring meanders. A window-level `pointerup` releases even if the pointer
  left the canvas; "leave" also releases.
- Notable: the scan emits its whole state through an *output user-struct
  port* (`output<ball_state, ui>`) — the class decodes x/v from the inline
  bytes in the getters, the first sketch to do that.

### Ink Echoes

- Methods: `setPointer`, `tick`, `reset`, `laneX/laneY(k)` (k = 0..3 in
  increasing depth), `pointerX/Y`.
- Recipe: shared raw pointer levels fan out into four delay chains — 3, 7,
  14 and 22 chained unit `delay<double>`s per axis (44 delay nodes total).
  The chain helper is recursive: the arena grows a type per delay, so the
  accumulator cannot be reassigned.
- Renderer: persistent canvas; each frame each lane paints the stroke segment
  it just traversed (skipped when a lane barely moved), so the ink
  accumulates and the deep lanes finish strokes after the pointer has moved
  on. A slow auto-signature keeps the card alive before the first gesture and
  resumes five seconds after the last one; a real pointer takes over drawing.
  Restart wipes the canvas.
- Notable: the pure fan-out/delay showcase with no folds — every node is a
  stateless lift over delays.

### Knock & Stamp

- Methods: `pulse`, `tick`, `reset`, `orbX/Y`, `knock`, `snapX/Y`,
  `stampCount`.
- Recipe: an `optional<double>` input (`pulse`) read in three places through
  one `share`: a decaying knock `scan` (host flashes on it), the phase speed
  (`dt * (0.10 + 0.55 * knock)` — clicks visibly wind the orbit up), and a
  three-way `zip(pulse, x, y)` fold over `snap_state { float x, y, count; }`
  that adopts the position on the firing tick and holds it otherwise
  (a stateful `scan` generalized over a stamped point). The event is engaged for
  exactly one tick per queued click: the class queues JS clicks up in a
  counter and sets engaged/disengaged at the top of each `tick`.
- Renderer: violet orbit (guide ellipse matches the two lissajous radii, halo
  alpha follows `knock`), amber ghost stamps that fade over 5 s with a dotted
  tether to the live orb, and a corner counter. The stamp equals the orb
  position on the firing step (law L1, same-step values); the wasm stores it
  in float32, so compare with ~1e-6 tolerance when testing.
- Notable: first sketch driven by an optional event input end to end — this
  is the containment pattern (`optional` at the port, plain `T` past it)
  with a visible payoff.

### Stirred Field

- Methods: `tick`, `setPointer(x, y, down)`, `setSwim(x, y)`, `reset`,
  `ballX/Y`, `velX/Y`, `stir`, `angle`, `curl`, `scale`, `speed`, `curX/Y`,
  `audioPitch`, `audioLevel`.
- Recipe, three regions across the three kernel clock tags (Three Clocks'
  crossing mechanics with real agency behind them):
  - **ui region** — the Fling ball from the shared `animations/ball_hold.hpp`
    plus the `stir` scan: `zip(ball_x, ball_y, dt)` folds the ball's speed
    into a smoothed energy. Outputs: `ballX/Y` (user struct), `stir` (double).
    Inputs: `targetX/Y`, `grabbed`, `dt`, `pushX/Y` — the loop's return edge
    (below) — and `swimX/Y`, the host-published swim offset (below).
  - **worker region** — the field environment: `spin`/`breathe`/`sway` phase
    scans (each `1 + 0.9 * stir` boosted) feed `curve` (a merged
    `field_curve { angle, curl }` — merging freed a `parallel` branch for
    the tide), `scale`, one `current` output (a 16-byte inline
    `field_current { x, y }`: two slow lane drifts, `(0.25 + stir)` scaled)
    and the `audio` fold. The stir and the tide both cross ui→worker /
    spirit→worker through `at<..., policy::latest>`.
  - **spirit region** (`clock_tag::audio` — the kernel's third region, same
    tag the kernel audio region stamps for audio; named "spirit" in the lab) — zero
    crossings in: only its own `dt_spirit` input drives three slow phases
    (rates 0.011/0.007/0.004 rad/s → full cycles in ~10–26 min) folded into
    a `spirit_tide { restlessness, warmth, calm, hue }` output. The plane no
    current can enter: nothing the player does reaches it directly.
    The host steps it in `stepTide(dt)` on its own 2 s timer, so the tide
    drifts even when the tab is hidden and rAF stops.
  - **The loop**: ball → stir crosses engine-side; worker folds the current
    from spin/sway + stir + calm; the *return edge* rides ui input ports:
    `tick` steps worker, then copies `current`'s fields into `pushX/pushY`
    for the next ui step (one frame of latency by design; `reset` copies
    them too). A fully in-recipe closure (current crossing back into the
    ball's scan) is not expressible yet — the shell's append-only arena
    cannot spell a recipe-level cycle — see the trap below. `ball_chain`'s
    five-wire overload carries the push plus the swim offset: `ball_in`
    stays 16 bytes (two int16 fields, both 1e-3-scaled) and the fold adds
    `cur * 1e-3 * dt` positionally while free, and rides `swim * 1e-3` on
    the spring target — **but only while free**: under the hand the swim
    field is ignored, so a grab pins the ball exactly where it points
    (pinned by a smoke test).
  - **The swim brain (renderer-side)**: while the ball is free, the renderer
    publishes a small spring-target offset through `setSwim`: darts fire per
    the tide's *restlessness* (gap `2.2–6.5 s × (1.3 − 0.8·r)`, magnitude up
    to ×1.55 at maximum restlessness; 78% of darts point along the field
    heading, rest cut sideways), and a pointer straying within 0.15 canvas
    of the free ball triggers a dodge *away* from it. The offset is target
    geometry, so darts compose with the pointer chase; the brain zeroes it
    on grab for smoothness (the fold gates it anyway).
  - **Audio (opt-in, muted by default)**: the `audio` output is a
    `scene_audio { pitch, level, speed }` fold (three floats — 24 bytes
    would not ride the 16-byte inline path) on the worker cadence; `speed`
    rides along so `parallel` stays within 8 branches, and the tide's
    warmth/calm set the hum's pitch floor bias and the flow's baseline
    (6.6·(0.78–1.02·calm) — visibly slower than the 8.0 era). The renderer
    mounts a "sound off/on" toggle in the canvas corner; arming it creates
    an AudioContext + AudioWorklet loading `web/sonic-worklet.js` (a plain
    consumer, *not* a clockr engine — the lab engine runs on the main
    thread, so the worklet reads folded state forwarded per frame and slews
    internally; a real audio-clock region lives in a host running the
    audio clock). The voice, tuned against `/tmp/reference.wav` (a dusty
    atmosphere in its first half, a deep soft-limited kick in its second
    half — both dialed far back here): a low hum at the folded pitch
    (~147 Hz rest → ~182 Hz stirred) with slow vibrato, a *dusty* flow bed
    (noise crushed to a ~1.4 kHz sample-and-hold grain, then the formant —
    brighter as the field quickens), a **deep kicked heartbeat**: a
    sub sweep 46→33 Hz over ~0.8 s behind a 30 ms soft nose, peaking around
    −22 dB and silent between hits; its period arrives from the page as
    `q = (12.5 − 4.8·restlessness) / (1 + 0.45·stir)` clamped to 4.5–16 s;
    and the one-shot bells for grab / throw / wall hit / swim dart. The
    kick's time counter is monotonic forward (a decrementing counter would
    re-open its body window on the way down and expose the decay envelope
    to negative time — an explosive click mid-cycle; sim-caught).
  - The wasm `tick` steps ui every frame and worker every 3rd frame with a
    compensated dt (`step * 3`); `stepTide` steps spirit on its own timer.
    `reset` warms ui → spirit → worker so both crossings have values to
    sample (every getter is finite on frame zero).
- The three-branch zip wire over the two user-struct axes plus `dt` is
  `std::pair<std::pair<ball_state, ball_state>, double>` (left-nested; one
  wrong alias level silently drops a leaf — the lift then runs with less
  fan-in than the emitter produces).
- Renderer: 880 particles integrate a near-stagnant field (workflow from
  `flow-field.js`; thicker and denser than it started: stroke width
  `max(1.35, w/480)`, base alpha 0.115, wash 0.075 tinted by *warmth*
  between a cool night shade and a warm dusk shade, step floor 0.42 so the
  rest state is the barely-perceptible drift the field was tuned to). The
  ball's obstacle influence also scales the step (`×(1 + 2.4 * influence)`),
  so flow remains visible around the ball even though the ambient flow is
  nearly still; inverse-square influence gives radial repulsion plus a
  tangential swirl scaled by `(0.35 + stir)`, with a hard `CORE_R` the
  particles slide back onto. Fling target ring/tether, ball trail and
  wall-hit ripples come from `fling.js`, all dimmed (halo ~0.08–0.14, trail
  peak 0.12); idle > 5 s wanders the target.
- `#demo=stir` in the URL scripts a grab/swing/release so a capture can hit
  the stirred moment (lab-only hook; inert without the hash).
- Notable: first sketch with a *closed temporal loop across clocks* and the
  first one spanning all three kernel regions with three different host
  cadences (rAF per frame, rAF/3, and a standing 2 s `setInterval` for the
  spirit tide — the tide keeps rolling even when the tab hides the canvas).
  Also the only sketch whose host-renderer logic feeds *input* ports twice
  per frame (swim offset alongside the pointer), and with an opt-in audio
  consumer of folded scene state.

## The two-file contract

A builder agent should own only:

```
animations/<id>.cpp     one embind class, one translation unit
web/<id>.js             export function mount(canvas, lib)
```

The integrator adds the card metadata to `lab.js` and runs the build. Do not
edit shared files from a per-sketch task.

### C++ contract

- C++20, public headers only (`clockr/shell/recipe.hpp`,
  `clockr/kernel/program.hpp`, `clockr/kernel/release_engine.hpp`).
- Include `"lab_common.hpp"` for tolerant `lab::read_double` / `read_bool`
  (NaN for a not-yet-produced value). Prefer `double` getters; embind `int64`
  becomes BigInt in JS.
- One unique class name and one unique `EMSCRIPTEN_BINDINGS` block per file.
  The class is exported exactly as the JS expects. No `main`.
- The default constructor should warm the program: set defaults and step each
  clock at least once so `latest` consumers are not reading monostate.
- `tick(double dt)` takes seconds. Clamp it in the sketch (the existing ones
  use `[0, 0.05]` after a NaN fallback). Set inputs, then step the relevant
  clocks.
- `reset()` is optional but recommended; the card's restart button calls it.
  `kernel::program` is not movable, so use
  `std::optional<kernel::program>` + `.emplace(recipe())` (or a
  `std::unique_ptr`) rather than assignment.
- Keep the exported surface small: a handful of scalars or a few points. The
  renderer can turn them into trails/particles.

Minimal shape:

```cpp
#include "lab_common.hpp"
#include "clockr/kernel/program.hpp"
#include "clockr/kernel/release_engine.hpp"
#include "clockr/shell/recipe.hpp"
#include <emscripten/bind.h>

namespace {
class MySketch {
public:
    MySketch() : m_program { recipe() } {}
    void tick(double dt) {
        m_program.set("dt", dt);
        m_engine.step(m_program, clockr::kernel::clock_tag::ui);
    }
    double x() const { return lab::read_double(m_program, "x"); }
private:
    clockr::kernel::program m_program;
    clockr::kernel::release_engine m_engine;
};
} // namespace

EMSCRIPTEN_BINDINGS(my_sketch) {
    emscripten::class_<MySketch>("MySketch")
        .constructor()
        .function("tick", &MySketch::tick)
        .function("x", &MySketch::x);
}
```

### JS contract

```js
import { createRunner, onPointer, palette } from "../lab-common.js";

export function mount(canvas, lib) {
    const sketch = new lib.MySketch();

    onPointer(canvas, (x, y) => sketch.setPointer(x, y));

    createRunner(canvas, (ctx, dt, w, h) => {
        sketch.tick(dt);
        // read sketch.x(), etc.; draw with Canvas 2D
    });

    return { reset: () => sketch.reset() };
}
```

- `createRunner(canvas, draw)` owns the rAF loop, DPR cap (2), resize,
  offscreen/visibility pause and `prefers-reduced-motion`. It calls `draw` once
  immediately with `dt = 1/60`, then on rAF. Do not add another loop.
- It does not clear the canvas. Clear, or deliberately leave a translucent
  wash for trails.
- Draw signature: `draw(ctx, dt, width, height)`. Canvas is 4:3 via CSS.
- `onPointer` gives normalized, clamped `(x, y, type)` with
  `type` = `pointermove` / `pointerdown` / `leave`.
- `palette`, `clear`, `rng(seed)` and `wasmSupported` are also in
  `lab-common.js`.
- Handle NaN from wasm with a sane fallback (center/0). Keep per-frame work
  modest; no external libraries.

## Adding a sketch

1. Pick an id (kebab-case) and an exported class name (PascalCase).
2. Add `animations/<id>.cpp`; do not edit `CMakeLists.txt` — the glob picks it
   up. Keep the class and binding names unique.
3. Add `web/<id>.js` exporting `mount(canvas, lib)`.
4. Add `{ id, title, tags, blurb }` to `ANIMATIONS` in `lab.js` (shared file).
5. Run `./build.sh`; the card appears at the end.
6. Check the browser. For JS-only changes, a refresh is enough.
7. Add the sketch to the table above if it is meant to stay.

If the sketch needs a generated asset beyond `lab_shell.js/.wasm`, update
`serve.mjs`'s generated-file routing. If you add generated content outside
`tools/website/`, keep it out of any native-build source tarball so that
tarball stays small.

## Testing / acceptance for this lab

- `node --check web/<id>.js` (and `lab.js` if edited).
- `./build.sh` succeeds; all TUs and the link are the real compile check.
- Open the page and check there are no per-card `.lab-error` messages.
- Exercise the restart button for sketches with `reset()`.
- Move the pointer over each interactive sketch.
- Check `prefers-reduced-motion` (draws one static frame) and a hidden tab
  (no runaway work) if you touched the runner.
- A real browser is needed to judge motion. Headless screenshots can capture
  only the first frame; they are useful for layout, not for animation quality.

A quick non-browser wasm smoke test is possible by copying the generated
module out of the `type: module` package and requiring it as CommonJS (the
Emscripten output is UMD), then instantiating each class, calling `tick`
several times and `reset` where present. The class list above is the expected
surface.

## Traps found

- **A recipe accumulator cannot be reassigned.** The arena grows one type per
  combinator, so `chain = chain >> delay(seed)` does not compile (the types
  differ). Variable-length fragments must be recursive (`echo_chain` in
  `ink_echoes.cpp` shows the shape).
- **`program::get` reads *outputs* only.** Reading an input by name through
  `get` throws (as a wasm exception, messageless at the JS boundary). Inputs
  are read back with `program::get_input`.
- **Headless screenshots cannot show motion.** The local chrome-headless-shell
  runs exactly one BeginFrame per page load even with
  `--virtual-time-budget` (rAF fires once); full-page captures are first
  frames, good for layout only. To judge the stirred wake or any motion you
  need a real browser (or a CDP-driven BeginFrame loop).
- **`shell::parallel` is capped at 8 branches (`max_branches`).** A ninth
  chain does not get a nice diagnostic — expect opaque construction failures
  ("no clock anchor", "scan networks have exactly one input", or worse).
  Count the parallel's chains before growing a network; if you run out of
  branches, merge per-axis values into one small inline user struct (see
  `stirred_field.cpp`'s `field_current`).
- **Recipe-level cycles are not expressible.** The arena is append-only with
  backward `node_ref`s, so a closed loop through a scan (ball ← current ←
  stir ← ball) cannot be linearized, even though each *region* alone is
  acyclic and the kernel links could carry it. Close the loop through ui
  input ports in the host `tick` (Stirred Field's `pushX/pushY` pattern);
  costs one frame of return-edge latency.
- **The conjugate trap: shared crossings re-walk subtrees.** A definite
  smell during the parallel hunt: variants that mix *unused* chains into
  the recipe compiled fine but the failure texts shifted with branch count
  and shape in ways that looked semantic (per-region DAGs could all carry
  the same recipe). Before blaming your lambdas for anchor/scan-input
  errors, count the parallel cardinality first.

- **`emscripten::function` takes a function pointer, not a lambda.** The
  temporary placeholder failed to compile until it became a named function.
- **`kernel::program` is not movable.** For reset, reconstruct with
  `std::optional<program>::emplace` (or a `unique_ptr`); do not assign.
- **Latest/queued reads can start as monostate.** Warm up producer ticks before
  consumers read, or the getters return NaN; handle both.
- **Queued crossings deliver one event per consumer tick.** Pair related
  values by setting both before each producer step; the shell's `tick` drains
  one event per consumer step.
- **A slower clock makes `latest` step.** That is the point of the Clock Loom /
  Three Clocks sketches; do not "smooth" it in wasm unless you mean to.
- **`serve.mjs` serves source + `_build`, not `_site`.** Editing `_site` has
  no effect. `_site` is only the optional staged bundle.
- **Generated directories must stay out of the native-build source tarball.**
  The hosting setup excludes `tools/website/`; update that exclude list if
  generated content starts living elsewhere.
- **This is still a throwaway lab.** Do not add deployment/robustness work
  here; port a winning sketch into the real site instead.

## Integration (done)

Cursor Clocks was promoted to the landing page. The durable copies now live
in the site, not here:

- recipe/embind class: `tools/website/animation/src/cursor_clocks.cpp`, built
  by the standalone Emscripten project `tools/website/animation/`;
- renderers + boot: `tools/website/site/animation/` (dial, field, runner,
  `page.js`), staged into `_site/animation/` by `scripts/build.mjs`;
- the canvases are fixed full-viewport layers behind the page content; the
  build omits them entirely when emscripten is unavailable.

The lab keeps the sketch for reference. If you change the concept here and it
wins again, port it across; do not link the lab from the deployed page.
