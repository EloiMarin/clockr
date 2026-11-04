// Node smoke test for the embind shell: drives the §6.1 acceptance script
// through the same calls the demo page makes (Dawmulator factory from
// daw_shell.js, whose directory arrives as argv[2]). Exact integer-valued
// doubles throughout — strict equality is intended.
const factory = require(require("path").resolve(process.argv[2], "daw_shell.js"));

factory().then((shell) => {
    const demo = new shell.Dawmulator();

    // Before any tick the outputs read NaN (monostate under the hood), never
    // throw — a shell may poll freely.
    if (!Number.isNaN(demo.playhead()))
        throw new Error("fresh program's playhead should read NaN");
    if (!Number.isNaN(demo.playheadUi()))
        throw new Error("fresh program's playhead_ui should read NaN");

    demo.setPlaying(true);
    demo.setBpm(120.0);
    demo.setLoop(0.0, 4.0, true);
    demo.setMaster(0.8);
    demo.tickUi();

    // The input getters expose the boundary state the page's controlled
    // widgets render from.
    if (demo.playing() !== true || demo.bpm() !== 120.0 || demo.master() !== 0.8
        || demo.loopStart() !== 0.0 || demo.loopEnd() !== 4.0 || demo.loopOn() !== true)
        throw new Error("ui input read-back does not match the setters");

    // A ui tick alone does not produce audio outputs.
    if (!Number.isNaN(demo.playhead()))
        throw new Error("ui tick must not produce audio outputs");

    const playheads = [1.0, 2.0, 3.0, 0.0, 1.0];
    let failed = false;

    const check = (what, got, want) => {
        const ok = got === want;
        if (!ok)
            failed = true;
        console.log(`${ok ? "ok" : "FAIL"} ${what}: got=${got} want=${want}`);
    };

    for (let k = 0; k < 5; k += 1) {
        demo.setBlock(k * 24000, 24000, 48000.0);
        demo.tickAudio();
        check(`block ${k} playhead`, demo.playhead(), playheads[k]);
        check(`block ${k} on count`, demo.midiCount(), k === 3 ? 1 : 0);
        check(`block ${k} offset`, demo.midiOffset(), k === 3 ? 12000 : -1);
        // The note is one beat long, so its off crossing wraps to beat 0.5:
        // crossed before any note (block 0, a no-op on the silent voice)
        // and right after the note (block 4, the release).
        check(`block ${k} off count`, demo.midiOffCount(), k === 0 || k === 4 ? 1 : 0);
        check(`block ${k} gain`, demo.voiceGain(), k === 3 ? 0.8 : 0.0);
    }

    // The playhead crossed back to ui: the latest audio sample, read at ui
    // cadence. Another ui tick after quiet audio holds the same sample.
    demo.tickUi();
    check("ui playhead samples latest", demo.playheadUi(), 1.0);
    demo.tickUi();
    check("ui playhead holds when audio is quiet", demo.playheadUi(), 1.0);

    // Seek is a queued event: set, one ui epoch, one audio epoch — the jump
    // lands even while paused (§7: seek-while-paused).
    demo.setPlaying(false);
    demo.setSeek(2.5);
    demo.tickUi();
    demo.setBlock(5 * 24000, 24000, 48000.0);
    demo.tickAudio();
    check("seek lands while paused", demo.playhead(), 2.5);
    demo.tickUi();
    check("cursor follows the seek", demo.playheadUi(), 2.5);

    demo.delete();
    process.exit(failed ? 1 : 0);
}).catch((err) => {
    console.error(`module load failed: ${err}`);
    process.exit(1);
});
