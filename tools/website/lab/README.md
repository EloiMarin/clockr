# animation lab (throwaway)

A scratch page for the animated-graphics experiments. The lab source is not
part of the site build, but the site's `npm run build` does compile
`lab_shell` and stage every sketch except Cursor Clocks as the deployed
**404 gallery**. The goal is still fast iteration: add a sketch, look at it,
keep or discard it, then port the winner into the real landing page.

`HANDOVER.md` is the agent-facing guide: current sketches, the full
C++/JS contract, traps and the integration direction.

```
tools/website/lab/
  animations/*.cpp       one embind class per sketch (shared lab_shell)
  web/*.js               one mount(canvas, lib) renderer per sketch
  web/page-experience.js page-wide arrangement: dial overlay + ambient field
  web/page-field.js      bottom-canvas flow field, stirred by the dial's balls
  lab-common.js          DPR/resize/visibility/reduced-motion/pointer helpers
  lab.js                 cards + module loading
  index.html, lab.css    the scratch page
  CMakeLists.txt         standalone Emscripten build, pulls the library in
  build.sh               configure + build + stage _site/
  serve.mjs              serve source + the built wasm
  HANDOVER.md            state/contracts/traps for the next agent
  _build/, _site/        generated, gitignored by the parent tools/website
```

## Run it

```bash
cd tools/website/lab
./build.sh
node serve.mjs                 # http://127.0.0.1:4180/
```

If `emcmake` is not on `PATH`:

```bash
nix shell nixpkgs#emscripten -c ./build.sh
```

The first configure fetches tlsf and readerwriterqueue; the debugger/protobuf
dependencies stay off. `serve.mjs` serves the source files directly and the
wasm module from `_build/`, so **JS/CSS edits need only a refresh**; after a
C++ edit run `./build.sh` again (incremental). The build also stages a
self-contained `_site/` copy if you want to hand the whole thing to someone
else.

## Adding a sketch

1. Add `animations/<id>.cpp` with one embind class.
2. Add `web/<id>.js` exporting `mount(canvas, lib)`.
3. Add an entry to `ANIMATIONS` in `lab.js` (id/title/tags/blurb).

### C++ side

Each file is one translation unit in the same module; use a unique class and
`EMSCRIPTEN_BINDINGS` block per file. Do not add `main`.

```cpp
#include "lab_common.hpp"
#include "clockr/kernel/release_engine.hpp"
#include "clockr/kernel/program.hpp"
#include "clockr/shell/recipe.hpp"
#include <emscripten/bind.h>

class my_sketch
{
public:
    my_sketch() : m_program { recipe() } {}
    void tick(double dt) { m_program.set("dt", dt); m_engine.step(m_program, clockr::kernel::clock_tag::ui); }
    double x() const { return lab::read_double(m_program, "x"); }
private:
    clockr::kernel::program m_program;
    clockr::kernel::release_engine m_engine;
};
```

`lab_common.hpp` has tolerant `read_double`/`read_bool` helpers (NaN for a
value that has not been produced yet). Keep the exposed surface to a handful
of scalars; the JS renderer can draw trails/particles from them.

### JS side

```js
import { palette, createRunner, onPointer } from "../lab-common.js";

export function mount(canvas, lib) {
    const sketch = new lib.MySketch();
    onPointer(canvas, (x, y) => sketch.setPointer(x, y));
    createRunner(canvas, (ctx, dt, w, h) => {
        sketch.tick(dt);
        // draw from sketch.x(), sketch.y(), ...
    });
    return { reset: () => sketch.reset() };
}
```

`createRunner` owns the frame loop, device-pixel-ratio cap, resize,
visibility/offscreen pause and `prefers-reduced-motion`. It calls `draw` once
immediately and then on `requestAnimationFrame`.

Guidelines for this lab: avoid per-frame allocations where easy, keep the
canvas 2D and dependency-free, and do not assume the wasm module loaded—the
shared page already handles the feature check and per-card failures.
