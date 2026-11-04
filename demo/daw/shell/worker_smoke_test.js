// Node smoke test for the worker protocol: drives the exact command/event
// contract the page speaks, against the real wasm module (module directory
// arrives as argv[2], the worker script sits next to this file). The §6.1
// acceptance script rides through the same path the site's auto mode and
// buttons use.
const path = require("path");

const shellDir = process.argv[2];
const factory = require(path.resolve(shellDir, "daw_shell.js"));
const { createDawEngine } = require(path.resolve(__dirname, "engine_worker.js"));

let failed = false;

const check = (what, ok) => {
    if (!ok)
        failed = true;
    console.log(`${ok ? "ok" : "FAIL"} ${what}`);
};

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

(async () => {
    const events = [];
    const engine = await createDawEngine(factory, (msg) => events.push(msg));

    const ofType = (type) => events.filter((e) => e.type === type);

    check("ready posted on construction", ofType("ready").length === 1);
    check(
        "ready carries the DAWmulator defaults",
        ofType("ready")[0].defaults.playing === false
            && ofType("ready")[0].defaults.bpm === 120
            && ofType("ready")[0].defaults.master === 0.8
            && ofType("ready")[0].defaults.loop.start === 0
            && ofType("ready")[0].defaults.loop.end === 4
            && ofType("ready")[0].defaults.loop.on === true);
    check(
        "fresh outputs read NaN",
        Number.isNaN(ofType("ready")[0].outputs.ui.playhead_ui)
            && Number.isNaN(ofType("ready")[0].outputs.audio.playhead));
    check(
        "ready carries the controlled state",
        ofType("ready")[0].state?.playing === false
            && ofType("ready")[0].state?.bpm === 120
            && ofType("ready")[0].state?.loop?.on === true
            && ofType("ready")[0].state?.auto?.ui === true);

    // The in-browser debugger producer: a graph description in the same shape
    // the native server ships, and trace batches while capture is on.
    const graph = engine.debugGraph();
    check(
        "debug graph names the DAWmulator",
        graph?.name === "dawmulator" && graph.nodes.length > 0);
    check("debug graph has six input ports", graph.inputs.length === 6);
    check(
        "debug graph carries described user types",
        graph.types.some((type) => type.name === "daw.loop_region"));

    engine.setDebugCapture(true);

    // The §6.1 script: five 1-beat blocks at 120 bpm through the protocol.
    // Construction already ticked ui once with safe defaults, so these
    // intents wait for a ui epoch before the audio region can see them —
    // the same set -> tick ui -> tick audio order the C++ tests drive.
    engine.handle({ type: "set", port: "playing", value: true });
    engine.handle({ type: "set", port: "bpm", value: 120.0 });
    engine.handle({ type: "loop", start: 0.0, end: 4.0, on: true });
    engine.handle({ type: "set", port: "master", value: 0.8 });
    engine.handle({ type: "stepUi" });
    engine.handle({ type: "blockParams", n: 24000, rate: 48000.0 });

    check(
        "ui carries the controlled state",
        ofType("ui")[0]?.state?.playing === true
            && ofType("ui")[0]?.state?.bpm === 120);
    check(
        "state carries block params",
        ofType("state").at(-1)?.state?.block?.n === 24000);

    const playheads = [1.0, 2.0, 3.0, 0.0, 1.0];

    for (let k = 0; k < 5; k += 1)
        engine.handle({ type: "stepAudio" });

    const blocks = ofType("audio");
    check("five audio events", blocks.length === 5);
    blocks.forEach((e, k) => {
        const wantOn = k === 3 ? 1 : 0;
        const wantOff = k === 0 || k === 4 ? 1 : 0;
        const wantOffset = k === 3 ? 12000 : -1;
        const wantGain = k === 3 ? 0.8 : 0.0;
        check(`block ${k} playhead`, e.outputs.playhead === playheads[k]);
        check(`block ${k} on count`, e.outputs.midi_count === wantOn);
        check(`block ${k} off count`, e.outputs.midi_off_count === wantOff);
        check(`block ${k} offset`, e.outputs.midi_offset === wantOffset);
        check(`block ${k} gain`, e.outputs.voice_gain === wantGain);
        check(`block ${k} sample counter`, e.block.start === k * 24000);
    });

    // The playhead crossed back to ui, through the protocol.
    engine.handle({ type: "stepUi" });
    const ui = ofType("ui");
    check("ui events posted", ui.length === 2);
    check("ui playhead samples latest", ui[1].outputs.playhead_ui === 1.0);
    check("ui seq counts", ui[1].seq === 2);

    const batches = engine.debugDrain();
    const audioBatch = batches.find((batch) => batch.clock === "audio");
    check("debug drain has an audio batch", Boolean(audioBatch));
    check(
        "debug batch carries node evaluations",
        Boolean(audioBatch)
            && audioBatch.events.some((event) => event.kind === "node_tick_end"));
    check(
        "debug drain has a ui batch",
        batches.some((batch) => batch.clock === "ui"));
    check(
        "debug trace carries struct values",
        batches.some((batch) =>
            batch.events.some((event) =>
                event.value?.kind === "struct"
                && event.value.value.fields.length > 0)));

    // Auto mode: the card ticks on wall time. One beat blocks are far too
    // slow for a 150 ms burst, so switch the card to 1024 @ 48 kHz first.
    engine.handle({ type: "blockParams", n: 1024, rate: 48000.0 });
    engine.handle({ type: "autoAudio", on: true });
    engine.handle({ type: "autoUi", on: true });
    await sleep(150);
    engine.handle({ type: "autoAudio", on: false });
    engine.handle({ type: "autoUi", on: false });

    const after = ofType("audio").length;
    check("auto audio advanced", after > 5);
    check("auto ui advanced", ofType("ui").length > 1);

    // The audio clock stays wall-locked: no burst beyond the sleep window.
    check("no unbounded burst", after - 5 <= 16);

    const lastState = ofType("state").at(-1);
    check(
        "state echoes the auto flags",
        lastState?.state?.auto?.ui === false && lastState?.state?.auto?.audio === false);

    // Reset rebuilds the engine and reposts ready; a debugger attached across
    // the reset keeps its trace.
    engine.handle({ type: "reset" });
    check("ready posted on reset", ofType("ready").length === 2);
    engine.handle({ type: "stepUi" });
    engine.handle({ type: "stepAudio" });
    check(
        "debug capture survives reset",
        engine.debugDrain().some((batch) =>
            batch.events.some((event) => event.kind === "node_tick_end")));

    process.exit(failed ? 1 : 0);
})().catch((err) => {
    console.error(`worker protocol test failed: ${err}`);
    process.exit(1);
});
