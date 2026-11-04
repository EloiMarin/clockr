// The debugger web UI.
//
// Consumes the protobuf wire protocol (schema.js) over a WebSocket, or the
// equivalent JavaScript frames over a BroadcastChannel with `?local=1`, keeps
// a client-side history of evaluation events, and renders:
//
//   * the network as a graph of combinator-labelled nodes, one region per
//     clock: inputs on a node's left edge, outputs on its right, and the value
//     an edge carries as a box at the curve's midpoint;
//   * a wall-clock timeline scrubber: per-thread lanes of tick marks on a
//     time axis, with the present pinned to the visual center;
//   * a cursor over (tick, event) that highlights the node being evaluated
//     and shows the value it produced on its output.
//
// Recording is client-side: every frame is appended to the local history, so
// browsing never needs the server to hold state. "Live" follows the newest
// tick and captures; leaving live mode (any gesture, button, or the checkbox)
// pauses capture too, so the visualized ticks cannot be evicted while you
// inspect them.

import { decodeFrame, encodePause, encodeResume, encodeSelectExample } from "./schema.js";
import {
  BOX_COL_PAD,
  BOX_HEADER_H,
  BOX_ROW_H,
  MAX_STRUCT_FIELDS,
  layoutGraph,
  portPositions,
  routeEdge,
  structBoxSize,
} from "./layout.js";

// --- configuration ----------------------------------------------------------

// Cap the client-side history so a long-running debug session cannot grow
// without bound. Old ticks are evicted from the front.
const MAX_TICKS = 4000;

// --- state ------------------------------------------------------------------

const state = {
  graph: null,        // { nodes, sharingPoints, inputs, outputs, types }
  schemasById: null,  // Map<type_id, TypeSchema>, from the graph description
  layout: null,       // { nodes: Map<id, box>, edges: [...], width, height }
  ticks: [],          // closed region ticks, in close order
  tickIndex: -1,      // selected tick
  eventIndex: -1,     // selected event within the tick
  live: true,
  dropped: 0,
  originNs: null,     // wall-clock origin for the axis legend (first tick)
  // The timeline viewport: centerNs is the wall-clock time pinned to the
  // visual center; pxPerSec is the zoom level.
  view: { centerNs: null, pxPerSec: 300 },
  // Ticks that are currently open (region_step_begin seen, no end): keyed
  // "thread|epoch". Node events accumulate here until the tick closes.
  open: new Map(),
};

const els = {
  status: document.getElementById("status"),
  dropped: document.getElementById("dropped"),
  live: document.getElementById("live"),
  example: document.getElementById("example"),
  graph: document.getElementById("graph"),
  timeline: document.getElementById("timeline"),
  eventList: document.getElementById("event-list"),
  cursorLabel: document.getElementById("cursor-label"),
  prevEvent: document.getElementById("prev-event"),
  prevTick: document.getElementById("prev-tick"),
  nextTick: document.getElementById("next-tick"),
  nextEvent: document.getElementById("next-event"),
  prevRegionTick: document.getElementById("prev-region-tick"),
  nextRegionTick: document.getElementById("next-region-tick"),
  zoomIn: document.getElementById("zoom-in"),
  zoomOut: document.getElementById("zoom-out"),
  graphZoomIn: document.getElementById("graph-zoom-in"),
  graphZoomOut: document.getElementById("graph-zoom-out"),
  graphFit: document.getElementById("graph-fit"),
  graphTooltip: document.getElementById("graph-tooltip"),
  inspectorValue: document.getElementById("inspector-value"),
  inspectorNode: document.getElementById("inspector-node"),
};

// The graph viewport transform: content is drawn once in graph coordinates
// and pan/zoom is a translate+scale on the viewport group, so browsing never
// re-lays-out. Node drags adjust the stored boxes and re-render.
const graphView = { tx: 0, ty: 0, k: 1 };
const GRAPH_MIN_K = 0.2;
const GRAPH_MAX_K = 4;
let graphGesturesAttached = false;

// --- transport: WebSocket, or the in-browser BroadcastChannel --------------

let ws = null;

// The local transport is for the wasm DAWmulator: its worker broadcasts the
// same graph/event protocol on a same-origin BroadcastChannel, so the demo
// page can show the debugger connected to a program that never leaves the
// browser. `?local=1` selects it; the page served by a native debug server
// keeps using /ws.
const localDebug = new URLSearchParams(location.search).get("local") === "1"
  || globalThis.CLOCKR_DEBUG_LOCAL === true;

let localChannel = null;
let localConnected = false;
let localHelloTimer = null;

// The debugger is normally served by its own server, so /ws is same-origin.
// A page hosted elsewhere can set window.DEBUGGER_WS_URL in config.js or
// override it per visit with ?ws=. Deriving wss from the page protocol keeps
// an HTTPS deployment from attempting a mixed-content ws:// connection.
function websocketUrl() {
  const query = new URLSearchParams(location.search).get("ws");
  if (query) return query;

  const override = globalThis.DEBUGGER_WS_URL;

  if (typeof override === "string" && override.length > 0)
    return override;

  const scheme = location.protocol === "https:" ? "wss:" : "ws:";
  return `${scheme}//${location.host}/ws`;
}

function requestLocalHello() {
  if (localChannel === null) return;

  localChannel.postMessage({ type: "hello" });

  clearTimeout(localHelloTimer);
  localHelloTimer = setTimeout(() => {
    if (localConnected) return;
    setStatus("waiting for the in-browser DAWmulator", "");
    requestLocalHello();
  }, 1500);
}

function connectLocal() {
  if (localChannel !== null) {
    if (!localConnected) requestLocalHello();
    return;
  }

  if (typeof BroadcastChannel !== "function") {
    setStatus("in-browser debugger unavailable", "err");
    return;
  }

  localChannel = new BroadcastChannel("clockr.debug.local");

  localChannel.onmessage = (event) => {
    const message = event.data;

    if (message === null || typeof message !== "object")
      return;

    if (message.type === "welcome") {
      localConnected = true;
      clearTimeout(localHelloTimer);
      setStatus("connected (in-browser)", "ok");
      sendCapture(state.live);
    } else if (message.type === "examples") {
      onExamples(message.examples);
    } else if (message.type === "graph") {
      onGraph(message.graph);
    } else if (message.type === "batch") {
      onBatch(message.batch);
    }
  };

  setStatus("connecting…", "");
  requestLocalHello();
}

function sendCapture(on) {
  if (localDebug) {
    if (localChannel !== null)
      localChannel.postMessage({ type: on ? "resume" : "pause" });
    return;
  }

  if (ws && ws.readyState === WebSocket.OPEN)
    ws.send(on ? encodeResume() : encodePause());
}

function sendSelectExample(name) {
  if (localDebug) {
    if (localChannel !== null)
      localChannel.postMessage({ type: "selectExample", name });
    return;
  }

  if (ws && ws.readyState === WebSocket.OPEN)
    ws.send(encodeSelectExample(name));
}

function connect() {
  if (localDebug) {
    connectLocal();
    return;
  }

  // Idempotent: never open a second socket while one is open or connecting.
  if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return;

  const url = websocketUrl();
  setStatus("connecting…", "");

  ws = new WebSocket(url);
  ws.binaryType = "arraybuffer";

  ws.onopen = () => {
    if (state.live) {
      setStatus("connected", "ok");
      // A pause may have outlived the previous socket; re-assert the live
      // state so a reconnect never resumes a paused recorder silently, nor
      // leaves a live one paused.
      ws.send(encodeResume());
    } else {
      setStatus("paused", "");
    }
  };
  ws.onclose = () => {
    // Paused: the connection is irrelevant — no retry, no error. Otherwise
    // keep retrying until the server is back.
    if (!state.live) {
      setStatus("paused", "");
      return;
    }
    setStatus("disconnected — retrying", "err");
    setTimeout(() => { if (state.live) connect(); }, 1000);
  };
  ws.onerror = () => { if (state.live) setStatus("connection error", "err"); };

  ws.onmessage = (event) => {
    const frame = decodeFrame(new Uint8Array(event.data));
    if (frame.graph) onGraph(frame.graph);
    else if (frame.examples) onExamples(frame.examples);
    else if (frame.batch) onBatch(frame.batch);
  };
}

function setStatus(text, cls) {
  els.status.textContent = text;
  els.status.className = "status" + (cls ? " " + cls : "");
}

// --- example list -----------------------------------------------------------

// The server's live-example set: an embedded host registers one example per
// running instance, so the toolbar selector is rebuilt from this list rather
// than the static fallback in index.html. The server's `active` wins, so every
// client agrees on what is live.
function onExamples(examples) {
  const names = examples.names || [];
  const existing = [...els.example.options].map((option) => option.value);
  const same = existing.length === names.length && existing.every((v, i) => v === names[i]);

  if (!same) {
    els.example.replaceChildren(...names.map((name) => {
      const option = document.createElement("option");
      option.value = name;
      option.textContent = name;
      return option;
    }));
  }

  if (examples.active && els.example.value !== examples.active)
    els.example.value = examples.active;
}

// --- graph model ------------------------------------------------------------

// The layout request generation: a new graph description supersedes any
// layout still in flight, so a fast example switch can never paint stale
// boxes over the new network.
let layoutRequest = 0;

function onGraph(graph) {
  state.graph = graph;
  state.schemasById = new Map((graph.types || []).map((t) => [t.id, t]));
  state.layout = null; // stale boxes must not survive into the new graph

  const request = ++layoutRequest;

  // The graph description precedes every batch, so a (re)connect resets the
  // view: the history belongs to the previous connection. The zoom level is
  // kept; the center time restarts with the new history.
  state.ticks = [];
  state.open.clear();
  state.tickIndex = -1;
  state.eventIndex = -1;
  state.originNs = null;
  state.view.centerNs = null;

  // A mid-session graph means an example switch (see the toolbar selector):
  // the dropdown follows the server so every client agrees on what is live,
  // including clients that connect while another example runs.
  if (graph.name && els.example.querySelector(`option[value="${graph.name}"]`))
    els.example.value = graph.name;

  els.graph.replaceChildren(); // drop the previous graph until layout lands

  // Layout is asynchronous; render once it lands. Until then the panel shows
  // the empty state rather than misplaced boxes.
  layoutGraph(graph, elk())
    .then((layout) => {
      if (request !== layoutRequest) return; // superseded by a newer graph
      state.layout = layout;
      renderGraph();
      fitView();
      updateCursorView();
    })
    .catch((error) => {
      if (request !== layoutRequest) return;
      console.error("graph layout failed:", error);
    });

  renderTimeline();
}

// The ELK instance is created lazily from the vendored global so a page that
// forgot the <script> tag fails with a clear message rather than a TypeError.
let elkEngine = null;

function elk() {
  if (elkEngine === null) {
    if (typeof ELK === "undefined")
      throw new Error("ELK not loaded: tools/web-debugger/web/vendor/elk.bundled.js must precede app.js");

    elkEngine = new ELK();
  }
  return elkEngine;
}

const NODE_FILL = { audio: "#1d3a24", ui: "#1b2b4d", worker: "#33204d" };
const NODE_STROKE = { audio: "#9ece6a", ui: "#7aa2f7", worker: "#bb9af7" };

// The SVG namespace, shared by every factory below.
const NS = "http://www.w3.org/2000/svg";

// A port dot marks where an edge attaches to a node or tab. Its position is a
// function of the box alone (see portPositions), so it never moves when a node
// is dragged.
function appendPortDot(parent, x, y, color) {
  const dot = document.createElementNS(NS, "circle");
  dot.setAttribute("cx", x);
  dot.setAttribute("cy", y);
  dot.setAttribute("r", 3);
  dot.setAttribute("class", "port-dot");
  dot.setAttribute("fill", color);
  dot.setAttribute("aria-hidden", "true");
  parent.appendChild(dot);
}

function appendText(parent, x, y, cls, text, anchor) {
  const el = document.createElementNS(NS, "text");
  el.setAttribute("x", x);
  el.setAttribute("y", y);
  el.setAttribute("class", cls);
  if (anchor) el.setAttribute("text-anchor", anchor);
  el.textContent = text;
  parent.appendChild(el);
  return el;
}

function renderGraph() {
  const { layout, graph } = state;
  const svg = els.graph;

  svg.replaceChildren();

  // The graph as a whole is one labelled image; each node carries its own
  // name (below), so a screen reader announces the network and its parts
  // rather than a void. Tick values stay out of it — they change every frame
  // and would flood an AT user.
  const clocks = layout.bands.map((b) => b.clock);
  svg.setAttribute("role", "img");
  svg.setAttribute(
    "aria-label",
    `Signal network: ${layout.boxes.size} nodes across the ${clocks.join(", ")} clocks`,
  );

  // Everything lives in one transform group: pan/zoom is a transform on it,
  // so the graph is drawn once per layout and never re-laid-out while
  // navigating.
  const viewport = document.createElementNS(NS, "g");
  viewport.setAttribute("id", "viewport");
  svg.appendChild(viewport);

  // An arrowhead for backward (delay-feedback) edges only: the port
  // convention (inputs left, outputs right) shows a forward edge's direction,
  // but a loop runs against it and needs an explicit arrow.
  const defs = document.createElementNS(NS, "defs");
  const marker = document.createElementNS(NS, "marker");
  marker.setAttribute("id", "edge-arrow");
  marker.setAttribute("viewBox", "0 0 10 10");
  marker.setAttribute("refX", "9");
  marker.setAttribute("refY", "5");
  marker.setAttribute("markerWidth", "7");
  marker.setAttribute("markerHeight", "7");
  marker.setAttribute("orient", "auto-start-reverse");
  const arrow = document.createElementNS(NS, "path");
  arrow.setAttribute("d", "M 0 0 L 10 5 L 0 10 z");
  arrow.setAttribute("class", "edge-arrow");
  marker.appendChild(arrow);
  defs.appendChild(marker);
  viewport.appendChild(defs);

  // One region container per clock, tinted by clock, always enclosing its
  // members (measured from the laid-out nodes, never a fixed height). Port
  // tabs center on the left/right edges, hanging half outside the region.
  for (const band of layout.bands) {
    const rect = document.createElementNS(NS, "rect");
    rect.setAttribute("x", band.x0);
    rect.setAttribute("y", band.y0);
    rect.setAttribute("width", band.x1 - band.x0);
    rect.setAttribute("height", band.y1 - band.y0);
    rect.setAttribute("rx", 18);
    rect.setAttribute("class", `lane-band band-${band.clock} dim`);
    rect.setAttribute("aria-hidden", "true");
    viewport.appendChild(rect);

    appendText(
      viewport,
      band.x0 + 20,
      band.y0 + 20,
      `band-label band-label-${band.clock} dim`,
      band.clock,
    );
  }

  const sharing = new Map((graph.sharingPoints || []).map((s) => [`${s.from}->${s.to}`, s]));

  for (const edge of layout.edges) {
    // Re-route from the current boxes so edges follow a dragged node (the
    // stored route is only the initial one).
    const a = layout.boxes.get(edge.from);
    const b = layout.boxes.get(edge.to);
    if (!a || !b) continue;

    edge.route = routeEdge(a, b, edge.slot);

    // A crossing link is dashed and hue-coded by its policy (latest/queued);
    // a same-clock signal is solid. The legend names the colours.
    const crossing = sharing.get(`${edge.from}->${edge.to}`);
    const cls = ["edge", "dim", crossing ? `link ${crossing.policy}` : "signal"];
    if (edge.route.backward) cls.push("backward");

    const path = document.createElementNS(NS, "path");
    path.setAttribute("class", cls.filter(Boolean).join(" "));
    path.setAttribute("d", edge.route.d);
    path.setAttribute("aria-hidden", "true");
    path.dataset.from = edge.from;
    path.dataset.to = edge.to;
    viewport.appendChild(path);
  }

  for (const [id, box] of layout.boxes) {
    const group = document.createElementNS(NS, "g");
    group.dataset.node = id;
    group.setAttribute("class", "dim");

    // A node's accessible name: combinator, id, clock, and port if it has one.
    const description =
      `${box.node.label || box.node.kind} #${box.node.id} on the ${box.node.clock} clock`
      + (box.node.port ? `, port ${box.node.port}` : "");
    group.setAttribute("aria-label", description);

    const title = document.createElementNS(NS, "title");
    title.textContent = description;
    group.appendChild(title);

    const stroke = NODE_STROKE[box.node.clock] || "#555";

    const rect = document.createElementNS(NS, "rect");
    rect.setAttribute("x", box.x);
    rect.setAttribute("y", box.y);
    rect.setAttribute("width", box.w);
    rect.setAttribute("height", box.h);
    rect.setAttribute("rx", box.tab ? 4 : 8);
    // Tabs keep node-box so highlights, value boxes and drag all keep working;
    // port-box restyles them as boundary markers.
    rect.setAttribute("class", box.tab ? "node-box port-box" : "node-box");
    rect.setAttribute("fill", NODE_FILL[box.node.clock] || "#222");
    rect.setAttribute("stroke", stroke);
    group.appendChild(rect);

    if (box.tab) {
      appendText(
        group,
        box.x + box.w / 2,
        box.y + box.h / 2 + 4,
        "port-label",
        box.node.port || box.node.kind,
        "middle",
      );
      // The tab's single port sits on its inner edge (an input tab's output,
      // an output tab's input).
      const edgeX = box.tab === "in" ? box.x + box.w : box.x;
      appendPortDot(group, edgeX, box.y + box.h / 2, stroke);
    } else {
      // The label block is centered: a tall node's ports run the full left
      // edge, so a top-anchored label would sit beside the top port.
      const cy = box.y + box.h / 2;
      appendText(
        group,
        box.x + box.w / 2,
        cy - 8,
        "node-label",
        `${box.node.label || box.node.kind} #${box.node.id}`,
        "middle",
      );
      appendText(
        group,
        box.x + box.w / 2,
        cy + 10,
        "node-port",
        box.node.port ? `port ${box.node.port}` : box.node.kind,
        "middle",
      );

      // Inputs on the left edge at a constant pitch, the output centered on
      // the right edge — the convention that makes a drag re-route only edges.
      const ports = portPositions(box, box.node.fanIn.length);
      for (const port of ports.inputs) appendPortDot(group, port.x, port.y, stroke);
      appendPortDot(group, ports.output.x, ports.output.y, stroke);
    }

    viewport.appendChild(group);
  }

  // Value boxes live in their own layer so they can be redrawn without a full
  // re-render.
  const valueLayer = document.createElementNS(NS, "g");
  valueLayer.setAttribute("id", "values");
  viewport.appendChild(valueLayer);

  applyView();
}

// --- graph viewport: pan, zoom, fit, node drag ------------------------------

function viewportEl() {
  return els.graph.querySelector("#viewport");
}

function applyView() {
  const viewport = viewportEl();
  if (viewport)
    viewport.setAttribute("transform", `translate(${graphView.tx} ${graphView.ty}) scale(${graphView.k})`);
}

function graphSize() {
  const rect = els.graph.getBoundingClientRect();
  return { w: rect.width || 800, h: rect.height || 400 };
}

function clampK(k) {
  return Math.min(GRAPH_MAX_K, Math.max(GRAPH_MIN_K, k));
}

// Center and scale the content to fit the panel.
function fitView() {
  if (!state.layout) return;

  const { w, h } = graphSize();
  const k = clampK(Math.min(w / state.layout.width, h / state.layout.height) * 0.96);

  graphView.k = k;
  graphView.tx = (w - state.layout.width * k) / 2;
  graphView.ty = (h - state.layout.height * k) / 2;
  applyView();
}

// Zoom around a point in panel coordinates (default: the panel center).
function zoomGraphBy(factor, px, py) {
  const { w, h } = graphSize();
  const cx = px ?? w / 2;
  const cy = py ?? h / 2;

  const next = clampK(graphView.k * factor);
  const ratio = next / graphView.k;

  graphView.tx = cx - (cx - graphView.tx) * ratio;
  graphView.ty = cy - (cy - graphView.ty) * ratio;
  graphView.k = next;
  applyView();
}

// Panel-local coordinates of a pointer/wheel event.
function localPoint(event) {
  const rect = els.graph.getBoundingClientRect();
  return { x: event.clientX - rect.left, y: event.clientY - rect.top };
}

function attachGraphGestures() {
  if (graphGesturesAttached) return;
  graphGesturesAttached = true;

  const svg = els.graph;
  const pointers = new Map();
  let drag = null; // { kind: "pan" | "node", ... }
  let pinch = null;

  const nodeAt = (event) => event.target.closest && event.target.closest("g[data-node]");

  svg.addEventListener("pointerdown", (event) => {
    pointers.set(event.pointerId, localPoint(event));

    if (pointers.size === 1) {
      const nodeGroup = nodeAt(event);
      const id = nodeGroup ? Number(nodeGroup.dataset.node) : null;

      drag = id === null
        ? { kind: "pan", start: localPoint(event), tx: graphView.tx, ty: graphView.ty }
        : { kind: "node", id, start: localPoint(event), box: state.layout?.boxes.get(id) };
      pinch = null;

      if (drag.kind === "node") {
        // Capture keeps the drag alive outside the graph; not all
        // environments implement it (jsdom), so failure is non-fatal.
        try {
          svg.setPointerCapture(event.pointerId);
        } catch (_) { /* no pointer capture */ }

        event.preventDefault();
      }
    } else if (pointers.size === 2) {
      const [a, b] = [...pointers.values()];
      pinch = { dist: Math.hypot(a.x - b.x, a.y - b.y) };
      drag = null;
    }
  });

  svg.addEventListener("pointermove", (event) => {
    if (!pointers.has(event.pointerId)) return;
    const point = localPoint(event);
    pointers.set(event.pointerId, point);

    if (pinch && pointers.size >= 2) {
      const [a, b] = [...pointers.values()];
      const dist = Math.hypot(a.x - b.x, a.y - b.y);
      if (pinch.dist > 0 && dist > 0) zoomGraphBy(dist / pinch.dist, (a.x + b.x) / 2, (a.y + b.y) / 2);
      pinch.dist = dist;
      return;
    }

    if (!drag) return;

    if (drag.kind === "pan") {
      graphView.tx = drag.tx + (point.x - drag.start.x);
      graphView.ty = drag.ty + (point.y - drag.start.y);
      applyView();
      return;
    }

    // Node drag: move the stored box and redraw so incident edges follow.
    // The node is clamped to its clock band so a drag cannot break the lane
    // guarantee; port tabs slide along their rail instead (x pinned).
    const box = drag.box;
    if (!box || !state.layout) return;

    const dx = (point.x - drag.start.x) / graphView.k;
    const dy = (point.y - drag.start.y) / graphView.k;
    drag.start = point;

    const band = state.layout.bands.find((b) => b.clock === box.node.clock);
    if (box.tab) {
      if (band) box.y = Math.min(Math.max(box.y + dy, band.y0), band.y1 - box.h);
    } else {
      box.x = Math.max(0, box.x + dx);
      box.y += dy;
      if (band) box.y = Math.min(Math.max(box.y, band.y0), band.y1 - box.h);
    }

    renderGraph();
    updateCursorView();
  });

  const endPointer = (event) => {
    pointers.delete(event.pointerId);
    if (pointers.size < 2) pinch = null;
    if (pointers.size === 0) drag = null;
  };

  svg.addEventListener("pointerup", endPointer);
  svg.addEventListener("pointercancel", endPointer);

  svg.addEventListener("wheel", (event) => {
    if (!state.layout) return;
    event.preventDefault();

    const point = localPoint(event);

    // Horizontal swipe pans, vertical zooms around the pointer (as on the
    // timeline).
    if (event.deltaX) {
      graphView.tx -= event.deltaX;
  // A re-render drops the hover highlight classes, and a fresh graph can leave
  // a hover pointing at nothing, so re-apply — or clear — the isolation the
  // pointer still implies.
  if (hover && !hoverElements(hover).some(Boolean)) hover = null;
  if (hover) applyHover();

  applyView();
}

    const factor = event.deltaY < 0 ? 1.1 : 1 / 1.1;
    if (event.deltaY) zoomGraphBy(factor, point.x, point.y);
  }, { passive: false });

  els.graphZoomIn.onclick = () => zoomGraphBy(1.25);
  els.graphZoomOut.onclick = () => zoomGraphBy(1 / 1.25);
  els.graphFit.onclick = () => fitView();
}

// --- hover isolation + edge tooltip -----------------------------------------

// Hovering a node isolates it and the edges it feeds or consumes; hovering an
// edge (or the value box it carries) isolates the edge, the nodes at both ends,
// and the box. Everything else dims, so the hovered path reads at a glance.
// Delegated from the SVG root so a re-render never needs to re-attach.
let hover = null; // { kind: "node", id } | { kind: "edge", from, to }

function attachGraphHover() {
  const svg = els.graph;

  svg.addEventListener("pointerover", (event) => {
    const next = hoverTarget(event);
    if (sameHover(hover, next)) return;
    hover = next;
    applyHover();
  });

  svg.addEventListener("pointermove", (event) => {
    if (hover && hover.kind === "edge") positionTooltip(event);
  });

  svg.addEventListener("pointerleave", () => {
    hover = null;
    applyHover();
  });
}

// The hover target under the pointer: an edge or value box wins (they draw on
// top of the nodes), then a node group, then nothing.
function hoverTarget(event) {
  const target = event.target;
  if (!target || !target.closest) return null;

  const edgeEl = target.closest(".edge, .value-box");
  if (edgeEl && edgeEl.dataset.from !== undefined)
    return { kind: "edge", from: Number(edgeEl.dataset.from), to: Number(edgeEl.dataset.to) };

  const nodeGroup = target.closest("g[data-node]");
  if (nodeGroup) return { kind: "node", id: Number(nodeGroup.dataset.node) };

  return null;
}

function sameHover(a, b) {
  if (!a || !b) return a === b;
  if (a.kind !== b.kind) return false;
  return a.kind === "node" ? a.id === b.id : a.from === b.from && a.to === b.to;
}

// The edge path and the value box for one (from, to) pair.
function edgeElements(from, to) {
  const svg = els.graph;
  return [
    svg.querySelector(`.edge[data-from="${from}"][data-to="${to}"]`),
    svg.querySelector(`.value-box[data-from="${from}"][data-to="${to}"]`),
  ];
}

// The elements a hover isolates.
function hoverElements(id) {
  const svg = els.graph;
  const out = [];

  if (id.kind === "node") {
    out.push(svg.querySelector(`g[data-node="${id.id}"]`));
    for (const edge of state.layout ? state.layout.edges : [])
      if (edge.from === id.id || edge.to === id.id) out.push(...edgeElements(edge.from, edge.to));
  } else {
    out.push(...edgeElements(id.from, id.to));
    out.push(svg.querySelector(`g[data-node="${id.from}"]`));
    out.push(svg.querySelector(`g[data-node="${id.to}"]`));
  }

  return out;
}

function applyHover() {
  const svg = els.graph;
  svg.querySelectorAll(".hot").forEach((el) => el.classList.remove("hot"));
  svg.classList.toggle("isolating", hover !== null);

  if (!hover) {
    hideTooltip();
    return;
  }

  for (const el of hoverElements(hover)) if (el) el.classList.add("hot");

  if (hover.kind === "edge") showTooltip();
}

function nodeName(id) {
  const box = state.layout && state.layout.boxes.get(id);
  return box ? `${box.node.label || box.node.kind} #${box.node.id}` : `#${id}`;
}

// What the edge carries at the inspected tick, when it carries anything: the
// user type's name for a struct, else the value itself.
function edgeCarry(from, frame) {
  const value = frame ? frame.values.get(from) : null;
  if (!value || value.kind === "absent") return "";

  if (value.kind === "struct" && value.value) {
    const schema = state.schemasById.get(value.value.typeId);
    if (schema && schema.name) return schema.name;
  }

  const text = renderValue(value);
  return text.length > 24 ? text.slice(0, 23) + "…" : text;
}

function showTooltip() {
  const tick = state.ticks[state.tickIndex];
  const carry = edgeCarry(hover.from, tick ? currentFrame() : null);

  els.graphTooltip.textContent =
    `${nodeName(hover.from)} → ${nodeName(hover.to)}` + (carry ? ` · carries ${carry}` : "");
  els.graphTooltip.hidden = false;
}

function positionTooltip(event) {
  const tip = els.graphTooltip;
  if (tip.hidden) return;

  const panel = els.graph.parentElement.getBoundingClientRect();
  const x = event.clientX - panel.left + 14;
  const y = event.clientY - panel.top + 18;

  tip.style.left = `${Math.min(x, panel.width - tip.offsetWidth - 8)}px`;
  tip.style.top = `${Math.min(y, panel.height - tip.offsetHeight - 8)}px`;
}

function hideTooltip() {
  els.graphTooltip.hidden = true;
}

// --- recording events into ticks --------------------------------------------

function tickKey(thread, epoch) {
  return `${thread}|${epoch}`;
}

function onBatch(batch) {
  if (batch.dropped > 0) {
    state.dropped += batch.dropped;
    els.dropped.textContent = `dropped ${state.dropped}`;
  }

  for (const event of batch.events) recordEvent(event);

  if (state.view.centerNs === null) {
    const newest = newestTickTime();
    if (newest !== null) state.view.centerNs = newest;
  } else if (state.live) {
    const newest = newestTickTime();
    if (newest !== null) state.view.centerNs = newest;
  }

  updateSelectionFromCenter();
  renderTimeline();
  updateCursorView();
}

function recordEvent(event) {
  const key = tickKey(event.clock, event.epoch);

  if (event.kind === "region_step_begin") {
    const tick = { clock: event.clock, epoch: event.epoch, events: [], threw: false, startNs: event.stampNs, pending: undefined };
    state.open.set(key, tick);
    return;
  }

  // Node events and the closing region event belong to the open tick. If an
  // end arrives for a tick whose begin we missed (first batch after connect
  // may start mid-tick), synthesize the tick so no events are lost.
  let tick = state.open.get(key);
  if (!tick) {
    tick = { clock: event.clock, epoch: event.epoch, events: [], threw: false, startNs: event.stampNs, pending: undefined };
    state.open.set(key, tick);
    state.ticks.push(tick);
  }

  // Fold each begin/end (or begin/throw) pair into one evaluation item: the
  // engine emits the pair strictly adjacent, so a single step shows both the
  // activation and its result.
  if (event.kind === "node_tick_begin") {
    if (tick.pending !== undefined)
      tick.events.push({ kind: "node_tick_begin", nodeId: tick.pending }); // unreachable; keep the data
    tick.pending = event.nodeId;
    return;
  }

  if (event.kind === "node_tick_end") {
    tick.pending = undefined;
    tick.events.push({ kind: "node_tick", nodeId: event.nodeId, value: event.value, durationNs: event.durationNs });
    return;
  }

  if (event.kind === "node_tick_throw") {
    tick.pending = undefined;
    tick.events.push({ kind: "node_tick_throw", nodeId: event.nodeId, durationNs: event.durationNs });
    return;
  }

  // The region_step_begin/end pair brackets the tick but is not part of its
  // event list; node evaluations are what the inspector steps through.
  if (event.kind === "region_step_end") {
    if (tick.pending !== undefined) {
      tick.events.push({ kind: "node_tick_begin", nodeId: tick.pending }); // unreachable; keep the data
      tick.pending = undefined;
    }
    tick.threw = event.threw;
    tick.closeNs = event.stampNs;
    tick.durationNs = event.durationNs;
    state.open.delete(key);

    if (!state.ticks.includes(tick)) state.ticks.push(tick);

    // Freeze the axis origin on the first tick ever; the legend is relative
    // wall-clock time from here.
    if (state.originNs === null) state.originNs = tick.startNs;

    while (state.ticks.length > MAX_TICKS) state.ticks.shift();
    return;
  }
}

// --- timeline: a wall-clock scrubber ----------------------------------------
//
// Producer steady-clock time (tick.startNs) maps to x. A vertical line at the
// visual center always marks the current moment (state.view.centerNs); the
// inspected tick is the latest tick at or before it — an exact hit selects
// that tick, never its predecessor. Drag pans, wheel and pinch zoom, the tick
// buttons jump the center onto the neighboring tick.

const TL_CLOCKS = ["audio", "ui", "worker"];
const TL_GUTTER = 76;  // px reserved for lane labels
const TL_LANE_H = 30;
const TL_AXIS_H = 22;
const TL_MAX_PX_PER_SEC = 5e6;
// Zoom-out floor: the visible window may not exceed the total tick span (so
// you cannot get lost in empty time). Floored at a few seconds so a fresh
// session with almost no span stays navigable.
const TL_MIN_WINDOW_SEC = 4;
const TL_NS = "http://www.w3.org/2000/svg";

const TICK_STROKE = { audio: "#9ece6a", ui: "#7aa2f7", worker: "#bb9af7" };
const TICK_THREW = "#ff6b6b";

function tickTime(tick) {
  return tick.startNs;
}

function timelineWidth() {
  const w = els.timeline.clientWidth;
  return w > 0 ? w : 800;
}

function timelineHeight() {
  return TL_CLOCKS.length * TL_LANE_H + TL_AXIS_H;
}

function centerX(width) {
  return TL_GUTTER + (width - TL_GUTTER) / 2;
}

function timeToX(tNs, width) {
  return centerX(width) + ((tNs - state.view.centerNs) / 1e9) * state.view.pxPerSec;
}

function xToTime(x, width) {
  return state.view.centerNs + ((x - centerX(width)) / state.view.pxPerSec) * 1e9;
}

// A "nice" grid step (1/2/5 x 10^n seconds) spanning at least ~90 px, so the
// legend stays readable at any zoom.
function gridStepNs() {
  const raw = (90 / state.view.pxPerSec) * 1e9;
  const mag = Math.pow(10, Math.floor(Math.log10(raw)));
  for (const m of [1, 2, 5, 10]) if (m * mag >= raw) return m * mag;
  return 10 * mag;
}

// Relative wall-clock time from the frozen session origin.
function fmtTime(tNs) {
  if (tNs === null || tNs === undefined || state.originNs === null) return "—";
  const s = (tNs - state.originNs) / 1e9;
  const a = Math.abs(s);
  const d = a >= 100 ? 0 : a >= 1 ? 1 : 0.01 <= a ? 2 : 3;
  // A tick that started a hair before the frozen origin formats as "-0.000s";
  // nobody wants to see negative zero on a clock.
  const text = s.toFixed(d);
  return (Number(text) === 0 ? (0).toFixed(d) : text) + "s";
}

function orderedTicks() {
  return state.ticks
    .map((t, i) => [t, i])
    .sort((a, b) => a[0].startNs - b[0].startNs || a[1] - b[1])
    .map(([t]) => t);
}

function newestTickTime() {
  let best = null;
  for (const tick of state.ticks)
    if (best === null || tick.startNs > best) best = tick.startNs;
  return best;
}

// The inspected tick: the latest one at or before the center. Exact equality
// uses <=, so sitting exactly on a tick selects it (no off-by-one); anything
// strictly between two ticks selects the left (past) one.
function tickAtCenter() {
  if (state.view.centerNs === null) return null;

  let best = null;
  for (const tick of state.ticks) {
    if (tick.startNs <= state.view.centerNs
        && (best === null || tick.startNs >= best.startNs)) best = tick;
  }
  return best;
}

function updateSelectionFromCenter() {
  const tick = tickAtCenter();
  const idx = tick ? state.ticks.indexOf(tick) : -1;

  if (idx !== state.tickIndex) {
    state.tickIndex = idx;
    state.eventIndex = tick ? tick.events.length - 1 : -1;
  }
}

// The recorded span, from the past-most to the future-most tick.
function tickSpan() {
  if (!state.ticks.length) return null;

  let lo = Infinity;
  let hi = -Infinity;
  for (const tick of state.ticks) {
    if (tick.startNs < lo) lo = tick.startNs;
    if (tick.startNs > hi) hi = tick.startNs;
  }
  return { lo, hi };
}

// The present may not leave the recorded span: panning past either end pins
// it to the past-most / future-most tick.
function clipCenter(ns) {
  const span = tickSpan();
  if (!span) return ns;
  return Math.min(span.hi, Math.max(span.lo, ns));
}

// Zoom-out limit for the given viewport width: the window may not exceed the
// total tick span (floored, see TL_MIN_WINDOW_SEC).
function minPxPerSec(width) {
  const plot = Math.max(1, width - TL_GUTTER);
  const span = tickSpan();
  const spanSec = span ? (span.hi - span.lo) / 1e9 : 0;
  return plot / Math.max(spanSec, TL_MIN_WINDOW_SEC);
}

function clampZoom(v, width) {
  return Math.min(TL_MAX_PX_PER_SEC, Math.max(minPxPerSec(width), v));
}

// Move the present to ns (clipped to the span; selection follows); callers
// own the live flag.
function setCenter(ns) {
  state.view.centerNs = clipCenter(ns);
  updateSelectionFromCenter();
  renderTimeline();
  updateCursorView();
}

function tlEl(name, attrs, parent) {
  const n = document.createElementNS(TL_NS, name);
  for (const [k, v] of Object.entries(attrs)) n.setAttribute(k, v);
  if (parent) parent.appendChild(n);
  return n;
}

function renderTimeline() {
  const width = timelineWidth();
  const height = timelineHeight();

  let svg = els.timeline.querySelector("svg");

  if (!svg) {
    svg = document.createElementNS(TL_NS, "svg");
    svg.setAttribute("id", "timeline-svg");
    els.timeline.appendChild(svg);
    attachTimelineGestures(svg);
  }

  svg.setAttribute("width", width);
  svg.setAttribute("height", height);
  svg.setAttribute("viewBox", `0 0 ${width} ${height}`);
  svg.replaceChildren();

  // Lane separators and labels always draw, so the widget has a stable shape
  // before any data arrives.
  TL_CLOCKS.forEach((clock, i) => {
    tlEl("line", { x1: 0, y1: i * TL_LANE_H, x2: width, y2: i * TL_LANE_H, class: "tl-lane-sep" }, svg);
    const lab = tlEl("text", { x: 8, y: i * TL_LANE_H + TL_LANE_H / 2 + 4, class: "tl-lane-label" }, svg);
    lab.textContent = clock;
  });
  tlEl(
    "line",
    { x1: 0, y1: TL_CLOCKS.length * TL_LANE_H, x2: width, y2: TL_CLOCKS.length * TL_LANE_H, class: "tl-lane-sep" },
    svg
  );

  if (state.view.centerNs === null || !state.ticks.length) {
    if (!state.ticks.length) {
      const hint = tlEl("text", { x: TL_GUTTER + 8, y: 18, class: "tl-axis" }, svg);
      hint.textContent = "no ticks recorded";
    }
    return;
  }

  const tLeft = xToTime(TL_GUTTER, width);
  const tRight = xToTime(width, width);

  const defs = tlEl("defs", {}, svg);
  const clip = tlEl("clipPath", { id: "tl-clip" }, defs);
  tlEl("rect", { x: TL_GUTTER, y: 0, width: width - TL_GUTTER, height: height - TL_AXIS_H }, clip);
  const plot = tlEl("g", { "clip-path": "url(#tl-clip)" }, svg);

  // Faint vertical gridlines with the time legend on the axis row.
  const step = gridStepNs();
  for (let t = Math.ceil(tLeft / step) * step; t <= tRight; t += step) {
    const x = timeToX(t, width);
    tlEl("line", { x1: x, y1: 0, x2: x, y2: height - TL_AXIS_H, class: "tl-grid" }, plot);
    const label = tlEl("text", { x: x + 4, y: height - 7, class: "tl-axis" }, svg);
    label.textContent = fmtTime(t);
  }

  // One vertical line per visible tick on its thread's lane. The selected tick
  // draws last (halo + heavier line on top) so it stays visible in a dense
  // lane instead of drowning among its neighbors.
  const selected = state.ticks[state.tickIndex];

  const drawTick = (tick, isSel) => {
    const t = tickTime(tick);
    if (t < tLeft || t > tRight) return;

    const lane = TL_CLOCKS.indexOf(tick.clock);
    if (lane < 0) return;

    const x = timeToX(t, width);
    const y0 = lane * TL_LANE_H + 4;
    const y1 = (lane + 1) * TL_LANE_H - 4;

    if (isSel)
      tlEl("line", { x1: x, y1: y0, x2: x, y2: y1, class: "tl-tick-halo" }, plot);

    const line = tlEl(
      "line",
      {
        x1: x, y1: y0, x2: x, y2: y1,
        class: "tl-tick" + (isSel ? " selected" : ""),
        stroke: tick.threw ? TICK_THREW : TICK_STROKE[tick.clock] || "#888",
      },
      plot
    );

    const tip = tlEl("title", {}, line);
    tip.textContent = `${tick.clock} #${tick.epoch}`
      + (typeof tick.durationNs === "number" ? ` · ${fmtDuration(tick.durationNs)}` : "")
      + (tick.threw ? " · threw" : "");
  };

  for (const tick of state.ticks) if (tick !== selected) drawTick(tick, false);
  if (selected) drawTick(selected, true);

  // The present: a vertical line pinned to the visual center.
  const cx = centerX(width);
  tlEl("line", { x1: cx, y1: 0, x2: cx, y2: height - TL_AXIS_H, class: "tl-center" }, svg);
  const now = tlEl("text", { x: cx + 5, y: 12, class: "tl-center-label" }, svg);
  now.textContent = fmtTime(state.view.centerNs);
}

// --- timeline gestures: drag to pan, wheel and pinch to zoom ---------------

// Leaving live mode always pauses capture (and re-entering resumes it), so
// the checkbox truthfully reflects what the producer is doing and browsing
// can never lose the visualized ticks to history eviction.
function setFollowing(on) {
  if (state.live === on && els.live.checked === on) return;
  state.live = on;
  els.live.checked = on;
  sendCapture(on);
  // Pausing with no live transport must not leave a stale disconnect error on
  // screen: while paused there is no connection to report on.
  const connected = localDebug
    ? localConnected
    : Boolean(ws && ws.readyState === WebSocket.OPEN);
  if (!on && !connected) setStatus("paused", "");
}

function zoomBy(factor) {
  if (state.view.centerNs === null) return;
  setFollowing(false);
  state.view.pxPerSec = clampZoom(state.view.pxPerSec * factor, timelineWidth());
  renderTimeline();
  updateCursorView();
}

function attachTimelineGestures(svg) {
  const pointers = new Map();
  let drag = null;
  let pinch = null;

  const svgRect = () => {
    const rect = svg.getBoundingClientRect();
    return { left: rect.left || 0, width: rect.width || timelineWidth() };
  };

  const nearestTick = (x, width, maxPx) => {
    if (state.view.centerNs === null) return null;
    const t = xToTime(x, width);
    const tol = (maxPx / state.view.pxPerSec) * 1e9;
    let best = null;
    let bestDt = Infinity;
    for (const tick of state.ticks) {
      const dt = Math.abs(tick.startNs - t);
      if (dt <= tol && dt < bestDt) { best = tick; bestDt = dt; }
    }
    return best;
  };

  svg.addEventListener("pointerdown", (e) => {
    try {
      svg.setPointerCapture(e.pointerId);
    } catch (_) { /* jsdom has no pointer capture */ }

    pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
    setFollowing(false);

    if (pointers.size === 1) {
      drag = { startX: e.clientX, startCenter: state.view.centerNs, moved: false };
      pinch = null;
    } else if (pointers.size === 2) {
      const [a, b] = [...pointers.values()];
      pinch = { dist: Math.hypot(a.x - b.x, a.y - b.y) };
      drag = null;
    }
  });

  svg.addEventListener("pointermove", (e) => {
    if (!pointers.has(e.pointerId) || state.view.centerNs === null) return;
    pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });

    if (pinch && pointers.size >= 2) {
      const [a, b] = [...pointers.values()];
      const dist = Math.hypot(a.x - b.x, a.y - b.y);
      const { left, width } = svgRect();
      const midX = (a.x + b.x) / 2 - left;

      if (pinch.dist > 0 && dist > 0) {
        const tAnchor = xToTime(midX, width);
        state.view.pxPerSec = clampZoom(state.view.pxPerSec * (dist / pinch.dist), width);
        setCenter(tAnchor - ((midX - centerX(width)) / state.view.pxPerSec) * 1e9);
      }

      pinch.dist = dist;
      return;
    }

    if (drag) {
      const dx = e.clientX - drag.startX;
      if (Math.abs(dx) > 4) drag.moved = true;

      if (drag.moved && drag.startCenter !== null)
        setCenter(drag.startCenter - (dx / state.view.pxPerSec) * 1e9);
    }
  });

  const endPointer = (e) => {
    const wasTap = pointers.size === 1 && drag !== null && !drag.moved;
    pointers.delete(e.pointerId);
    if (pointers.size < 2) pinch = null;

    if (wasTap) {
      // A tap (no drag) on a tick line jumps the present onto it, selecting
      // it exactly.
      const { left, width } = svgRect();
      const hit = nearestTick(e.clientX - left, width, 8);
      if (hit) setCenter(hit.startNs);
    }

    if (pointers.size === 0) {
      drag = null;
    } else if (pointers.size === 1 && drag === null) {
      // Pinch released into a drag: re-anchor on the remaining pointer so the
      // view does not jump. Mark moved so the release is not read as a tap.
      const [, p] = [...pointers.entries()][0];
      drag = { startX: p.x, startCenter: state.view.centerNs, moved: true };
    }
  };

  svg.addEventListener("pointerup", endPointer);
  svg.addEventListener("pointercancel", endPointer);

  svg.addEventListener("wheel", (e) => {
    e.preventDefault();
    if (state.view.centerNs === null) return;
    setFollowing(false);

    // Firefox reports lines instead of pixels; normalize to pixels.
    const unit = e.deltaMode === 1 ? 16 : 1;
    const dx = e.deltaX * unit;
    const dy = e.deltaY * unit;

    const { left, width } = svgRect();

    // A horizontal swipe (two fingers on a trackpad, or shift+wheel) pans the
    // present; the vertical component zooms around the pointer, as before. A
    // diagonal gesture does both, like a map.
    let center = state.view.centerNs;
    if (dx !== 0) center = center - (dx / state.view.pxPerSec) * 1e9;

    if (dy !== 0) {
      const x = e.clientX - left;
      const tAnchor = xToTime(x, width);
      const factor = dy < 0 ? 1.1 : 1 / 1.1;
      state.view.pxPerSec = clampZoom(state.view.pxPerSec * factor, width);
      center = tAnchor - ((x - centerX(width)) / state.view.pxPerSec) * 1e9;
    }

    setCenter(center);
  }, { passive: false });
}

function renderInspector(tick, selectedEventIndex) {
  els.eventList.replaceChildren();

  const event = tick && selectedEventIndex >= 0 ? tick.events[selectedEventIndex] : null;
  renderInspectorValue(event);
  renderInspectorNode(event);

  if (!tick) return;

  for (let i = 0; i < tick.events.length; ++i) {
    const item = tick.events[i];
    const li = document.createElement("li");
    li.textContent = describeEvent(item);
    if (i === selectedEventIndex) li.className = "current";
    li.onclick = () => {
      state.eventIndex = i;
      updateCursorView();
    };
    els.eventList.appendChild(li);
  }
}

// The selected event's produced value: a struct renders as a field table — the
// same rows as its on-graph value box — anything else as plain text.
function renderInspectorValue(event) {
  const el = els.inspectorValue;
  el.replaceChildren();

  if (!event || event.kind !== "node_tick" || !event.value || event.value.kind === "absent") {
    el.textContent = "—";
    return;
  }

  const value = event.value;
  const schema = value.kind === "struct" && value.value
    ? state.schemasById.get(value.value.typeId)
    : null;

  if (!schema) {
    const code = document.createElement("code");
    code.textContent = renderValue(value);
    el.appendChild(code);
    return;
  }

  const table = document.createElement("table");
  table.className = "struct-table";

  const head = document.createElement("tr");
  const th = document.createElement("th");
  th.colSpan = 3;
  th.textContent = schema.name || "struct";
  head.appendChild(th);
  table.appendChild(head);

  for (const [i, field] of (schema.fields || []).slice(0, MAX_STRUCT_FIELDS).entries()) {
    const tr = document.createElement("tr");
    const cells = [
      ["col-type", field.kind || "?"],
      ["col-name", `${field.name};`],
      ["col-value", structCellText(value.value.fields[i])],
    ];

    for (const [cls, text] of cells) {
      const td = document.createElement("td");
      td.className = cls;
      td.textContent = text;
      tr.appendChild(td);
    }

    table.appendChild(tr);
  }

  el.appendChild(table);
}

// The selected event's node: identity, kind, clock, and fan-in/fan-out.
function renderInspectorNode(event) {
  const el = els.inspectorNode;
  el.replaceChildren();

  const box = event && state.layout ? state.layout.boxes.get(event.nodeId) : null;
  if (!box) {
    el.textContent = "—";
    return;
  }

  const fanOut = state.layout.edges.filter((e) => e.from === event.nodeId).length;
  el.textContent =
    `${box.node.label || box.node.kind} #${box.node.id} · ${box.node.kind}`
    + ` · ${box.node.clock} clock · fan-in ${box.node.fanIn.length} · fan-out ${fanOut}`;
}

function describeEvent(item) {
  if (item.kind === "node_tick") return `eval #${item.nodeId} = ${renderValue(item.value)} · ${fmtDuration(item.durationNs)}`;
  if (item.kind === "node_tick_throw") return `#${item.nodeId} THREW · ${fmtDuration(item.durationNs)}`;
  // Defensive: an unpaired begin (unreachable with the current engine).
  return `eval_begin #${item.nodeId}`;
}

// A measured evaluation time in appropriate units: steady_clock nanoseconds
// read as ns below a microsecond, then µs, ms, s. Deliberately quiet — one
// small suffix on the inspector row, nowhere else.
function fmtDuration(ns) {
  if (!Number.isFinite(ns) || ns < 0) return "—";

  const units = [[1e9, "s"], [1e6, "ms"], [1e3, "µs"], [1, "ns"]];

  for (const [factor, unit] of units) {
    if (ns >= factor) {
      const v = ns / factor;
      return `${v >= 100 ? Math.round(v) : Math.round(v * 10) / 10} ${unit}`;
    }
  }

  return "0 ns";
}

// A user struct renders as `name{field=value, ...}` from the schema shipped in
// the graph description. A type with no schema (anonymous, or unknown to this
// client) renders as a bare `struct`; nested structs beyond a shallow depth do
// too, so a deep value cannot blow up a chip.
const MAX_STRUCT_DEPTH = 2;
const MAX_STRUCT_TEXT = 60;

function renderValue(value, depth = 0) {
  if (!value || value.kind === "absent") return "—";
  if (value.kind === "unit") return "∅";
  if (value.kind === "bool") return String(value.value);
  if (value.kind === "int") return String(value.value);
  if (value.kind === "double") return Number.isInteger(value.value) ? value.value + ".0" : String(value.value);

  if (value.kind === "string") {
    const text = `"${value.value}"`;
    return text.length > MAX_STRUCT_TEXT ? text.slice(0, MAX_STRUCT_TEXT - 1) + "…" : text;
  }

  if (value.kind === "struct") {
    const schema = state.schemasById ? state.schemasById.get(value.value.typeId) : null;

    if (!schema || depth >= MAX_STRUCT_DEPTH) return schema && schema.name ? schema.name : "struct";

    const shown = schema.fields.slice(0, MAX_STRUCT_FIELDS);
    const parts = shown.map((field, i) => `${field.name}=${renderValue(value.value.fields[i], depth + 1)}`);
    let text = `${schema.name || "struct"}{${parts.join(", ")}`;

    if (schema.fields.length > shown.length) text += ", …";
    text += "}";

    return text.length > MAX_STRUCT_TEXT ? text.slice(0, MAX_STRUCT_TEXT - 1) + "…" : text;
  }

  return "?";
}

// --- cursor view: node highlights + value chips -----------------------------

// Derive, for the selected tick at the selected evaluation, which node is
// under inspection (highlighted), which nodes produced which values so far
// (chips), and which node threw, if any.
function currentFrame() {
  const tick = state.ticks[state.tickIndex];
  if (!tick) return { evaluating: null, values: new Map(), threw: null };

  const values = new Map();
  let threw = null;

  for (let i = 0; i <= state.eventIndex && i < tick.events.length; ++i) {
    const item = tick.events[i];
    if (item.kind === "node_tick") values.set(item.nodeId, item.value);
    else if (item.kind === "node_tick_throw") threw = item.nodeId;
  }

  const cur = state.eventIndex >= 0 && state.eventIndex < tick.events.length
    ? tick.events[state.eventIndex]
    : null;
  const evaluating = cur && (cur.kind === "node_tick" || cur.kind === "node_tick_begin") ? cur.nodeId : null;

  return { evaluating, values, threw };
}

function updateCursorView() {
  const tick = state.ticks[state.tickIndex];

  if (!tick) {
    els.cursorLabel.textContent = state.ticks.length
      ? `t=${fmtTime(state.view.centerNs)} · no tick at cursor`
      : "no ticks recorded";
    renderInspector(null, -1);
    return;
  }

  // A batch can only arrive after its connection's graph description, but
  // guard anyway so a bogus frame cannot turn the whole view inert.
  if (!state.layout) return;

  const frame = currentFrame();

  els.cursorLabel.textContent =
    `t=${fmtTime(state.view.centerNs)} · ${tick.clock} #${tick.epoch} · event ${state.eventIndex + 1}/${tick.events.length}`
    + (typeof tick.durationNs === "number" ? ` · tick ${fmtDuration(tick.durationNs)}` : "")
    + (tick.threw ? " · threw" : "");

  // Node fills: evaluating (light green), threw (red), else default.
  for (const [id, box] of state.layout.boxes) {
    const group = els.graph.querySelector(`g[data-node="${id}"] rect.node-box`);
    if (!group) continue;

    if (id === frame.threw) {
      group.setAttribute("fill", "var(--threw)");
      group.setAttribute("stroke", "var(--threw)");
    } else if (id === frame.evaluating) {
      group.setAttribute("fill", "var(--evaluating)");
      group.setAttribute("stroke", "var(--evaluating)");
    } else {
      group.setAttribute("fill", NODE_FILL[box.node.clock] || "#222");
      group.setAttribute("stroke", NODE_STROKE[box.node.clock] || "#555");
    }
  }

  renderValueBoxes(frame.values);
  renderInspector(tick, state.eventIndex);
}

// A value box: the value an edge carries, drawn at the curve's midpoint. A
// struct with a schema renders as a field table (a header with the type name,
// then one row per field: type · name · value); anything else is a one-line
// chip. Nested structs collapse to `{…}` — the inspector shows the full tree.
function appendValueBox(layer, edge, value, full) {
  const schema = value && value.kind === "struct" && value.value
    ? state.schemasById.get(value.value.typeId)
    : null;

  const table = full && schema ? structBoxSize(schema) : null;
  const mid = edge.route.mid;
  const clock = state.layout.boxes.get(edge.from)?.node.clock;
  const stroke = NODE_STROKE[clock] || "#555";

  const group = document.createElementNS(NS, "g");
  group.setAttribute("class", "value-box dim");
  group.dataset.from = edge.from;
  group.dataset.to = edge.to;

  // The full text survives as the native tooltip (and in the inspector); the
  // box itself is only as wide as its schema warrants.
  const title = document.createElementNS(NS, "title");
  title.textContent = renderValue(value);
  group.appendChild(title);

  if (!table) {
    const text = valueBoxChipText(value, schema);
    const w = text.length * 7 + 10;

    const bg = document.createElementNS(NS, "rect");
    bg.setAttribute("x", mid.x - w / 2);
    bg.setAttribute("y", mid.y - 9);
    bg.setAttribute("width", w);
    bg.setAttribute("height", 18);
    bg.setAttribute("rx", 3);
    bg.setAttribute("class", "value-chip-bg");
    group.appendChild(bg);

    appendText(group, mid.x, mid.y + 4, "value-chip-text", text, "middle");
    layer.appendChild(group);
    return;
  }

  const x = mid.x - table.w / 2;
  const y = mid.y - table.h / 2;
  const fields = (schema.fields || []).slice(0, MAX_STRUCT_FIELDS);
  const more = (schema.fields || []).length - fields.length;
  const typeW = Math.max(0, ...fields.map((f) => (f.kind || "").length)) * 7;
  const nameX = x + 12 + typeW + BOX_COL_PAD;

  const frame = document.createElementNS(NS, "rect");
  frame.setAttribute("x", x);
  frame.setAttribute("y", y);
  frame.setAttribute("width", table.w);
  frame.setAttribute("height", table.h);
  frame.setAttribute("rx", 8);
  frame.setAttribute("class", "value-box-frame");
  group.appendChild(frame);

  appendText(group, x + 12, y + 20, "value-box-header", schema.name || "struct");

  const sep = document.createElementNS(NS, "line");
  sep.setAttribute("x1", x);
  sep.setAttribute("y1", y + BOX_HEADER_H);
  sep.setAttribute("x2", x + table.w);
  sep.setAttribute("y2", y + BOX_HEADER_H);
  sep.setAttribute("class", "value-box-sep");
  group.appendChild(sep);

  fields.forEach((field, i) => {
    const rowY = y + BOX_HEADER_H + (i + 0.5) * BOX_ROW_H + 4;
    appendText(group, x + 12, rowY, "value-box-type", field.kind || "?");
    appendText(group, nameX, rowY, "value-box-name", `${field.name};`);
    appendText(
      group,
      x + table.w - 12,
      rowY,
      "value-box-value",
      structCellText(value.value.fields[i]),
      "end",
    );
  });

  if (more > 0)
    appendText(
      group,
      x + 12,
      y + BOX_HEADER_H + (fields.length + 0.5) * BOX_ROW_H + 4,
      "value-box-more",
      `… ${more} more`,
    );

  // Attachment dots where the edge meets the box.
  appendPortDot(group, x, mid.y, stroke);
  appendPortDot(group, x + table.w, mid.y, stroke);

  layer.appendChild(group);
}

// A value as a one-line chip: structs collapse to `name{…}`, scalars are their
// value, both capped to the width the layout reserves.
function valueBoxChipText(value, schema) {
  let text;

  if (value && value.kind === "struct")
    text = schema && schema.name ? `${schema.name}{…}` : "struct";
  else
    text = renderValue(value);

  return text.length > 16 ? text.slice(0, 15) + "…" : text;
}

// A field's value as a table cell: nested structs collapse, an absent field is
// an em dash. Cells are capped to the width the box was sized for.
function structCellText(fieldValue) {
  if (!fieldValue || fieldValue.kind === "absent") return "—";
  if (fieldValue.kind === "struct") return "{…}";

  const text = renderValue(fieldValue);
  return text.length > 10 ? text.slice(0, 9) + "…" : text;
}

function renderValueBoxes(values) {
  const layer = els.graph.querySelector("#values");
  if (!layer) return;
  layer.replaceChildren();

  // Tab labels reset first: this runs on every cursor move without a
  // re-render, so stale inline values must be cleared before applying new ones.
  for (const [id, box] of state.layout.boxes) {
    if (!box.tab) continue;
    const label = els.graph.querySelector(`g[data-node="${id}"] text.port-label`);
    if (label) label.textContent = box.node.port || box.node.kind;
  }

  // Every edge carries its source's value, so group the edges by source. A
  // fan-out source paints the full box on its first edge (lowest target id)
  // and a compact chip on the rest, so a wide struct table is never duplicated
  // across parallel edges.
  const bySource = new Map();
  for (const edge of state.layout.edges) {
    const value = values.get(edge.from);
    if (value === undefined) continue;
    if (!bySource.has(edge.from)) bySource.set(edge.from, { value, edges: [] });
    bySource.get(edge.from).edges.push(edge);
  }

  for (const [, { value, edges }] of bySource) {
    edges.sort((a, b) => a.to - b.to);
    edges.forEach((edge, i) => appendValueBox(layer, edge, value, i === 0));
  }

  // Port values also render inline in their tab — no separate chip competes
  // for gutter space. The tab is sized for its name plus a short scalar;
  // longer values truncate (full text survives in the inspector).
  for (const [id, value] of values) {
    const box = state.layout.boxes.get(id);
    if (!box || !box.tab) continue;

    const label = els.graph.querySelector(`g[data-node="${id}"] text.port-label`);
    if (!label) continue;

    const text = renderValue(value);
    const name = box.node.port || box.node.kind;
    const room = Math.floor((box.w - 16) / 7) - name.length - 1;
    const shown = text.length <= room ? text : text.slice(0, Math.max(0, room - 1)) + "…";
    label.textContent = `${name} ${shown}`;
  }
}

// --- transport --------------------------------------------------------------

// Step within the selected tick; crossing its edge moves the present onto the
// neighboring tick in time (and the center with it, so the selection stays
// meaningful).
function gotoEvent(delta) {
  setFollowing(false);

  const tick = state.ticks[state.tickIndex];
  const order = orderedTicks();

  if (!tick || !tick.events.length) {
    const nt = tick ? neighborTick(order, tick, delta > 0 ? 1 : -1) : order[0];
    if (nt) setCenter(nt.startNs);
    return;
  }

  const index = state.eventIndex + delta;

  if (index < 0 || index >= tick.events.length) {
    const nt = neighborTick(order, tick, index < 0 ? -1 : 1);
    if (!nt) {
      state.eventIndex = index < 0 ? 0 : tick.events.length - 1;
    } else {
      setCenter(nt.startNs);
      state.eventIndex = index < 0 ? nt.events.length - 1 : 0;
    }
  } else {
    state.eventIndex = index;
  }

  renderTimeline();
  updateCursorView();
}

function neighborTick(order, tick, dir) {
  const pos = order.indexOf(tick);
  if (pos < 0) return dir > 0 ? order[0] || null : order[order.length - 1] || null;
  const next = pos + dir;
  return next >= 0 && next < order.length ? order[next] : null;
}

// The tick buttons move the present itself: the center jumps onto the
// neighboring tick, which the <= rule then selects exactly.
function gotoTick(delta) {
  setFollowing(false);

  const order = orderedTicks();
  if (!order.length) return;

  const tick = state.ticks[state.tickIndex];
  const nt = tick ? neighborTick(order, tick, delta) : (delta > 0 ? order[0] : order[order.length - 1]);
  if (nt) setCenter(nt.startNs);
}

// The region buttons jump within one lane only: to the previous/next tick of
// the region whose tick is currently under inspection, skipping every other
// thread's ticks in between.
function gotoRegionTick(delta) {
  setFollowing(false);

  const tick = state.ticks[state.tickIndex];
  if (!tick) return;

  const order = orderedTicks().filter((t) => t.clock === tick.clock);
  const nt = neighborTick(order, tick, delta);
  if (nt) setCenter(nt.startNs);
}

els.live.onchange = () => {
  setFollowing(els.live.checked);

  if (state.live) {
    connect(); // no-op unless the socket is gone (e.g. died while paused)
    const newest = newestTickTime();
    if (newest !== null) setCenter(newest);
  }
};

// Switching examples is a request, not a local state change: the server
// flips, quarantines, and broadcasts the new graph, and every client
// (including this one) resets when that graph arrives. A re-select of the
// live example is a server-side no-op, so the view never flickers.
els.example.onchange = () => {
  sendSelectExample(els.example.value);
};

els.prevEvent.onclick = () => gotoEvent(-1);
els.nextEvent.onclick = () => gotoEvent(1);
els.prevTick.onclick = () => gotoTick(-1);
els.nextTick.onclick = () => gotoTick(1);
els.prevRegionTick.onclick = () => gotoRegionTick(-1);
els.nextRegionTick.onclick = () => gotoRegionTick(1);
els.zoomIn.onclick = () => zoomBy(1.25);
els.zoomOut.onclick = () => zoomBy(1 / 1.25);

document.addEventListener("keydown", (event) => {
  if (event.key === "ArrowRight") event.shiftKey ? gotoTick(1) : gotoEvent(1);
  else if (event.key === "ArrowLeft") event.shiftKey ? gotoTick(-1) : gotoEvent(-1);
  else if (event.key === "+" || event.key === "=") zoomBy(1.25);
  else if (event.key === "-" || event.key === "_") zoomBy(1 / 1.25);
  else if (event.key === " ") {
    event.preventDefault();
    els.live.checked = !els.live.checked;
    els.live.onchange();
  }
});

// --- boot -------------------------------------------------------------------

if (typeof ResizeObserver !== "undefined")
  new ResizeObserver(() => renderTimeline()).observe(els.timeline);

attachGraphGestures();
attachGraphHover();
connect();