// Graph layout: ELK orders the DAG, we snap it into clock lanes.
//
// ELK's own layout partitions cannot guarantee disjoint clock bands: a
// cross-partition edge that runs against the partition order (here, ui → audio)
// or a delay-guarded cycle lets a partition's nodes bleed into another's band.
// So ELK is used for what it is good at — layering and crossing-minimised
// ordering — and the lane geometry is imposed afterwards:
//
//   1. ELK lays the graph out left-to-right (x = causal column, y = order
//      within a column).
//   2. Each clock becomes a horizontal band, stacked audio → ui → worker.
//   3. Within a band, nodes keep ELK's column and are stacked so no two share
//      a cell. A node's height encodes its fan-in, so its input ports sit on
//      the left edge at a constant pitch.
//   4. Edges are routed as cubic beziers: out of the source's right edge (its
//      output port) and into the target's left edge at the fan-in slot.
//   5. Input/output ports become small tabs centered on their band's
//      left/right edges — inputs left, outputs right, each hanging half
//      outside the region to read as its boundary. Each tab sits near its
//      connected node's height; tabs sharing a rail are swept apart
//      vertically.
//
// Columns and rails reserve room for the value boxes that render on the edges
// (see structBoxSize): a wide struct table never overlaps a node.
//
// Pure and DOM-free so it runs in the browser and under node (the layout
// smoke test); the caller supplies the ELK instance.

export const CLOCKS = ["audio", "ui", "worker"];

const PADDING = 24;
const NODE_MIN_W = 116;
const NODE_MIN_H = 44;
const NODE_CHAR_W = 7; // monospace 11px, rounded up
// Constant vertical pitch between a node's input ports, and the unit of its
// height: a node with n inputs is (n + 1) * PORT_PITCH tall. Ports therefore
// never move when the node is dragged — only the edges re-route.
export const PORT_PITCH = 25;
// Horizontal room an edge's value box needs on each side of a column boundary,
// plus the air around it.
const BOX_GAP = 24;
const ROW_GAP = 24;    // vertical gap between stacked nodes within a column
// Vertical gap between bands: cross-clock links sweep through it, so it is
// deliberately generous.
const LANE_GAP = 140;
const BAND_PAD = 20;   // band box padding above the first / below the last row
const PORT_H = 20;     // port tabs are single-line and short
const PORT_TAB_GAP = 8; // minimum gap between tabs sharing a rail
const RAIL_MARGIN = 8;  // viewport air outside the outermost tab halves

// Value-box metrics. An edge carries its source's value as a box at the
// curve's midpoint; a struct value renders as a field table, whose width the
// layout must reserve up front.
export const MAX_STRUCT_FIELDS = 8;
export const BOX_HEADER_H = 30;
export const BOX_ROW_H = 26;
export const BOX_COL_PAD = 10;
const BOX_CHAR_W = 7;       // monospace 11px
const BOX_VALUE_CHARS = 10; // "120.0", "true", "{…}"
const SCALAR_BOX_W = 16 * BOX_CHAR_W + 10; // widest one-line chip

// The size of a struct value's field table: a header row with the type name,
// then one row per field (type · name · value), capped at MAX_STRUCT_FIELDS
// with a trailing "… n more" row.
export function structBoxSize(schema) {
  const all = (schema && schema.fields) || [];
  const fields = all.slice(0, MAX_STRUCT_FIELDS);
  const more = all.length - fields.length;

  const typeW = Math.max(0, ...fields.map((f) => f.kind.length)) * BOX_CHAR_W;
  const nameW = Math.max(0, ...fields.map((f) => f.name.length + 1)) * BOX_CHAR_W;
  const rowsW = typeW + BOX_COL_PAD + nameW + BOX_COL_PAD + BOX_VALUE_CHARS * BOX_CHAR_W;
  const headerW = ((schema && schema.name) || "struct").length * BOX_CHAR_W + 2 * BOX_COL_PAD;

  return {
    w: Math.max(120, rowsW, headerW),
    h: BOX_HEADER_H + (fields.length + (more > 0 ? 1 : 0)) * BOX_ROW_H + 8,
  };
}

// The widest value box any edge of this graph can carry: the widest struct
// table among the shipped type schemas, or a one-line scalar chip.
function maxValueBoxW(types) {
  let w = SCALAR_BOX_W;
  for (const type of types || []) w = Math.max(w, structBoxSize(type).w);
  return w;
}

const isPortNode = (node) => node.kind === "input" || node.kind === "output";

// A port tab shows just its name (falling back to the combinator kind).
function portSize(node) {
  const text = node.port || node.kind;
  // Room for the name plus a short scalar value, which the renderer appends
  // inline (`bpm 120`); longer values truncate with an ellipsis there, full
  // text surviving in the inspector.
  return { w: Math.max(88, (text.length + 7) * NODE_CHAR_W + 16), h: PORT_H };
}

// A node is two lines ("lift #4" / "port tempo"); size the box to the longer
// one so text never spills outside it. Its height is driven by its fan-in: one
// PORT_PITCH per input plus one of padding, so the input ports on its left
// edge (see portPositions) keep a constant pitch.
export function nodeSize(node) {
  const line1 = `${node.label || node.kind} #${node.id}`;
  const line2 = node.port ? `port ${node.port}` : node.kind;
  const chars = Math.max(line1.length, line2.length);
  const fanIn = node.fanIn ? node.fanIn.length : 0;

  return {
    w: Math.max(NODE_MIN_W, chars * NODE_CHAR_W + 16),
    h: Math.max(NODE_MIN_H, (fanIn + 1) * PORT_PITCH),
  };
}

// The ports of a node box: input i on the left edge at (i + 1) * PORT_PITCH
// from the top, the output centered on the right edge. Ports are a function of
// the box alone — inputs left, outputs right — so dragging a node re-routes
// its edges without its ports ever moving.
export function portPositions(box, fanIn) {
  const inputs = [];

  for (let i = 0; i < fanIn; ++i)
    inputs.push({ x: box.x, y: box.y + (i + 1) * PORT_PITCH });

  return { inputs, output: { x: box.x + box.w, y: box.y + box.h / 2 } };
}

function edgeSpecs(graph) {
  const specs = [];
  for (const node of graph.nodes)
    for (const producer of node.fanIn)
      specs.push({ id: `${producer}->${node.id}`, sources: [String(producer)], targets: [String(node.id)] });

  return specs;
}

// ELK pass: left-to-right layered layout, no partitions. Returns a map of
// node id -> {x, y} used purely for column assignment and in-column order.
async function elkOrder(graph, elk, specs) {
  const children = graph.nodes.map((node) => {
    const { w, h } = nodeSize(node);
    return { id: String(node.id), width: w, height: h };
  });

  const result = await elk.layout({
    id: "root",
    layoutOptions: {
      "elk.algorithm": "layered",
      "elk.direction": "RIGHT",
      "elk.edgeRouting": "POLYLINE",
      "elk.spacing.nodeNode": "18",
      "elk.layered.spacing.nodeNodeBetweenLayers": "40",
      "elk.padding": "[top=20,left=20,bottom=20,right=20]",
    },
    children,
    edges: specs,
  });

  return new Map(result.children.map((child) => [Number(child.id), { x: child.x, y: child.y }]));
}

// Cluster the ELK x values (one layer per column, so equal up to float noise)
// into ordered column indices.
function columnsOf(graph, order) {
  const xs = [...new Set(graph.nodes.map((node) => order.get(node.id).x))].sort((a, b) => a - b);
  const centers = [];

  for (const x of xs)
    if (centers.length === 0 || x - centers[centers.length - 1] >= 1)
      centers.push(x);

  const nearest = (x) => {
    let best = 0;
    for (let i = 1; i < centers.length; ++i)
      if (Math.abs(centers[i] - x) < Math.abs(centers[best] - x)) best = i;
    return best;
  };

  return new Map(graph.nodes.map((node) => [node.id, nearest(order.get(node.id).x)]));
}

// An edge always leaves from a's right edge (the output port, vertically
// centered) and always enters through b's left edge at its fan-in slot:
// inputs strictly left, outputs strictly right, data flowing left to right.
// Forward edges are one cubic with horizontal tangents at both ends — the
// S-curve of node-graph editors. Edges that run backwards (delay feedback)
// sweep out to the right of both boxes, drop below them, and come back into
// the target's left port; they are marked `backward` so the renderer can add
// the arrowhead the port convention cannot imply. Exported so the renderer can
// re-route after a node is dragged.
//
// `slot` picks b's attachment port: input (index + 1) of total, at a constant
// PORT_PITCH from the top of b (a tab attaches at its own center instead).
//
// Returns { d, start, end, mid, backward }: the SVG path, the attachment
// points, and the curve's visual midpoint, where its value box sits.
export function routeEdge(a, b, slot) {
  const start = { x: a.x + a.w, y: a.y + a.h / 2 };
  const end = b.tab
    ? { x: b.x, y: b.y + b.h / 2 }
    : { x: b.x, y: b.y + ((slot ? slot.index : 0) + 1) * PORT_PITCH };

  if (b.x >= a.x + a.w) {
    const k = Math.min((end.x - start.x) / 2, 160);
    return {
      d: `M ${start.x} ${start.y} C ${start.x + k} ${start.y}, ${end.x - k} ${end.y}, ${end.x} ${end.y}`,
      start,
      end,
      mid: { x: (start.x + end.x) / 2, y: (start.y + end.y) / 2 },
      backward: false,
    };
  }

  // Return loop: out to the right of both boxes, down below them, then back
  // into the target's left port.
  const right = Math.max(a.x + a.w, b.x + b.w) + 40;
  const below = Math.max(a.y + a.h, b.y + b.h) + ROW_GAP + 16;

  return {
    d: `M ${start.x} ${start.y} C ${start.x + 60} ${start.y}, ${right - 60} ${start.y}, ${right} ${start.y}`
      + ` L ${right} ${below}`
      + ` C ${right - 60} ${below}, ${end.x + 60} ${end.y}, ${end.x} ${end.y}`,
    start,
    end,
    mid: { x: (start.x + right) / 2, y: start.y },
    backward: true,
  };
}

// Returns { boxes: Map<id, {x,y,w,h,node}>, edges: [{from,to,slot,route}],
//           bands: [{clock,y0,y1}], width, height }.
export async function layoutGraph(graph, elk) {
  const specs = edgeSpecs(graph);
  const order = await elkOrder(graph, elk, specs);
  const columnOf = columnsOf(graph, order);

  // Ports live on their band's border, not in its rows. A clock with no
  // internal nodes still gets a (row-less, thin) band so its ports attach
  // somewhere meaningful instead of falling back to flow nodes. Only a graph
  // with no internal nodes at all lays ports out as ordinary nodes.
  const hasInternals = graph.nodes.some((n) => !isPortNode(n));
  const layoutNodes = hasInternals ? graph.nodes.filter((n) => !isPortNode(n)) : graph.nodes;
  const portNodes = hasInternals ? graph.nodes.filter(isPortNode) : [];

  const sizes = new Map(layoutNodes.map((node) => [node.id, nodeSize(node)]));
  const portSizes = new Map(portNodes.map((node) => [node.id, portSize(node)]));
  const boxW = maxValueBoxW(graph.types);
  const colW = Math.max(...[...sizes.values()].map((s) => s.w)) + boxW + BOX_GAP;

  // The input rail hugs the left edge; columns start past the widest input tab
  // plus room for a value box on the tab → first-column edges (the box sits at
  // the edge midpoint).
  const maxInTabW = Math.max(0, ...portNodes.filter((n) => n.kind === "input").map((n) => portSizes.get(n.id).w));
  const leftPad = maxInTabW > 0 ? maxInTabW + RAIL_MARGIN + boxW + BOX_GAP - PADDING : 0;

  // Per band, stack the nodes that share a column, keeping ELK's in-column
  // order and each node's real height (a node's height encodes its fan-in). A
  // band's height is driven by its tallest column.
  const yOf = new Map();
  const contentH = new Map(CLOCKS.map((clock) => [clock, 0]));

  for (const clock of CLOCKS) {
    const byColumn = new Map();

    for (const node of layoutNodes)
      if (node.clock === clock) {
        const column = columnOf.get(node.id);
        if (!byColumn.has(column)) byColumn.set(column, []);
        byColumn.get(column).push(node);
      }

    let tallest = 0;
    for (const column of [...byColumn.keys()].sort((a, b) => a - b)) {
      const list = byColumn.get(column).sort((a, b) => order.get(a.id).y - order.get(b.id).y || a.id - b.id);
      let y = 0;
      for (const node of list) {
        yOf.set(node.id, y);
        y += sizes.get(node.id).h + ROW_GAP;
      }
      tallest = Math.max(tallest, y - ROW_GAP);
    }

    contentH.set(clock, tallest);
  }

  // Bands, top to bottom. A band fits its internal rows and both tab rails:
  // rail tabs stack vertically from a headroom offset (clear of the band
  // label), so the content reserves room for the fuller rail. A clock with
  // only ports still gets a band, sized by its rails alone.
  const RAIL_TOP = 8;
  const railNeed = (n) => (n > 0 ? RAIL_TOP + n * (PORT_H + PORT_TAB_GAP) - PORT_TAB_GAP : 0);

  const bands = [];
  const bandTop = new Map();
  let y = PADDING;

  for (const clock of CLOCKS) {
    const content = contentH.get(clock);
    const clockPorts = portNodes.filter((n) => n.clock === clock);
    if (content === 0 && clockPorts.length === 0) continue;

    const nIn = clockPorts.filter((n) => n.kind === "input").length;
    const height = Math.max(content, railNeed(nIn), railNeed(clockPorts.length - nIn));
    bandTop.set(clock, y);
    bands.push({ clock, y0: y - BAND_PAD, y1: y + height + BAND_PAD });
    y += height + LANE_GAP;
  }

  const boxes = new Map();
  for (const node of layoutNodes) {
    const { w, h } = sizes.get(node.id);
    const x = PADDING + leftPad + columnOf.get(node.id) * colW;
    const boxY = bandTop.get(node.clock) + yOf.get(node.id);

    boxes.set(node.id, { x, y: boxY, w, h, node });
  }

  // Port tabs ride vertical rails centered on their band's left/right edges —
  // inputs left, outputs right, each hanging half outside the region to read
  // as its boundary. Each tab sits near its connected node's height (consumer
  // for inputs, producer for outputs, band center when that box is missing);
  // tabs sharing a rail are swept apart top-to-bottom so they never overlap.
  // The band was sized for the fuller rail, so the sweep always fits. Edges
  // and columns already clear the widest tab half plus a gap on both sides.
  const byId = new Map(graph.nodes.map((n) => [n.id, n]));
  const contentMinX = Math.min(PADDING, ...[...boxes.values()].map((b) => b.x));
  const contentMaxX = Math.max(PADDING, ...[...boxes.values()].map((b) => b.x + b.w));
  const maxOutTabW = Math.max(0, ...portNodes.filter((n) => n.kind === "output").map((n) => portSizes.get(n.id).w));
  const hasInTabs = portNodes.some((n) => n.kind === "input");
  const hasOutTabs = portNodes.some((n) => n.kind === "output");
  const inCX = hasInTabs ? maxInTabW / 2 + RAIL_MARGIN : contentMinX;
  const outCX = hasOutTabs ? contentMaxX + maxOutTabW / 2 + boxW + BOX_GAP : contentMaxX;
  const bandX0 = hasInTabs ? inCX : contentMinX;
  const bandX1 = hasOutTabs ? outCX : contentMaxX;

  for (const band of bands) {
    band.x0 = bandX0;
    band.x1 = bandX1;
  }

  const tabs = [];

  for (const node of portNodes) {
    const band = bands.find((b) => b.clock === node.clock);
    const size = portSizes.get(node.id);
    const side = node.kind === "input" ? "in" : "out";

    let anchorY = null;
    if (node.kind === "input") {
      const consumer = graph.nodes.find((n) => n.fanIn.includes(node.id));
      if (consumer && boxes.has(consumer.id)) {
        const c = boxes.get(consumer.id);
        anchorY = c.y + c.h / 2;
      }
    } else {
      const producer = byId.get(node.fanIn[0]);
      if (producer && boxes.has(producer.id)) {
        const p = boxes.get(producer.id);
        anchorY = p.y + p.h / 2;
      }
    }

    tabs.push({
      id: node.id,
      node,
      side,
      band,
      anchorY: anchorY ?? (band.y0 + band.y1) / 2,
      w: size.w,
      h: size.h,
    });
  }

  // Sweep each rail top-to-bottom: tabs keep anchor order, pushed apart so
  // they never overlap. The band was sized for the fuller rail, so the sweep
  // always fits without leaving it.
  for (const band of bands)
    for (const side of ["in", "out"]) {
      const rail = tabs
        .filter((t) => t.band === band && t.side === side)
        .sort((a, b) => a.anchorY - b.anchorY);

      let cursor = bandTop.get(band.clock) + RAIL_TOP;
      for (const t of rail) {
        t.y = Math.max(t.anchorY - t.h / 2, cursor);
        cursor = t.y + t.h + PORT_TAB_GAP;
      }
    }

  for (const t of tabs)
    boxes.set(t.id, {
      x: (t.side === "in" ? inCX : outCX) - t.w / 2,
      y: t.y,
      w: t.w,
      h: t.h,
      node: t.node,
      tab: t.side,
    });

  const edges = [];
  for (const node of graph.nodes)
    node.fanIn.forEach((producer, index) => {
      const a = boxes.get(producer);
      const b = boxes.get(node.id);
      if (!a || !b) return;

      // The slot records this edge's position in the target's fan-in so the
      // renderer distributes attachments identically on re-route (dragging).
      const slot = { index, total: node.fanIn.length };
      edges.push({ from: producer, to: node.id, slot, route: routeEdge(a, b, slot) });
    });

  const maxX = Math.max(...[...boxes.values()].map((b) => b.x + b.w), 0);
  const maxY = Math.max(...[...boxes.values()].map((b) => b.y + b.h), 0);

  return { boxes, edges, bands, width: maxX + PADDING, height: maxY + PADDING };
}
