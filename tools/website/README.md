# clockr website

A landing page for `clockr`, a dedicated demo page (DAWmulator first, debugger
on a switch), plus the published documentation and in-browser demos.

The demo page has a Runtime selector: the in-browser wasm DAWmulator and
debugger (staged by the static build) or an externally hosted live pair. A
build that stages the wasm pair defaults to it (or `?runtime=local`); select
Live (or add `?runtime=live`) when the build also has live endpoints. The
in-browser pair needs no server, so the demo works even when the live host or
the network is down.

`npm run build` stages everything into `_site/`, a self-contained static
bundle. Deployment is intentionally kept out of this repository; point any
static host at `_site/`.

## Source forge configuration

The site itself is forge-neutral. Everything it needs is in
**`site.config.json`**:

```json
{
  "name": "clockr",
  "sourceName": "GitHub",
  "sourceUrl": "https://github.com/EloiMarin/clockr",
  "sourceBlobBase": "https://github.com/EloiMarin/clockr/blob/main",
  "mirrorUrl": ""
}
```

- `sourceUrl` drives the **Code** links in the nav, hero and docs/demo chrome
  (the visible label is now always "Code"); `sourceName` stays the forge name
  in configuration.
- `sourceBlobBase` is used for documentation links that leave the published
  set (e.g. `CONTRIBUTING.md`, module READMEs); it must be the blob URL prefix
  for the canonical forge.
- `mirrorUrl` is optional and currently unused by the one-screen landing page.

There is no GitHub assumption in the build scripts beyond these config values.
A future move to another forge is a config change plus a new CI driver, not a
code change. The CI driver only has to run `npm ci` and `npm run build`.

## The in-browser demos

The static build stages the in-browser pair when Emscripten is available:
`/daw/` is the wasm shell and `/debugger/?local=1` is the debugger UI. The
debugger's local transport listens on the same-origin
`clockr.debug.local` BroadcastChannel, which the wasm worker feeds with the
same graph and event shape the native server sends — no sockets, server or
protobuf on that path. It is the demo page's default runtime when the build
contains it.

To point the demo page at an externally hosted live pair instead, build with
their origins:

```bash
DEBUGGER_URL=https://debugger.example.com \
DAW_URL=https://daw.example.com npm run build
```

The `/demo/` page embeds those URLs, so each iframe's `/ws` connection must be
same-origin with the document that proves the live endpoints. The live host
runs the native DAWmulator on 9003 and its embedded debugger server on 9002.

## Prerequisites

- Node.js 20+ and npm.
- Emscripten for the **landing-page animation** (`tools/website/animation`),
  the animation lab, and the **in-browser DAWmulator/debugger** pair. `npm run
  build` skips those with a warning when `emcmake` is not on `PATH`, so the
  static site still builds without them. CI runs the build inside the pinned
  Nix devShell (`nix develop .#ci -c ...`), which provides Node, Emscripten and
  the CMake/Ninja toolchain.

## Install

```bash
cd tools/website
npm install
```

## Preview locally

With Emscripten on `PATH`, this builds and stages the in-browser pair too:

```bash
npm run build            # landing page + demo page + docs + wasm demos
npm run serve            # http://127.0.0.1:4173/demo/
```

The demo page defaults to the in-browser runtime. To also preview against a
native demo running on this machine, start it and build with its two origins
(the same process serves the DAW page and its embedded debugger); select
Runtime → Live or add `?runtime=live`:

```bash
./_build/linux_x64_debug/demo/daw/native/dawmulator_native --open
DEBUGGER_URL=http://127.0.0.1:9002 \
DAW_URL=http://127.0.0.1:9003 npm run build
npm run serve
```

## Published documentation

`npm run build` stages the repository's design record under `/docs`: every
markdown file in `doc/` and `GLOSSARY.md`, rendered to static pages with the
landing page's styling, plus the media assets beside them (for example the
talk slides under `doc/archive/adc-2026/`). The landing page's nav, hero and
Documentation section link to it. `scripts/docs.mjs` does the rendering; the
index page is the reading map (`doc/README.md`) plus a generated, complete
list of the pages.
Cross-links between published pages resolve inside the site; links out of the
documented set (`CONTRIBUTING.md`, module READMEs, examples) fall back to
`sourceBlobBase` with the anchor fragment kept. Everything the site needs is
in `_site/`.

## Landing-page animation

The approved animation ships with the landing page:

- `animation/` — a standalone Emscripten project (like the lab's) that
  compiles `animation/src/cursor_clocks.cpp` into `clockr_page.js/.wasm`.
- `site/animation/` — the page-side modules: `page.js` boots the wasm shell
  and mounts the dial and field; `cursor-clocks.js` and `page-field.js` are
  the renderers (ported from the lab); `runner.js` owns DPR/resize,
  visibility/offscreen pause and reduced motion.
- `scripts/build.mjs` configures/builds the shell and stages everything into
  `_site/animation/`. Without Emscripten it logs a warning and omits the
  layers and their script tag, so the page degrades to its static form.
- The landing page is a single non-scrolling screen (header + hero + three
  actions). The dial and the field are fixed full-viewport canvases *behind*
  the page content (negative z-index, `pointer-events: none`); clicks launch
  a value unless they land on a control.
- Documentation pages get the **field only** (`field-only.js`): ambient
  particles, no dial and no wasm, so reading pages stay calm and fast.
- The **404 page** is the lab gallery: `stageLab()` builds the lab's
  `lab_shell` wasm (all sketches except Cursor Clocks) and stages the
  renderers under `/lab/`; the page is themed with the site tokens and uses
  absolute paths so it works from any missing URL. Without Emscripten it
  falls back to the plain not-found page.
- Design tokens live at the top of `site/style.css` ("Terminal Dusk": blue-black
  ground, icy ink, lime signal, violet secondary). Type is self-hosted under
  `site/fonts/` — Bricolage Grotesque for display, Instrument Sans for body,
  IBM Plex Mono for labels/code. The animation renderers are recoloured to the
  same palette (`soft` in `cursor-clocks.js`, `COLORS` in `page-field.js`).

## Animation lab

`tools/website/lab/` is a throwaway, non-deployed page with eleven wasm-driven
sketches in cards (Signal Bloom, Clock Loom, Queued Ripples, Shape of a Hold,
Three Clocks One Brush, Deterministic Replay, Flow Field, Fling, Ink Echoes,
Knock & Stamp, Stirred Field). Cursor Clocks was promoted out of the cards:
the durable copy is the landing-page animation above. The lab still has its own
standalone CMake/Emscripten build — experiment there, then port the winner
into `tools/website/animation/` and `site/animation/`.

```bash
cd tools/website/lab
nix shell nixpkgs#emscripten -c ./build.sh
node serve.mjs                 # http://127.0.0.1:4180/
```

## Files

```
tools/website/
  site.config.json          public name/copy + source URLs
  animation/
    CMakeLists.txt          standalone Emscripten shell for the landing page
    src/cursor_clocks.cpp   the dial recipe (ported from the lab)
  site/
    index.template.html     landing page (placeholders filled by build.mjs)
    demo.template.html      demo page at /demo/ (runtime + debugger switches)
    style.css               site, demo, 404 and docs styling (Terminal Dusk tokens)
    clockr-mark.svg        standalone multi-clock icon and favicon
    clockr-logo.svg        horizontal lockup with an outlined wordmark
    fonts/                  self-hosted latin woff2: Bricolage Grotesque,
                            Instrument Sans, IBM Plex Mono
    404.template.html       not-found page + the lab gallery (build fills it in)
    animation/              page.js, renderers and canvas runner (staged as-is)
  scripts/
    build.mjs               render landing + demo + docs, build/stage the animation and wasm demos
    docs.mjs                render doc/ + GLOSSARY into _site/docs
    serve.mjs               local static preview of _site/
    mime.mjs                Content-Type / Cache-Control mapping
    lib.mjs                 shared paths/config/run helpers
  lab/                      throwaway wasm animation lab (not deployed)
  package.json              marked + highlight.js
  package-lock.json         pinned dependency tree
  _site/                    generated, gitignored
  .cache/                   generated wasm build tree, gitignored
```
