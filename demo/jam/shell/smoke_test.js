const path = require("path");

const dir = process.argv[2];
const factory = require(path.resolve(dir, "jam_shell.js"));

let failed = false;

function check(what, ok, detail) {
    if (!ok) {
        failed = true;
        console.log(`FAIL ${what}${detail ? `: ${detail}` : ""}`);
    } else {
        console.log(`ok ${what}`);
    }
}

function parse(text, what) {
    try {
        return JSON.parse(text);
    } catch (error) {
        check(`${what} is valid JSON`, false, error.message);
        return null;
    }
}

factory().then((module) => {
    const shell = new module.JamShell();

    const score = parse(shell.defaultScoreJson(), "default score");
    check("default score has 16 patterns", score.patterns.length === 16);
    check("default score has 8 tracks", score.tracks.length === 8);
    check("default score bpm", score.bpm === 120);

    const init = parse(shell.init(120), "init");
    check("init succeeds", init.ok === true && init.initialized === true);

    const intents = parse(shell.setIntents(1, 120, 0, 4, 1, 0.8), "intents");
    check("intents apply", intents.ok === true && intents.intents.playing === true);
    check("intent read-back", intents.intents.bpm === 120
        && intents.intents.loop.start === 0
        && intents.intents.loop.end === 4
        && intents.intents.master === 0.8);

    const firstRun = parse(shell.step(8), "first step");
    check("step returns the requested blocks", firstRun.ticks.length === 8);
    check("block schedule advances", firstRun.block.index === 8);

    const baselineNotes = firstRun.ticks
        .flatMap((tick) => tick.events)
        .filter((event) => event.on)
        .map((event) => event.note);

    const before = parse(shell.getStateJson(), "state before patch");
    const originalNote = before.score.patterns[0].steps[0][0];

    const patch = parse(
        shell.setPatchJson(JSON.stringify({
            ops: [{ op: "transpose", a: 0, d: 12 }],
        })),
        "patch"
    );
    check("patch applies", patch.ok === true && patch.revision === 1);
    check(
        "patch changes the rendered score",
        patch.score.patterns[0].steps[0][0] === Math.min(127, originalNote + 12)
    );

    const secondRun = parse(shell.step(32), "second step");
    const patchedNotes = secondRun.ticks
        .flatMap((tick) => tick.events)
        .filter((event) => event.on)
        .map((event) => event.note);

    check("patched run produces events", patchedNotes.length > 0);
    check(
        "patched run differs from the raw run",
        patchedNotes.join(",") !== baselineNotes.join(",")
    );

    const exportedText = shell.exportScenarioJson();
    const exported = parse(exportedText, "exported scenario");
    check("scenario version", exported.version === "clockr.replay.v1");
    check("scenario has entries", exported.entries.length > 0);
    check("scenario carries its initial score",
        exported.initialScore && exported.initialScore.patterns.length === 16);

    const imported = parse(shell.importScenarioJson(exportedText), "import");
    check("import succeeds", imported.ok === true);
    check("import preserves entries", imported.entries === exported.entries.length);
    check("import returns a digest", typeof imported.digest === "string"
        && imported.digest.startsWith("0x"));

    const reexported = parse(shell.exportScenarioJson(), "re-export");
    check("import then export round-trips",
        reexported.version === "clockr.replay.v1"
        && reexported.entries.length === exported.entries.length);

    const branch = parse(
        shell.replayBranchJson(JSON.stringify({
            patch: { ops: [{ op: "transpose", a: 0, d: 7 }] },
        })),
        "branch"
    );
    check("branch replays", branch.ok === true);
    check("branch is not equal to the original", branch.equal === false);
    check("branch reports output mismatches", branch.outputMismatches > 0);
    check("branch has candidate ticks", branch.candidateTicks.length > 0);
    check("branch has original ticks", branch.originalTicks.length > 0);
    check("branch digest differs", branch.digest !== branch.originalDigest);

    const compared = parse(
        shell.compareWithOriginalJson(),
        "compare"
    );
    check("compare reports the difference", compared.ok === true
        && compared.equal === false && compared.outputMismatches > 0);

    const badImport = parse(shell.importScenarioJson("{bad"), "bad import");
    check("malformed scenario JSON is rejected", badImport.ok === false);

    const badPatch = parse(
        shell.setPatchJson(JSON.stringify({ ops: [{ op: "wat" }] })),
        "bad patch"
    );
    check("malformed patch is rejected", badPatch.ok === false);

    const badScore = parse(
        shell.setScoreJson(JSON.stringify({
            bpm: 120,
            patterns: [],
            tracks: [{ pattern: 99 }],
        })),
        "bad score"
    );
    check("out-of-range score is rejected", badScore.ok === false);

    const reset = parse(shell.reset(), "reset");
    check("reset returns a fresh journal", reset.ok === true
        && reset.journal.entries > 0 && reset.journal.patches === 0);

    const stepEdit = parse(shell.setStep(0, 0, 72, 104, 1), "step edit");
    check("setStep helper applies an edit", stepEdit.ok === true
        && stepEdit.score.patterns[0].steps[0][0] === 72
        && stepEdit.score.patterns[0].steps[0][1] === 104);

    const reload = parse(
        shell.setScoreJson(JSON.stringify(reset.score)),
        "score reload"
    );
    check("setScoreJson reloads a full score", reload.ok === true
        && reload.journal.patches === 0);

    shell.delete();
    process.exit(failed ? 1 : 0);
}).catch((error) => {
    console.error(`module load failed: ${error}`);
    process.exit(1);
});
