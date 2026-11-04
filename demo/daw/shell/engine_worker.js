// engine_worker.js — the demo's simulated threads.
//
// The whole engine (the wasm module and its Dawmulator) lives in this worker;
// the main thread is pure UI — exactly the split a real host has between an
// audio thread and a UI. Two clocks tick here:
//
//   audio — a wall-clock-locked block scheduler. Each pump ticks as many
//           blocks as wall time owes since the "card" started, so engine
//           time tracks the clock the way a real audio callback would; if
//           the timer was starved for too long (a throttled tab), the card
//           resyncs instead of bursting a backlog of blocks.
//   ui    — a plain message loop at UI_HZ.
//
// The ui region is also stepped manually (stepUi); when the audio region
// has never ticked, the playhead_ui link still carries its initial
// monostate and the getter reports NaN — the page renders "not yet".
//
// Protocol (main -> worker):
//   {type:"set", port, value}         playing | bpm | master (levels)
//   {type:"loop", start, end, on}      the LoopRegion struct port
//   {type:"seek", beats}               queued event (beats == null clears it)
//   {type:"blockParams", n, rate}     BlockDesc knobs; the sample counter
//                                      stays engine-owned
//   {type:"stepUi"} {type:"stepAudio"}   manual single steps
//   {type:"autoUi", on} {type:"autoAudio", on}
//   {type:"reset"}
//
// Protocol (worker -> main):
//   {type:"ready", defaults, block, uiHz, outputs, state}
//   {type:"ui", seq, outputs, state}
//   {type:"audio", k, block, outputs}
//   {type:"state", state}                 controlled-widget snapshot
//   {type:"block", block}
//   {type:"error", text}
//
// Dual-mode by design: inside a worker it bootstraps through importScripts;
// under node it exports createDawEngine so the smoke test can drive the exact
// protocol the page speaks against the real wasm module.

/*global importScripts, postMessage, onmessage, performance */

(function (global) {
    "use strict";

    const UI_HZ = 60;
    const MAX_BURST = 32; // blocks of catch-up before the card resyncs

    function createDawEngine(moduleFactory, post) {
        return moduleFactory().then((shell) => {
            let demo = null;
            let blocksDone = 0; // audio blocks ticked since construction
            let blockN = 1024; // BlockDesc::n_frames
            let blockRate = 48000.0;
            let uiSeq = 0;
            let uiTimer = null;
            let audioTimer = null;
            let autoUiWanted = true; // the page's initial auto preference; the
            let autoAudioWanted = true; // timers follow once the page sends it
            let audioT0 = 0; // performance.now() the card started at
            let debugWanted = false; // a local debugger asked for the trace

            const blockPeriodMs = () => (blockN / blockRate) * 1000.0;

            function audioOutputs() {
                return {
                    playhead: demo.playhead(),
                    midi_count: demo.midiCount(),
                    midi_offset: demo.midiOffset(),
                    midi_off_count: demo.midiOffCount(),
                    voice_gain: demo.voiceGain(),
                };
            }

            function uiOutputs() {
                return { playhead_ui: demo.playheadUi() };
            }

            // The domain state the page renders its widgets from. The ui region
            // is single-threaded here, so these are the boundary values the
            // next tick will consume; setters and ticks post a fresh snapshot.
            function controlState() {
                return {
                    playing: demo.playing(),
                    bpm: demo.bpm(),
                    master: demo.master(),
                    loop: { start: demo.loopStart(), end: demo.loopEnd(), on: demo.loopOn() },
                    block: { n: blockN, rate: blockRate },
                    auto: { ui: autoUiWanted, audio: autoAudioWanted },
                };
            }

            function postState() {
                post({ type: "state", state: controlState() });
            }

            function postReady() {
                post({
                    type: "ready",
                    defaults: {
                        playing: false,
                        bpm: 120.0,
                        master: 0.8,
                        loop: { start: 0.0, end: 4.0, on: true },
                    },
                    block: { n: blockN, rate: blockRate },
                    uiHz: UI_HZ,
                    outputs: { ui: uiOutputs(), audio: audioOutputs() },
                    state: controlState(),
                });
            }

            function construct() {
                if (demo !== null)
                    demo.delete();

                demo = new shell.Dawmulator();
                blocksDone = 0;
                uiSeq = 0;

                // Init invariant (same ordering the C++ tests assume): the
                // ui ports carry levels and the ui region has ticked once
                // before any audio tick — a latest link holds monostate
                // until its producer's first tick, and the bundle lift
                // reads the crossed levels. One call: the levels and the
                // tick live in daw/dawmulator.hpp, shared with the
                // debugger's `daw` example so the two cannot drift apart.
                demo.initDefaults();

                // A reset rebuilds the shells; a debugger that was attached
                // across it keeps its engine traced.
                if (debugWanted)
                    demo.debugCapture(true);

                if (audioTimer !== null)
                    audioT0 = performance.now();

                postReady();

                if (typeof api.onDebugReset === "function")
                    api.onDebugReset();
            }

            function stepUi() {
                demo.tickUi();
                uiSeq += 1;
                post({ type: "ui", seq: uiSeq, outputs: uiOutputs(), state: controlState() });
            }

            function stepAudio() {
                demo.setBlock(blocksDone * blockN, blockN, blockRate);
                demo.tickAudio();
                blocksDone += 1;
                post({
                    type: "audio",
                    k: blocksDone,
                    block: {
                        start: (blocksDone - 1) * blockN,
                        n: blockN,
                        rate: blockRate,
                    },
                    outputs: audioOutputs(),
                });
            }

            function setBlockParams(n, rate) {
                blockN = n;
                blockRate = rate;

                // A new block length is a new card: keep the done-block
                // count, restart wall-clock tracking for it.
                if (audioTimer !== null)
                    audioT0 = performance.now() - blocksDone * blockPeriodMs();

                post({ type: "block", block: { n: blockN, rate: blockRate } });
                postState();
            }

            function audioPump() {
                const target = Math.floor((performance.now() - audioT0) / blockPeriodMs());

                if (target - blocksDone > MAX_BURST) {
                    // Starved for far too long: resync the card rather than
                    // replay a backlog of blocks.
                    audioT0 = performance.now() - blocksDone * blockPeriodMs();
                    return;
                }

                while (blocksDone < target)
                    stepAudio();
            }

            function setAutoUi(on) {
                autoUiWanted = on;

                if (uiTimer !== null) {
                    clearInterval(uiTimer);
                    uiTimer = null;
                }

                if (on)
                    uiTimer = setInterval(stepUi, 1000.0 / UI_HZ);

                postState();
            }

            function setAutoAudio(on) {
                autoAudioWanted = on;

                if (audioTimer !== null) {
                    clearInterval(audioTimer);
                    audioTimer = null;
                }

                if (on) {
                    // Back-date the card start so a stopped/resumed engine
                    // continues seamlessly instead of jumping.
                    audioT0 = performance.now() - blocksDone * blockPeriodMs();
                    audioTimer = setInterval(
                        audioPump,
                        Math.max(4, blockPeriodMs() / 4));
                }

                postState();
            }

            function handle(cmd) {
                try {
                    switch (cmd.type) {
                        case "set":
                            if (cmd.port === "playing")
                                demo.setPlaying(cmd.value);
                            else if (cmd.port === "bpm")
                                demo.setBpm(cmd.value);
                            else if (cmd.port === "master")
                                demo.setMaster(cmd.value);
                            else
                                throw new Error("unknown port: " + cmd.port);
                            postState();
                            break;

                        case "loop":
                            demo.setLoop(cmd.start, cmd.end, cmd.on);
                            postState();
                            break;

                        case "seek":
                            if (cmd.beats === null || cmd.beats === undefined)
                                demo.clearSeek();
                            else
                                demo.setSeek(cmd.beats);
                            break;

                        case "blockParams":
                            setBlockParams(cmd.n, cmd.rate);
                            break;

                        case "stepUi":
                            stepUi();
                            break;

                        case "stepAudio":
                            stepAudio();
                            break;

                        case "autoUi":
                            setAutoUi(cmd.on);
                            break;

                        case "autoAudio":
                            setAutoAudio(cmd.on);
                            break;

                        case "reset":
                            construct();
                            break;

                        default:
                            throw new Error("unknown command: " + cmd.type);
                    }
                } catch (err) {
                    post({
                        type: "error",
                        text: String(err && err.message ? err.message : err),
                    });
                }
            }

            function setDebugCapture(on) {
                debugWanted = Boolean(on);

                if (demo !== null)
                    demo.debugCapture(debugWanted);
            }

            function debugGraph() {
                return demo === null ? null : demo.debugGraph();
            }

            function debugDrain() {
                return demo === null ? [] : demo.debugDrain();
            }

            const api = {
                handle,
                setDebugCapture,
                debugGraph,
                debugDrain,
                onDebugReset: null,
            };

            construct();

            return api;
        });
    }

    // The local debugger transport: the DAW page's worker broadcasts the same
    // graph and event batches the native server sends over its WebSocket, on a
    // BroadcastChannel scoped to this origin. The debugger page (served from
    // the same static site) speaks the same protocol and never needs a server.
    // A bounded per-clock history lets a late or reloaded debugger replay the
    // recent past; the page still owns the full client-side timeline.
    function attachLocalDebug(engine) {
        if (typeof BroadcastChannel !== "function")
            return;

        const channel = new BroadcastChannel("clockr.debug.local");
        const history = new Map();
        const HISTORY_EVENTS = 20000;
        const DRAIN_MS = 33;

        let capturing = false;

        function remember(batch) {
            let events = history.get(batch.clock);

            if (events === undefined) {
                events = [];
                history.set(batch.clock, events);
            }

            for (const event of batch.events)
                events.push(event);

            if (events.length > HISTORY_EVENTS)
                events.splice(0, events.length - HISTORY_EVENTS);
        }

        function publish(batch) {
            remember(batch);
            channel.postMessage({ type: "batch", batch });
        }

        function flush() {
            if (!capturing)
                return;

            for (const batch of engine.debugDrain())
                publish(batch);
        }

        function announce() {
            channel.postMessage({ type: "welcome" });
            channel.postMessage({
                type: "examples",
                examples: { names: ["dawmulator"], active: "dawmulator" },
            });
            channel.postMessage({ type: "graph", graph: engine.debugGraph() });

            for (const [clock, events] of history)
                if (events.length > 0)
                    channel.postMessage({
                        type: "batch",
                        batch: { clock, dropped: 0, events: events.slice() },
                    });
        }

        function handle(message) {
            if (message === null || typeof message !== "object")
                return;

            switch (message.type) {
                case "hello":
                    capturing = true;
                    engine.setDebugCapture(true);
                    announce();
                    break;

                case "resume":
                    if (capturing)
                        break;
                    capturing = true;
                    engine.setDebugCapture(true);
                    break;

                case "pause":
                    if (!capturing)
                        break;
                    // Keep the last window before the release engines take over.
                    flush();
                    capturing = false;
                    engine.setDebugCapture(false);
                    break;

                default:
                    break;
            }
        }

        channel.onmessage = (event) => handle(event.data);

        engine.onDebugReset = () => {
            history.clear();

            if (capturing)
                announce();
        };

        setInterval(flush, DRAIN_MS);
    }

    if (typeof module !== "undefined" && module.exports) {
        // Node (the smoke test): the factory arrives from require().
        module.exports = { createDawEngine };
    }

    if (typeof importScripts === "function") {
        // The worker bootstrap: the module is a global from importScripts.
        importScripts("daw_shell.js");
        createDawEngine(global.DawShell, (msg) => postMessage(msg)).then((engine) => {
            onmessage = (event) => engine.handle(event.data);
            attachLocalDebug(engine);
        });
    }
})(typeof self !== "undefined" ? self : globalThis);
