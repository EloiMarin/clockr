# Vendored JavaScript

Third-party browser bundles committed as-is so the debugger page runs offline
with no bundler. Do not edit these files by hand; upgrade by re-downloading
and re-recording the hash below.

## `elk.bundled.js`

- **Package**: [`elkjs`](https://github.com/kieler/elkjs) — the Eclipse Layout
  Kernel compiled to JavaScript; used for graph layout (layered algorithm with
  layout partitions, one per clock lane).
- **Version**: `0.12.0`
- **Source**: <https://registry.npmjs.org/elkjs/-/elkjs-0.12.0.tgz> →
  `package/lib/elk.bundled.js`
- **SHA-256**: `1222e44f953ce7746af23801e723708f8e6f436b8b377a6a5fc7552f34a307b3`
- **License**: EPL-2.0 OR GPL-3.0-or-later (see `ELK-LICENSE`). It is bundled
  unmodified and invoked as a library; clockr itself remains MIT (see the
  repository `LICENSE.md`, which carries the third-party notice).
- **Shape**: a single UMD file exposing the global `ELK`. The `bundled` build
  runs layout on the main thread (no separate worker file); our graphs are
  tens of nodes, so that is fine. If a graph ever blocks the UI, the split
  `elk-api.js` + `elk-worker.min.js` pair is the upgrade path.

Upgrade procedure: download the chosen version, copy `lib/elk.bundled.js`
here, update the version/source/hash above, and re-run the debugger's node
layout test (it loads this file).
