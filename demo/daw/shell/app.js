// app.js — the demo page: a DAW-shaped shell around the FRP network.
//
// Everything here is deliberately dumb: no transport math, no scheduling, no
// playback logic. Widgets translate gestures into port sets, the timeline
// renders outputs the engine posts, and the two auto loops belong to the
// engine's clocks. The one piece of "intelligence" is the timeline geometry
// (snap, drag, window), which is drawing, not audio.
//
// Transport: the wasm build runs the engine in a Web worker (engine_worker.js);
// the native host opens a WebSocket and speaks the same messages. `?native=1`
// selects the latter; the rest of the page cannot tell the difference.
//
// The domain widgets are controlled: gestures only post commands, and the
// engine's `state` snapshots are the only source of the rendered playing/bpm/
// master/loop/block/auto values. Seek and pure view state stay page-local.
//
// The FRP path to care about: the thick cursor is `playhead_ui` — the ui
// region's latest sample of the audio playhead, crossed back through the
// network. Pause the ui auto-loop and it freezes while the thin ghost (the
// audio region's own output) keeps moving: that gap IS the latest crossing.

/*global Worker, WebSocket, URLSearchParams, document, window */

(function () {
    "use strict";

    // ---------- tiny helpers -------------------------------------------------

    const $ = (id) => document.getElementById(id);
    const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
    const num = (v) => (typeof v === "number" && Number.isFinite(v) ? v : null);
    const fmt = (v, d = 3) => (num(v) === null ? "—" : v.toFixed(d));

    // Mirrors the network's script (daw/network.hpp score) — display only:
    // the note block spans [start, start+len); its off crossing is the
    // wrapped end beat, so under a short loop the block may extend past the
    // loop band and close on the next lap.
    const SCORE_BEAT = 3.5;
    const SCORE_LEN = 1.0; // the note's length in beats (its off crossing)
    const SNAP = 0.25; // timeline snap grid, in beats

    // ---------- state --------------------------------------------------------

    const controls = {
        playing: false,
        bpm: 120,
        master: 0.8,
        loop: { start: 0, end: 4, on: true },
        blockN: 1024,
        rate: 48000,
        autoUi: true,
        autoAudio: true,
    };

    const ui = { seq: 0, playhead_ui: null };
    const audio = {
        k: 0,
        block: null,
        playhead: null,
        midi_count: null,
        midi_offset: null,
        midi_off_count: null,
        voice_gain: null,
    };

    const log = { queue: [], paused: false, filter: "all" };
    let seekPending = null;
    let loopPreview = null; // drag-only view state, cleared on pointerup
    let haveState = false;

    const els = {};

    // ---------- transport ----------------------------------------------------

    // A Worker-shaped facade over the native host's WebSocket. Commands sent
    // before the socket opens are queued; messages are decoded into the same
    // {data} shape worker.onmessage receives.
    function makeNativeWorker() {
        const proto = window.location.protocol === "https:" ? "wss:" : "ws:";
        const socket = new WebSocket(`${proto}//${window.location.host}/ws`);
        const queued = [];
        const facade = {
            onmessage: null,
            onerror: null,
            postMessage(cmd) {
                const text = JSON.stringify(cmd);

                if (socket.readyState === WebSocket.OPEN)
                    socket.send(text);
                else
                    queued.push(text);
            },
        };

        socket.addEventListener("open", () => {
            for (const text of queued)
                socket.send(text);
            queued.length = 0;
        });
        socket.addEventListener("message", (event) => {
            if (!facade.onmessage)
                return;
            try {
                facade.onmessage({ data: JSON.parse(event.data) });
            } catch {
                // A malformed frame is a host bug, not a page crash.
            }
        });
        socket.addEventListener("error", () => {
            if (facade.onerror)
                facade.onerror({ message: "native engine socket failed" });
        });
        socket.addEventListener("close", () => {
            if (facade.onerror)
                facade.onerror({ message: "native engine socket closed" });
        });

        return facade;
    }

    const native = window.CLOCKR_NATIVE === true
        || new URLSearchParams(window.location.search).has("native");
    const worker = native ? makeNativeWorker() : new Worker("engine_worker.js");

    function send(cmd) {
        worker.postMessage(cmd);
    }

    worker.onmessage = (event) => {
        const msg = event.data;

        switch (msg.type) {
            case "ready": {
                controls.playing = msg.defaults.playing;
                controls.bpm = msg.defaults.bpm;
                controls.master = msg.defaults.master;
                controls.loop = { ...msg.defaults.loop };
                controls.blockN = msg.block.n;
                controls.rate = msg.block.rate;
                ui.seq = 0;
                ui.playhead_ui = msg.outputs.ui.playhead_ui;
                audio.k = 0;
                audio.block = null;
                audio.playhead = null;
                audio.midi_count = null;
                audio.midi_offset = null;
                audio.midi_off_count = null;
                audio.voice_gain = null;
                for (const name of Object.keys(outputCounts))
                    outputCounts[name] = 0;
                seekPending = null;
                loopPreview = null;
                $("seek-mark").hidden = true;
                badge("engine: running", "on");
                pushLog("sys", "engine constructed — ui region ticked once with defaults (a latest link reads monostate until its producer ticks)");

                haveState = false;

                if (msg.state)
                    applyState(msg.state, false);
                else
                {
                    syncWidgets();
                    haveState = true;
                }

                // Match the region auto-loops to the checkboxes (they start
                // checked; on reset the worker kept whatever ran before).
                send({ type: "autoUi", on: $("auto-ui").checked });
                send({ type: "autoAudio", on: $("auto-audio").checked });
                break;
            }

            case "ui": {
                ui.seq = msg.seq;
                ui.playhead_ui = msg.outputs.playhead_ui;
                if (msg.state)
                    applyState(msg.state);
                outputCounts.playhead_ui += 1;
                pushLog("ui", `[ui #${msg.seq}] playhead_ui = ${fmt(msg.outputs.playhead_ui)}`);
                break;
            }

            case "audio": {
                audio.k = msg.k;
                audio.block = msg.block;
                audio.playhead = msg.outputs.playhead;
                audio.midi_count = msg.outputs.midi_count;
                audio.midi_offset = msg.outputs.midi_offset;
                audio.midi_off_count = msg.outputs.midi_off_count;
                audio.voice_gain = msg.outputs.voice_gain;
                outputCounts.playhead += 1;
                outputCounts.midi_count += 1;
                outputCounts.midi_offset += 1;
                outputCounts.midi_off_count += 1;
                outputCounts.voice_gain += 1;
                pushLog(
                    "audio",
                    `[audio #${msg.k}] playhead = ${fmt(audio.playhead)}  on = ${fmt(audio.midi_count, 0)}  off = ${fmt(audio.midi_off_count, 0)}  offset = ${fmt(audio.midi_offset, 0)}  gain = ${fmt(audio.voice_gain)}  · smp ${msg.block.start}+${msg.block.n}`
                );
                if (num(audio.midi_count) !== null && audio.midi_count > 0)
                    flashNote();
                break;
            }

            case "block": {
                controls.blockN = msg.block.n;
                controls.rate = msg.block.rate;
                syncWidgets();
                break;
            }

            case "state":
                applyState(msg.state);
                break;

            case "error": {
                badge("engine: error", "err");
                pushLog("err", `error: ${msg.text}`);
                break;
            }

            default:
                break;
        }
    };

    worker.onerror = (event) => {
        badge("engine: dead", "err");
        pushLog("err", `worker failed: ${event.message}`);
    };

    function badge(text, cls) {
        const el = $("engine-badge");
        el.textContent = text;
        el.className = "badge" + (cls ? " " + cls : "");
    }

    function pushLog(kind, text) {
        log.queue.push({ kind, text });
    }

    // ---------- command senders ----------------------------------------------

    function setPlaying(value) {
        send({ type: "set", port: "playing", value: Boolean(value) });
    }

    function setBpm(value) {
        if (!Number.isFinite(value))
            return; // a cleared field is not a bpm — keep the last level
        send({ type: "set", port: "bpm", value: clamp(Math.round(value), 40, 240) });
    }

    function setMaster(value) {
        if (!Number.isFinite(value))
            return;
        send({ type: "set", port: "master", value: clamp(value, 0, 1) });
    }

    function commitLoop(loop) {
        const start = Math.max(0, loop.start);
        const end = Math.max(start + SNAP, loop.end);
        send({ type: "loop", start, end, on: Boolean(loop.on) });
    }

    // The network-panel loop fields are the current edit buffer; reading all
    // three avoids losing a sibling field that has not echoed yet.
    function loopFromPanel() {
        const start = parseFloat(inputWidgets.loop.start.value);
        const end = parseFloat(inputWidgets.loop.end.value);

        return {
            start: Number.isFinite(start) ? start : controls.loop.start,
            end: Number.isFinite(end) ? end : controls.loop.end,
            on: inputWidgets.loop.on.checked,
        };
    }

    function fireSeek(beats) {
        seekPending = beats;
        $("seek-mark").hidden = false;
        send({ type: "seek", beats });
    }

    function setBlockParams(n, rate) {
        send({ type: "blockParams", n, rate });
    }

    // ---------- ports table ---------------------------------------------------

    function cell(row, className, content) {
        const td = document.createElement("td");
        td.className = className;
        if (content !== undefined)
            td.append(content);
        row.append(td);
        return td;
    }

    function textCell(row, className, text) {
        const td = cell(row, className);
        td.textContent = text;
        return td;
    }

    function makeNumber(value, step, onChange, width) {
        const input = document.createElement("input");
        input.type = "number";
        input.step = String(step);
        input.value = String(value);
        if (width)
            input.style.width = width;
        input.addEventListener("change", () => {
            const parsed = parseFloat(input.value);
            if (Number.isFinite(parsed))
                onChange(parsed);
            // No local sync here: the host echo owns the value now.
        });
        return input;
    }

    function makeCheckbox(value, onChange) {
        const input = document.createElement("input");
        input.type = "checkbox";
        input.checked = value;
        input.addEventListener("change", () => onChange(input.checked));
        return input;
    }

    function makeButton(label, onClick, title) {
        const button = document.createElement("button");
        button.className = "mid-btn";
        button.textContent = label;
        if (title)
            button.title = title;
        button.addEventListener("click", onClick);
        return button;
    }

    const inputWidgets = {};
    const outputValueCells = {};
    const outputArrivalCells = {};
    const outputCounts = {
        playhead: 0,
        midi_count: 0,
        midi_offset: 0,
        midi_off_count: 0,
        voice_gain: 0,
        playhead_ui: 0,
    };

    function buildPortsTables() {
        buildInputs();
        buildOutputs();
    }

    function buildInputs() {
        const body = $("inputs-table").querySelector("tbody");

        const row = (name, type, clock) => {
            const tr = document.createElement("tr");
            textCell(tr, "port-name", name);
            textCell(tr, "port-type", type);
            textCell(tr, "port-clock", clock);
            const widget = cell(tr, "port-widget");
            const value = textCell(tr, "port-value", "—");
            body.append(tr);
            return { widget, value };
        };

        {
            const r = row("playing", "bool", "ui");
            const box = makeCheckbox(controls.playing, setPlaying);
            r.widget.append(box);
            inputWidgets.playing = { value: r.value, box };
        }

        {
            const r = row("seek", "optional<double>", "ui");
            const group = document.createElement("span");
            group.className = "widget-group";
            const fire = () => {
                const parsed = parseFloat(field.value);
                if (Number.isFinite(parsed))
                    fireSeek(clamp(parsed, 0, 1e6));
            };
            const field = makeNumber(SCORE_BEAT, SNAP, fire);
            inputWidgets.seek = { value: r.value, field };
            group.append(field);
            group.append(
                makeButton("seek", fire, "Fire the queued seek event (one ui epoch, one audio epoch)")
            );
            r.widget.append(group);
        }

        {
            const r = row("loop", "LoopRegion", "ui");
            const group = document.createElement("span");
            group.className = "widget-group";
            const start = makeNumber(controls.loop.start, SNAP, (v) => {
                commitLoop({ ...loopFromPanel(), start: v });
            }, "4rem");
            const end = makeNumber(controls.loop.end, SNAP, (v) => {
                commitLoop({ ...loopFromPanel(), end: v });
            }, "4rem");
            const on = makeCheckbox(controls.loop.on, (v) => {
                commitLoop({ ...loopFromPanel(), on: v });
            });
            inputWidgets.loop = { value: r.value, start, end, on };
            group.append(start, document.createTextNode(" … "), end, on);
            r.widget.append(group);
        }

        {
            const r = row("bpm", "double", "ui");
            r.widget.append(makeNumber(controls.bpm, 1, setBpm, "4.5rem"));
            inputWidgets.bpm = { value: r.value };
        }

        {
            const r = row("master", "double", "ui");
            r.widget.append(makeNumber(controls.master, 0.01, setMaster, "4.5rem"));
            inputWidgets.master = { value: r.value };
        }

        {
            const r = row("block", "BlockDesc", "audio");
            const group = document.createElement("span");
            group.className = "widget-group";
            const sizes = document.createElement("select");
            [128, 256, 512, 1024, 2048, 4096].forEach((n) => {
                const option = document.createElement("option");
                option.value = String(n);
                option.textContent = String(n);
                sizes.append(option);
            });
            sizes.value = String(controls.blockN);
            const rates = document.createElement("select");
            [44100, 48000, 96000].forEach((rate) => {
                const option = document.createElement("option");
                option.value = String(rate);
                option.textContent = String(rate);
                rates.append(option);
            });
            rates.value = String(controls.rate);
            sizes.addEventListener("change", () => setBlockParams(parseInt(sizes.value, 10), parseInt(rates.value, 10)));
            rates.addEventListener("change", () => setBlockParams(parseInt(sizes.value, 10), parseInt(rates.value, 10)));
            inputWidgets.block = { value: r.value, sizes, rates };
            group.append(sizes, document.createTextNode(" @ "), rates);
            r.widget.append(group);
        }
    }

    function buildOutputs() {
        const body = $("outputs-table").querySelector("tbody");

        const outputs = [
            ["playhead", "double", "audio"],
            ["midi_count", "int64", "audio"],
            ["midi_offset", "int64", "audio"],
            ["midi_off_count", "int64", "audio"],
            ["voice_gain", "double", "audio"],
            ["playhead_ui", "double", "ui"],
        ];

        for (const [name, type, clock] of outputs) {
            const tr = document.createElement("tr");
            textCell(tr, "port-name", name);
            textCell(tr, "port-type", type);
            textCell(tr, "port-clock", clock);
            const arrivals = textCell(tr, "port-widget dim", "× 0");
            const value = textCell(tr, "port-value", "—");
            body.append(tr);
            outputValueCells[name] = value;
            outputArrivalCells[name] = arrivals;
        }
    }

    // ---------- timeline ------------------------------------------------------

    const tl = { beats: 8, drag: null };

    const pct = (beat) => clamp(beat / tl.beats, 0, 1) * 100;

    function eventBeat(event) {
        const rect = els.lane.getBoundingClientRect();
        return clamp((event.clientX - rect.left) / rect.width, 0, 1) * tl.beats;
    }

    const snap = (beat) => Math.round(beat / SNAP) * SNAP;

    function rebuildRuler() {
        const beatPct = 100 / tl.beats;
        const subPct = beatPct / 4;
        const barPct = beatPct * 4;
        const grid = (period, alpha) =>
            `repeating-linear-gradient(90deg, rgba(215,219,228,${alpha}) 0, rgba(215,219,228,${alpha}) 1px, transparent 1px, transparent ${period}%)`;
        const background = [
            "background-image:",
            grid(subPct, 0.05) + ",",
            grid(beatPct, 0.10) + ",",
            grid(barPct, 0.22) + ";",
        ].join(" ");
        els.ruler.style.cssText = background;
        els.lane.style.cssText = background;

        const stride = tl.beats <= 16 ? 1 : tl.beats <= 64 ? 4 : tl.beats <= 256 ? 16 : 32;
        els.ruler.innerHTML = "";
        for (let beat = 0; beat <= tl.beats; beat += stride) {
            const span = document.createElement("span");
            span.className = "ruler-num" + (beat % 4 === 0 ? " bar" : "");
            span.textContent = String(beat);
            span.style.left = pct(beat) + "%";
            els.ruler.append(span);
        }
    }

    function layoutTimeline() {
        const loop = loopPreview ?? controls.loop;
        els.loopBand.classList.toggle("off", !loop.on);
        els.loopBand.style.left = pct(loop.start) + "%";
        els.loopBand.style.width = Math.max(0, pct(loop.end) - pct(loop.start)) + "%";
        els.handleL.style.left = pct(loop.start) + "%";
        els.handleR.style.left = pct(loop.end) + "%";

        els.note.style.left = pct(SCORE_BEAT) + "%";
        els.note.style.width = Math.max(0, pct(SCORE_BEAT + SCORE_LEN) - pct(SCORE_BEAT)) + "%";

        const ghost = num(audio.playhead);
        els.ghostCursor.style.display = ghost === null ? "none" : "block";
        els.ghostCursor.style.left = pct(ghost ?? 0) + "%";

        const cursor = num(ui.playhead_ui);
        els.cursor.style.display = cursor === null ? "none" : "block";
        els.cursor.style.left = pct(cursor ?? 0) + "%";

        if (seekPending !== null) {
            els.seekMark.hidden = false;
            els.seekMark.style.left = pct(seekPending) + "%";
        } else {
            els.seekMark.hidden = true;
        }
    }

    function updateWindow() {
        const need = Math.max(
            8,
            Math.ceil((num(audio.playhead) ?? 0) + 1),
            Math.ceil((num(ui.playhead_ui) ?? 0) + 1),
            Math.ceil((loopPreview ?? controls.loop).end),
            Math.ceil(SCORE_BEAT + SCORE_LEN) // the note block stays in view
        );
        if (need > tl.beats) {
            tl.beats = need;
            rebuildRuler();
        }
    }

    function timelinePointers() {
        const startDrag = (mode, event) => {
            tl.drag = {
                mode,
                grab: snap(eventBeat(event)),
                orig: { ...controls.loop },
            };
            loopPreview = { ...controls.loop };
            event.preventDefault();
        };

        els.lane.addEventListener("pointerdown", (event) => {
            if (event.target === els.loopBand)
                startDrag("move", event);
            else if (event.target === els.handleL)
                startDrag("left", event);
            else if (event.target === els.handleR)
                startDrag("right", event);
            else
                fireSeek(snap(eventBeat(event)));
        });

        els.ruler.addEventListener("pointerdown", (event) => {
            fireSeek(snap(eventBeat(event)));
        });

        window.addEventListener("pointermove", (event) => {
            if (tl.drag === null)
                return;
            const beat = snap(eventBeat(event));
            const { mode, grab, orig } = tl.drag;

            let next;

            if (mode === "move") {
                const delta = beat - grab;
                next = {
                    start: Math.max(0, orig.start + delta),
                    end: Math.max(0, orig.end + delta),
                    on: orig.on,
                };
            } else if (mode === "left") {
                next = { ...orig, start: Math.min(beat, orig.end - SNAP) };
            } else {
                next = { ...orig, end: Math.max(beat, orig.start + SNAP) };
            }

            // The band follows the local preview while the pointer is down; the
            // command still goes through the host, and the echo owns the value
            // again after pointerup.
            loopPreview = next;
            commitLoop(next);
        });

        window.addEventListener("pointerup", () => {
            tl.drag = null;
            loopPreview = null;
        });
    }

    function flashNote() {
        els.note.classList.add("fired");
        window.setTimeout(() => els.note.classList.remove("fired"), 220);
    }

    // ---------- widget sync ---------------------------------------------------

    // The host is the single source of truth for every domain control. This
    // copies a state snapshot into the render model and updates the widgets;
    // gesture handlers below only send commands, they never mutate `controls`.
    // `loopPreview` is the one render-only exception: the band follows a drag
    // while the pointer is down; the host echo owns the value afterwards.
    function applyState(next, log = true) {
        if (!next)
            return;

        const changes = [];

        if (typeof next.playing === "boolean" && next.playing !== controls.playing) {
            controls.playing = next.playing;
            changes.push(`playing ${next.playing ? "on" : "off"}`);
        }

        if (typeof next.bpm === "number" && Number.isFinite(next.bpm) && Math.abs(next.bpm - controls.bpm) > 1e-9) {
            controls.bpm = next.bpm;
            changes.push(`bpm ${fmt(next.bpm, 0)}`);
        }

        if (typeof next.master === "number" && Number.isFinite(next.master) && Math.abs(next.master - controls.master) > 1e-9) {
            controls.master = next.master;
            changes.push(`master ${fmt(next.master, 2)}`);
        }

        if (next.loop) {
            const start = Number(next.loop.start);
            const end = Number(next.loop.end);
            const on = next.loop.on === true;

            if (Number.isFinite(start) && Number.isFinite(end)) {
                if (Math.abs(start - controls.loop.start) > 1e-9
                    || Math.abs(end - controls.loop.end) > 1e-9
                    || on !== controls.loop.on) {
                    controls.loop = { start, end, on };
                    changes.push(`loop ${fmt(start, 2)}…${fmt(end, 2)} ${on ? "on" : "off"}`);
                }
            }
        }

        if (next.block) {
            const n = Number(next.block.n);
            const rate = Number(next.block.rate);

            if (Number.isFinite(n) && Number.isFinite(rate) && n > 0
                && (n !== controls.blockN || Math.abs(rate - controls.rate) > 1e-9)) {
                controls.blockN = n;
                controls.rate = rate;
                changes.push(`block ${n} @ ${rate}`);
            }
        }

        if (next.auto) {
            const ui = next.auto.ui === true;
            const audio = next.auto.audio === true;

            if (ui !== controls.autoUi) {
                controls.autoUi = ui;
                changes.push(`auto ui ${ui ? "on" : "off"}`);
            }

            if (audio !== controls.autoAudio) {
                controls.autoAudio = audio;
                changes.push(`auto audio ${audio ? "on" : "off"}`);
            }
        }

        if (!haveState || changes.length > 0) {
            syncWidgets();
            haveState = true;
        }

        if (log && changes.length > 0)
            pushLog("ui", `controls: ${changes.join(", ")}`);
    }

    function syncWidgets() {
        $("play-btn").textContent = controls.playing ? "Pause" : "Play";
        $("play-btn").classList.toggle("on", controls.playing);

        $("bpm-slider").value = String(controls.bpm);
        $("bpm-input").value = String(controls.bpm);

        $("master-slider").value = String(controls.master);
        $("master-value").textContent = controls.master.toFixed(2);

        $("loop-btn").classList.toggle("on", controls.loop.on);
        $("loop-start").value = String(controls.loop.start);
        $("loop-end").value = String(controls.loop.end);

        inputWidgets.playing.box.checked = controls.playing;
        $("auto-ui").checked = controls.autoUi;
        $("auto-audio").checked = controls.autoAudio;
        if (seekPending !== null)
            inputWidgets.seek.field.value = String(seekPending);
        inputWidgets.loop.start.value = String(controls.loop.start);
        inputWidgets.loop.end.value = String(controls.loop.end);
        inputWidgets.loop.on.checked = controls.loop.on;
        inputWidgets.block.sizes.value = String(controls.blockN);
        inputWidgets.block.rates.value = String(controls.rate);
    }

    // ---------- render loop ----------------------------------------------------

    const beatsToBars = (beat) => {
        const bar = Math.floor(beat / 4) + 1;
        const inBar = Math.floor(beat % 4) + 1;
        const sixteenth = Math.floor((beat % 1) * 4) + 1;
        return `${bar}.${inBar}.${sixteenth}`;
    };

    function render() {
        updateWindow();
        layoutTimeline();

        const cursor = num(ui.playhead_ui);
        $("pos-bars").textContent = cursor === null ? "—" : beatsToBars(cursor);
        $("pos-beats").textContent = cursor === null ? "—" : cursor.toFixed(3) + " beats";

        const gain = num(audio.voice_gain);
        $("voice-meter").style.width = (gain ?? 0) * 100 + "%";
        $("voice-value").textContent = fmt(gain, 2);
        els.note.classList.toggle("active", (gain ?? 0) > 0);

        $("ui-ticks").textContent = `ticks ${ui.seq}`;
        $("audio-ticks").textContent = `blocks ${audio.k}`;
        $("audio-hz").textContent =
            `${(controls.rate / controls.blockN).toFixed(1)} Hz · ${controls.blockN} @ ${controls.rate / 1000}k`;

        outputValueCells.playhead.textContent = fmt(audio.playhead);
        outputValueCells.midi_count.textContent = fmt(audio.midi_count, 0);
        outputValueCells.midi_offset.textContent = fmt(audio.midi_offset, 0);
        outputValueCells.midi_off_count.textContent = fmt(audio.midi_off_count, 0);
        outputValueCells.voice_gain.textContent = fmt(audio.voice_gain, 2);
        outputValueCells.playhead_ui.textContent = fmt(ui.playhead_ui);

        for (const name of Object.keys(outputArrivalCells))
            outputArrivalCells[name].textContent = `× ${outputCounts[name]}`;

        inputWidgets.playing.value.textContent = String(controls.playing);
        inputWidgets.seek.value.textContent = seekPending === null ? "event" : `queued ${fmt(seekPending, 2)}`;
        inputWidgets.loop.value.textContent = `${fmt(controls.loop.start, 2)} … ${fmt(controls.loop.end, 2)} ${controls.loop.on ? "on" : "off"}`;
        inputWidgets.bpm.value.textContent = fmt(controls.bpm, 0);
        inputWidgets.master.value.textContent = fmt(controls.master, 2);
        const nextStart = audio.block === null ? 0 : audio.block.start + audio.block.n;
        inputWidgets.block.value.textContent = `next smp ${nextStart}`;

        // A pending seek is consumed once either playhead lands on it.
        if (seekPending !== null) {
            const cur = num(ui.playhead_ui);
            const raw = num(audio.playhead);
            const landed = (x) => x !== null && Math.abs(x - seekPending) < 0.01;
            if (landed(cur) || landed(raw))
                seekPending = null;
        }

        flushLog();
        window.requestAnimationFrame(render);
    }

    function flushLog() {
        const logEl = els.log;

        if (log.queue.length === 0)
            return;

        if (log.paused) {
            log.queue.length = 0; // a live view: paused drops, it does not bank
            return;
        }

        const nearBottom =
            logEl.scrollHeight - logEl.scrollTop - logEl.clientHeight < 48;

        const fragment = document.createDocumentFragment();
        for (const line of log.queue) {
            if (log.filter !== "all" && log.filter !== line.kind)
                continue;
            const div = document.createElement("div");
            div.className = "log-line " + line.kind;
            div.textContent = line.text;
            fragment.append(div);
        }
        log.queue.length = 0;
        logEl.append(fragment);

        while (logEl.childElementCount > 400)
            logEl.firstElementChild.remove();

        if (nearBottom)
            logEl.scrollTop = logEl.scrollHeight;
    }

    // ---------- static wiring ---------------------------------------------------

    function wireTransportBar() {
        $("play-btn").addEventListener("click", () => setPlaying(!controls.playing));
        $("rtz-btn").addEventListener("click", () => fireSeek(0));

        $("bpm-slider").addEventListener("input", () => setBpm(parseFloat($("bpm-slider").value)));
        $("bpm-input").addEventListener("change", () => setBpm(parseFloat($("bpm-input").value)));

        $("master-slider").addEventListener("input", () => setMaster(parseFloat($("master-slider").value)));

        $("loop-btn").addEventListener("click", () => {
            commitLoop({ ...controls.loop, on: !controls.loop.on });
        });
        $("loop-start").addEventListener("change", () => {
            commitLoop({
                start: parseFloat($("loop-start").value) || 0,
                end: parseFloat($("loop-end").value) || controls.loop.end,
                on: controls.loop.on,
            });
        });
        $("loop-end").addEventListener("change", () => {
            commitLoop({
                start: parseFloat($("loop-start").value) || controls.loop.start,
                end: parseFloat($("loop-end").value) || 0,
                on: controls.loop.on,
            });
        });

        document.addEventListener("keydown", (event) => {
            if (event.code !== "Space")
                return;
            const target = event.target;
            if (target && ["INPUT", "BUTTON", "SELECT", "TEXTAREA"].includes(target.tagName))
                return;
            event.preventDefault();
            setPlaying(!controls.playing);
        });
    }

    function wireNetworkPanel() {
        $("step-ui-btn").addEventListener("click", () => send({ type: "stepUi" }));
        $("step-audio-btn").addEventListener("click", () => send({ type: "stepAudio" }));

        $("auto-ui").addEventListener("change", () => {
            send({ type: "autoUi", on: $("auto-ui").checked });
        });
        $("auto-audio").addEventListener("change", () => {
            send({ type: "autoAudio", on: $("auto-audio").checked });
        });

        $("reset-btn").addEventListener("click", () => {
            badge("engine: rebuilding");
            send({ type: "reset" });
        });

        const chips = document.querySelectorAll(".log-controls .chip[data-filter]");
        chips.forEach((chip) => {
            chip.addEventListener("click", () => {
                log.filter = chip.dataset.filter;
                chips.forEach((c) => c.classList.toggle("sel", c === chip));
            });
        });
        chips[0].classList.add("sel");

        $("log-pause").addEventListener("click", () => {
            log.paused = !log.paused;
            $("log-pause").textContent = log.paused ? "resume" : "pause";
            $("log-pause").classList.toggle("sel", log.paused);
        });

        $("log-clear").addEventListener("click", () => {
            els.log.innerHTML = "";
        });
    }

    function cacheElements() {
        els.lane = $("lane");
        els.ruler = $("ruler");
        els.loopBand = $("loop-band");
        els.handleL = $("loop-handle-l");
        els.handleR = $("loop-handle-r");
        els.note = $("note");
        els.cursor = $("cursor");
        els.ghostCursor = $("ghost-cursor");
        els.seekMark = $("seek-mark");
        els.log = $("log");
    }

    // ---------- boot -----------------------------------------------------------

    cacheElements();
    buildPortsTables();
    rebuildRuler();
    timelinePointers();
    wireTransportBar();
    wireNetworkPanel();
    syncWidgets();
    pushLog("sys", "waiting for the engine…");
    window.requestAnimationFrame(render);
})();
