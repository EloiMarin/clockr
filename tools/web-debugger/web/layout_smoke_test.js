// Smoke test for the graph layout (layout.js), run under node.
//
// Loads the vendored ELK bundle, lays out a graph shaped like the debugger
// examples (three clocks, cross-clock edges, a delay-guarded cycle), and
// asserts the properties the old hand-rolled grid could not guarantee:
// nodes never overlap, and each clock's nodes sit in their own disjoint band,
// ordered audio → ui → worker.

import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

import { CLOCKS, PORT_PITCH, layoutGraph, structBoxSize } from "./layout.js";

const here = dirname(fileURLToPath(import.meta.url));

// The vendored bundle is a UMD file: load it through a CommonJS require.
const require = createRequire(import.meta.url);
const ELK = require(join(here, "vendor/elk.bundled.js"));

let failed = 0;

function check(what, ok, detail) {
  if (!ok) {
    failed += 1;
    console.log(`FAIL ${what}${detail ? `: ${detail}` : ""}`);
  } else {
    console.log(`ok ${what}`);
  }
}

// A graph with all three clocks, two cross-clock edges, a cycle guarded by a
// delay node (3 → 2 → 3), and a struct type the value boxes are sized from.
const transportIn = {
  id: 1,
  name: "daw.transport_in",
  fields: [
    { name: "block", kind: "audio_block", typeId: 2 },
    { name: "loop", kind: "loop_region", typeId: 3 },
    { name: "is_playing", kind: "bool", typeId: 0 },
    { name: "seek", kind: "std::optional<double>", typeId: 0 },
    { name: "bpm", kind: "double", typeId: 0 },
  ],
};

const graph = {
  types: [transportIn],
  nodes: [
    { id: 0, clock: "ui", kind: "input", label: "input", port: "bpm", fanIn: [] },
    { id: 1, clock: "audio", kind: "internal", label: "lift", port: "", fanIn: [0] },
    { id: 2, clock: "audio", kind: "delay", label: "delay", port: "", fanIn: [3] },
    { id: 3, clock: "audio", kind: "internal", label: "lift", port: "", fanIn: [1, 2] },
    { id: 4, clock: "audio", kind: "output", label: "output", port: "tempo", fanIn: [3] },
    { id: 5, clock: "worker", kind: "scan", label: "scan", port: "", fanIn: [0] },
    { id: 6, clock: "worker", kind: "output", label: "output", port: "triggers", fanIn: [5] },
    { id: 7, clock: "ui", kind: "input", label: "input", port: "gain", fanIn: [] },
    { id: 8, clock: "ui", kind: "output", label: "output", port: "gain_out", fanIn: [7] },
  ],
};

const expectedEdges = graph.nodes.reduce((n, node) => n + node.fanIn.length, 0);

const layout = await layoutGraph(graph, new ELK());

check("every node is boxed", layout.boxes.size === graph.nodes.length, `got ${layout.boxes.size}`);

// No two boxes may overlap.
let overlaps = 0;
const boxes = [...layout.boxes.entries()];

for (let i = 0; i < boxes.length; ++i)
  for (let j = i + 1; j < boxes.length; ++j) {
    const a = boxes[i][1];
    const b = boxes[j][1];
    const eps = 0.5;

    if (a.x < b.x + b.w - eps && b.x < a.x + a.w - eps && a.y < b.y + b.h - eps && b.y < a.y + a.h - eps) {
      overlaps += 1;
      console.log(`  overlap: #${boxes[i][0]} vs #${boxes[j][0]}`);
    }
  }

check("no node boxes overlap", overlaps === 0, `${overlaps} overlaps`);

// Bands: one per non-empty clock, in CLOCKS order, disjoint top-to-bottom.
const bandClocks = layout.bands.map((b) => b.clock);
const expectedBands = CLOCKS.filter((clock) => graph.nodes.some((n) => n.clock === clock));
check("bands follow CLOCKS order", JSON.stringify(bandClocks) === JSON.stringify(expectedBands), JSON.stringify(bandClocks));

let disjoint = true;
for (let i = 1; i < layout.bands.length; ++i)
  if (layout.bands[i].y0 < layout.bands[i - 1].y1) disjoint = false;

check("bands are disjoint", disjoint, JSON.stringify(layout.bands));

// Every internal node sits inside its own clock's band. Port tabs straddle
// their band's border instead (inputs centered on the top edge, outputs on
// the bottom edge) — never inside another band.
let contained = true;
for (const [id, box] of layout.boxes) {
  if (box.tab) continue;
  const band = layout.bands.find((b) => b.clock === box.node.clock);
  if (!band || box.y < band.y0 || box.y + box.h > band.y1) {
    contained = false;
    console.log(`  out of band: #${id} (${box.node.clock}) y=${box.y}`);
  }
}
check("each node lies within its clock band", contained);

// Port tabs hang off their band's edges: inputs center on the left edge,
// outputs on the right edge, each tab straddling inside/outside. Rails align
// across bands (one shared x per side), and tabs stay inside their band
// vertically.
{
  const tabs = [...layout.boxes.values()].filter((b) => b.tab);
  const ins = tabs.filter((b) => b.tab === "in");
  const outs = tabs.filter((b) => b.tab === "out");

  check("every port is a tab", tabs.length === 5, `${tabs.length} tabs`);
  check("bands share one x-span", layout.bands.every((b) => b.x0 === layout.bands[0].x0 && b.x1 === layout.bands[0].x1));

  const centers = (list) => list.map((b) => b.x + b.w / 2);
  check("input tabs center on the left edge", ins.length > 0 && centers(ins).every((c) => c === layout.bands[0].x0));
  check("output tabs center on the right edge", outs.length > 0 && centers(outs).every((c) => c === layout.bands[0].x1));

  const straddles = (b, edge) => b.x < edge && b.x + b.w > edge;
  check(
    "tabs straddle their edge",
    tabs.every((b) => {
      const band = layout.bands.find((x) => x.clock === b.node.clock);
      return band && straddles(b, b.tab === "in" ? band.x0 : band.x1);
    })
  );

  let inBand = true;
  for (const b of tabs) {
    const band = layout.bands.find((x) => x.clock === b.node.clock);
    if (!band || b.y < band.y0 || b.y + b.h > band.y1) {
      inBand = false;
      console.log(`  tab out of band: (${b.node.clock}) y=${b.y}`);
    }
  }
  check("rail tabs stay inside their band", inBand);
}

// Edges: cubic beziers (a forward edge is one cubic; delay feedback adds a
// return loop). Every edge leaves its source's right edge at the centered
// output port and enters its target's left edge at the fan-in port — inputs
// strictly left, outputs strictly right.
check("every edge is routed", layout.edges.length === expectedEdges, `got ${layout.edges.length}/${expectedEdges}`);
check(
  "edges are bezier curves",
  layout.edges.every((e) => e.route.d.startsWith("M") && e.route.d.includes("C")),
);
check(
  "edges exit right at the output port, enter left",
  layout.edges.every((e) => {
    const a = layout.boxes.get(e.from);
    const b = layout.boxes.get(e.to);
    return e.route.start.x === a.x + a.w
      && e.route.start.y === a.y + a.h / 2
      && e.route.end.x === b.x;
  }),
);

// Ports: a node's height encodes its fan-in — one PORT_PITCH per input plus
// one of padding — so inputs attach at a constant pitch down its left edge.
{
  const fanIn2 = layout.boxes.get(3); // fan-in [1, 2]
  check("node height encodes fan-in", fanIn2.h === 3 * PORT_PITCH, `${fanIn2.h}`);

  const lone = layout.boxes.get(1); // fan-in [0]
  check("single-input node keeps a constant pitch", lone.h === Math.max(44, 2 * PORT_PITCH), `${lone.h}`);

  const incoming = layout.edges.filter((e) => e.to === 3);
  const ends = incoming.map((e) => e.route.end.y);
  const want = [fanIn2.y + PORT_PITCH, fanIn2.y + 2 * PORT_PITCH];
  check(
    "fan-in attaches at constant pitch",
    incoming.length === 2 && ends.every((y, i) => Math.abs(y - want[i]) < 1e-6),
    JSON.stringify(ends),
  );

  const single = layout.edges.filter((e) => e.to === 1);
  check(
    "a lone input lands mid-edge (one pitch = h/2)",
    single.length === 1 && Math.abs(single[0].route.end.y - (lone.y + lone.h / 2)) < 1e-6,
  );
}

// Delay feedback is marked backward so the renderer can arrow it.
check("delay feedback is marked backward", layout.edges.filter((e) => e.route.backward).length === 1);

// Column pitch reserves room for the widest value box the graph can carry, so
// a struct table on an edge never overlaps a node.
{
  const internals = [...layout.boxes.values()].filter((b) => !b.tab);
  const xs = [...new Set(internals.map((b) => b.x))].sort((a, b) => a - b);
  const pitch = xs.length > 1 ? xs[1] - xs[0] : 0;
  const maxNodeW = Math.max(...internals.map((b) => b.w));
  const boxW = structBoxSize(transportIn).w;
  check(
    "column pitch reserves value-box room",
    pitch > 0 && pitch - maxNodeW >= boxW + 24,
    `pitch=${pitch} node=${maxNodeW} box=${boxW}`,
  );
}

// Everything fits the reported viewport.
let inBounds = true;
for (const box of layout.boxes.values())
  if (box.x < 0 || box.y < 0 || box.x + box.w > layout.width || box.y + box.h > layout.height) inBounds = false;
check("boxes fit the viewport", inBounds, `${layout.width}x${layout.height}`);

// A clock with only ports (the DAW's ui lane: sources, no compute) still
// gets a thin band, and its ports attach as tabs — never as flow nodes.
{
  const g2 = {
    nodes: [
      { id: 0, clock: "ui", kind: "input", label: "input", port: "bpm", fanIn: [] },
      { id: 1, clock: "ui", kind: "input", label: "input", port: "master", fanIn: [] },
      { id: 2, clock: "audio", kind: "internal", label: "lift", port: "", fanIn: [0, 1] },
      { id: 3, clock: "audio", kind: "output", label: "output", port: "out", fanIn: [2] },
    ],
  };

  const l2 = await layoutGraph(g2, new ELK());
  const uiBand = l2.bands.find((b) => b.clock === "ui");
  check("port-only clock gets a band", !!uiBand, JSON.stringify(l2.bands.map((b) => b.clock)));

  const tabs = [l2.boxes.get(0), l2.boxes.get(1)];
  check(
    "port-only inputs are tabs",
    tabs.every((b) => b && b.tab === "in"),
    JSON.stringify(tabs.map((b) => b && b.tab))
  );
  check(
    "rail tabs center on the band edge and stay in band",
    tabs.every((b) => b.x + b.w / 2 === uiBand.x0 && b.y >= uiBand.y0 && b.y + b.h <= uiBand.y1),
    JSON.stringify(tabs.map((b) => [b.x, b.y]))
  );

  let o2 = 0;
  const all = [...l2.boxes.values()];
  for (let i = 0; i < all.length; ++i)
    for (let j = i + 1; j < all.length; ++j) {
      const a = all[i], b = all[j];
      if (a.x < b.x + b.w - 0.5 && b.x < a.x + a.w - 0.5 && a.y < b.y + b.h - 0.5 && b.y < a.y + a.h - 0.5) o2++;
    }
  check("thin-band layout has no overlaps", o2 === 0, `${o2} overlaps`);
}

if (failed > 0) {
  console.log(`${failed} check(s) failed`);
  process.exit(1);
}

console.log("layout smoke test passed");
