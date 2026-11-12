/*global Worker, document, window, performance, Blob, URL, AudioContext */

(function () {
    "use strict";

    const $ = (id) => document.getElementById(id);
    const clamp = (value, lo, hi) => Math.min(hi, Math.max(lo, value));
    const fmt = (value, digits = 2) =>
        typeof value === "number" && Number.isFinite(value)
            ? value.toFixed(digits)
            : "—";

    const worker = new Worker("worker.js");
    let nextCall = 0;
    const pending = new Map();

    function call(method, ...args) {
        return new Promise((resolve, reject) => {
            const id = ++nextCall;
            pending.set(id, { resolve, reject });
            worker.postMessage({ type: "call", id, method, args });
        });
    }

    worker.onmessage = (event) => {
        const message = event.data;

        if (message.type === "ready") {
            boot().catch((error) => badge(`shell: ${error.message}`, "err"));
            return;
        }

        if (message.type === "failed") {
            badge("shell: failed", "err");
            showResult(message.error, "err");
            return;
        }

        const waiter = pending.get(message.id);
        if (!waiter)
            return;

        pending.delete(message.id);

        if (message.ok)
            waiter.resolve(message.result);
        else
            waiter.reject(new Error(message.error || "shell call failed"));
    };

    worker.onerror = (event) => {
        badge(`shell: ${event.message}`, "err");
    };

    function badge(text, cls) {
        const element = $("engine-badge");
        element.textContent = text;
        element.className = "badge" + (cls ? " " + cls : "");
    }

    function showResult(text, cls) {
        const element = $("result");
        element.textContent = text;
        element.className = "result" + (cls ? " " + cls : "");
    }

    const live = {
        rawTicks: [],
        state: null,
        branch: null,
        history: null,
        selected: null,
        playing: false,
        blockMs: (1024 / 48000) * 1000,
        playOriginFrame: null,
        playOriginTime: 0,
        liveNodes: [],
        scrubbing: false,
    };

    async function boot() {
        const score = JSON.parse(await call("defaultScoreJson"));
        const init = JSON.parse(await call("init", 120));
        applyState(init);
        live.playing = false;
        syncTransport(init);
        await pushIntents();
        renderScore(score);
        badge("shell: ready", "on");
        schedulePump();
        showResult(
            "Recording from the first ui tick. Play, edit steps, export, import, " +
            "fork a patch, then audition raw and candidate."
        );
    }

    function applyState(state) {
        if (!state || state.ok === false)
            return;

        live.state = state;
        live.blockMs = (state.block.frames / state.block.rate) * 1000;
        $("revision-value").textContent = String(state.journal.patches);
        $("entries-value").textContent = String(state.journal.entries);
        $("branch-value").textContent = state.branchRun ? "ready" : "—";
        $("voice-value").textContent = String(state.voices.length);
        $("playhead-value").textContent = fmt(state.playhead, 3);
        const beats = Number.isFinite(state.playhead) ? state.playhead : 0;
        const uiBeats = Number.isFinite(state.playheadUi) ? state.playheadUi : beats;
        const bar = Math.floor(beats / 4) + 1;
        const beat = (beats % 4) + 1;
        $("pos-value").textContent = `${bar}.${beat.toFixed(2)}`;
        updateCursors(beats, uiBeats);
        renderHistory(state);
    }

    function syncTransport(state) {
        if (!state || !state.intents)
            return;

        $("bpm-input").value = String(Math.round(state.intents.bpm));
        $("master-slider").value = String(state.intents.master);
        $("master-value").textContent = fmt(state.intents.master, 2);
        $("loop-start").value = String(state.intents.loop.start);
        $("loop-end").value = String(state.intents.loop.end);
        $("loop-on").checked = state.intents.loop.enabled;
        setPlayLabel(state.intents.playing);
    }

    function setPlayLabel(playing) {
        live.playing = playing;
        $("play-btn").textContent = playing ? "Stop" : "Play";
    }

    function currentIntents() {
        const bpm = Number($("bpm-input").value);
        const loopStart = Number($("loop-start").value);
        const loopEnd = Number($("loop-end").value);
        const master = Number($("master-slider").value);

        return {
            playing: live.playing,
            bpm: Number.isFinite(bpm) ? clamp(bpm, 20, 240) : 120,
            loopStart: Number.isFinite(loopStart) ? loopStart : 0,
            loopEnd: Number.isFinite(loopEnd) ? loopEnd : 8,
            loopOn: $("loop-on").checked,
            master: Number.isFinite(master) ? clamp(master, 0, 1) : 0.8,
        };
    }

    async function pushIntents() {
        const intents = currentIntents();
        const response = JSON.parse(await call(
            "setIntents",
            intents.playing ? 1 : 0,
            intents.bpm,
            intents.loopStart,
            intents.loopEnd,
            intents.loopOn ? 1 : 0,
            intents.master
        ));

        applyState(response);
        return response;
    }

    function renderScore(score) {
        const grid = $("grid");
        grid.replaceChildren();

        score.tracks.forEach((track, trackIndex) => {
            const row = document.createElement("div");
            row.className = "track-row";

            const label = document.createElement("div");
            label.className = "track-label";
            label.textContent = `T${trackIndex + 1} · p${track.pattern} · g${track.gain}${track.mute ? " · mute" : ""}`;
            row.append(label);

            const pattern = score.patterns[track.pattern];

            for (let step = 0; step < 16; step += 1) {
                const cell = pattern.steps[step] || { note: 60, velocity: 0, gate: false };
                const button = document.createElement("button");
                button.className = "step" + (cell.gate ? " on" : "");
                button.textContent = cell.gate ? String(cell.note) : "·";
                button.title = `track ${trackIndex + 1} step ${step} (shift-click to select)`;
                button.addEventListener("click", (event) => {
                    if (event.shiftKey)
                        selectStep(trackIndex, step, pattern, cell);
                    else
                        toggleStep(trackIndex, step, pattern, cell).catch(report);
                });
                row.append(button);
            }

            grid.append(row);
        });

        if (live.selected)
            highlightSelection();
    }

    function selectStep(trackIndex, step, pattern, cell) {
        live.selected = { trackIndex, step, pattern: pattern };
        $("selection-label").textContent =
            `track ${trackIndex + 1} · pattern ${live.state ? live.state.score.tracks[trackIndex].pattern : "?"} · step ${step}`;
        $("note-input").value = String(cell.note);
        $("velocity-input").value = String(cell.velocity);
        $("gate-input").checked = Boolean(cell.gate);
        highlightSelection();
    }

    async function toggleStep(trackIndex, step, pattern, cell) {
        if (live.history) {
            showResult(
                "Viewing the past is read-only. Return to live to edit, or " +
                "fork at the selected block.", "err"
            );
            return;
        }

        selectStep(trackIndex, step, pattern, cell);

        const engage = !cell.gate;
        $("gate-input").checked = engage;

        if (engage && !(Number($("velocity-input").value) > 0))
            $("velocity-input").value = "96";

        await applyStep(engage);
    }

    function highlightSelection() {
        const rows = $("grid").querySelectorAll(".track-row");

        rows.forEach((row, trackIndex) => {
            row.querySelectorAll(".step").forEach((step, stepIndex) => {
                const selected = live.selected
                    && live.selected.trackIndex === trackIndex
                    && live.selected.step === stepIndex;
                step.classList.toggle("selected", Boolean(selected));
            });
        });
    }

    async function applyStep(gate) {
        if (live.history) {
            showResult(
                "Viewing the past is read-only. Return to live to edit, or " +
                "fork at the selected block.", "err"
            );
            return;
        }

        if (!live.selected) {
            showResult("Select a step first.", "err");
            return;
        }

        const track = live.state.score.tracks[live.selected.trackIndex];
        const note = Number($("note-input").value);
        const velocity = Number($("velocity-input").value);
        const response = JSON.parse(await call(
            "setStep",
            track.pattern,
            live.selected.step,
            Number.isFinite(note) ? clamp(note, 0, 127) : 60,
            Number.isFinite(velocity) ? clamp(velocity, 0, 127) : 96,
            gate ? 1 : 0
        ));

        if (response.ok === false) {
            showResult(response.error, "err");
            return;
        }

        await advance(2);
        await refreshState();
        showResult(
            `Applied step ${live.selected.step} to pattern ${track.pattern}; ` +
            `edit revision ${response.revision}.`
        );
    }

    async function refreshState() {
        const state = JSON.parse(await call("getStateJson"));
        applyState(state);

        if (!live.history)
            renderScore(state.score);

        return state;
    }

    async function advance(count) {
        const result = JSON.parse(await call("step", count));
        collectRaw(result.ticks);
        applyState({ ...live.state, ...result, journal: live.state.journal });
        scheduleLive(result.ticks);
        return result;
    }

    function renderHistory(state) {
        const history = state && state.history ? state.history : { blocks: 0, patches: [] };
        const range = $("history-range");
        const marks = $("history-marks");

        if (!range || !marks)
            return;

        const lastBlock = Math.max(0, history.blocks - 1);
        const selectedBlock = live.history
            ? Math.min(live.history.block, lastBlock)
            : lastBlock;

        range.max = String(lastBlock);

        if (!live.scrubbing)
            range.value = String(selectedBlock);

        marks.replaceChildren();

        const total = Math.max(1, history.blocks);

        for (const patch of history.patches || []) {
            const mark = document.createElement("button");
            mark.className = "history-mark";
            mark.style.left = `${(patch.block / total) * 100}%`;
            mark.title =
                `revision ${patch.revision} at block ${patch.block} ` +
                `(entry ${patch.entry})`;
            mark.addEventListener("click", () => {
                selectHistoryBlock(patch.block).catch(report);
            });
            marks.append(mark);
        }

        $("history-label").textContent = live.history
            ? `viewing block ${live.history.block} · cut ${live.history.cut}`
            : `live · ${history.blocks} block${history.blocks === 1 ? "" : "s"}`;
    }

    async function selectHistoryBlock(block) {
        if (!live.state)
            return;

        live.scrubbing = false;

        if (live.playing) {
            setPlayLabel(false);
            stopLiveAudio();
            await pushIntents();
        }

        const response = JSON.parse(await call("replayBlockJson", block));

        if (response.ok === false) {
            showResult(response.error, "err");
            return;
        }

        live.history = response;
        renderHistory(live.state);
        renderScore(response.score);
        $("history-range").value = String(response.block);
        showResult(
            `Replaying through block ${response.block} ` +
            `(${response.patches} revision${response.patches === 1 ? "" : "s"}). ` +
            `"Audition selected" listens to this prefix; "Fork" cuts here.`,
            "ok"
        );
    }

    function returnToLive() {
        live.history = null;
        renderHistory(live.state);

        if (live.state)
            renderScore(live.state.score);

        showResult("Back at the live revision.");
    }

    async function auditionSelected() {
        const ticks = live.history ? live.history.ticks : live.rawTicks;
        const label = live.history ? `block ${live.history.block}` : "raw";
        await audition(ticks, label);
    }

    function collectRaw(ticks) {
        if (!Array.isArray(ticks))
            return;

        live.rawTicks.push(...ticks);

        if (live.rawTicks.length > 4096)
            live.rawTicks.splice(0, live.rawTicks.length - 4096);
    }

    let pumpBusy = false;
    let pumpAccum = 0;
    let pumpLast = performance.now();
    let refreshCountdown = 0;

    async function pump() {
        const now = performance.now();
        const elapsed = now - pumpLast;
        pumpLast = now;

        if (!live.playing) {
            pumpAccum = 0;
        } else {
            pumpAccum += elapsed / live.blockMs;
        }

        if (!pumpBusy && live.playing && pumpAccum >= 1) {
            const blocks = Math.min(8, Math.floor(pumpAccum));
            pumpAccum -= blocks;
            pumpBusy = true;

            try {
                await advance(blocks);
                refreshCountdown -= blocks;

                if (refreshCountdown <= 0) {
                    refreshCountdown = 16;
                    await refreshState();
                }
            } catch (error) {
                badge(`shell: ${error.message}`, "err");
            } finally {
                pumpBusy = false;
            }
        }

        window.setTimeout(pump, 30);
    }

    function schedulePump() {
        pumpLast = performance.now();
        pump();
    }

    function updateCursors(beats, uiBeats) {
        const intents = live.state ? live.state.intents : null;
        const start = intents ? intents.loop.start : 0;
        const end = intents ? intents.loop.end : 8;
        const span = Math.max(0.25, end - start);
        const fraction = clamp((beats - start) / span, 0, 1);
        const uiFraction = clamp((uiBeats - start) / span, 0, 1);
        const band = $("loop-band");
        band.style.width = `${(intents && intents.loop.enabled ? 100 : 0)}%`;
        $("cursor-raw").style.left = `${fraction * 100}%`;
        $("cursor-ui").style.left = `${uiFraction * 100}%`;
    }

    $("play-btn").addEventListener("click", async () => {
        const playing = !live.playing;
        setPlayLabel(playing);

        if (playing) {
            await ensureAudio();
            live.playOriginFrame = null;
            live.playOriginTime = 0;
        } else {
            stopLiveAudio();
        }

        await pushIntents();
    });

    $("bpm-input").addEventListener("change", () => { pushIntents().catch(report); });
    $("master-slider").addEventListener("input", () => {
        $("master-value").textContent = fmt(Number($("master-slider").value), 2);
    });
    $("master-slider").addEventListener("change", () => { pushIntents().catch(report); });
    $("loop-on").addEventListener("change", () => { pushIntents().catch(report); });
    $("loop-start").addEventListener("change", () => { pushIntents().catch(report); });
    $("loop-end").addEventListener("change", () => { pushIntents().catch(report); });

    $("note-input").addEventListener("change", () => {
        applyStep($("gate-input").checked).catch(report);
    });
    $("velocity-input").addEventListener("change", () => {
        applyStep($("gate-input").checked).catch(report);
    });
    $("gate-input").addEventListener("change", () => {
        applyStep($("gate-input").checked).catch(report);
    });

    $("take-btn").addEventListener("click", async () => {
        try {
            const state = JSON.parse(await call(
                "setScoreJson",
                JSON.stringify(live.state.score)
            ));
            live.rawTicks = [];
            live.branch = null;
            live.history = null;
            live.selected = null;
            stopLiveAudio();
            stopAudition();
            applyState(state);
            renderScore(state.score);
            await pushIntents();
            showResult("Record: the journal restarted at the current score.");
        } catch (error) {
            report(error);
        }
    });

    $("reset-btn").addEventListener("click", async () => {
        try {
            const state = JSON.parse(await call("reset"));
            live.rawTicks = [];
            live.branch = null;
            live.history = null;
            live.selected = null;
            setPlayLabel(false);
            stopLiveAudio();
            stopAudition();
            applyState(state);
            syncTransport(state);
            renderScore(state.score);
            await pushIntents();
            showResult("Reset to the demo score.");
        } catch (error) {
            report(error);
        }
    });

    $("export-btn").addEventListener("click", async () => {
        try {
            const scenario = await call("exportScenarioJson");
            $("scenario-text").value = scenario;
            showResult(
                `Exported ${JSON.parse(scenario).entries.length} journal entries.`,
                "ok"
            );
        } catch (error) {
            report(error);
        }
    });

    $("import-btn").addEventListener("click", async () => {
        try {
            const response = JSON.parse(await call(
                "importScenarioJson",
                $("scenario-text").value
            ));

            if (response.ok === false) {
                showResult(response.error, "err");
                return;
            }

            live.branch = null;
            live.history = null;
            live.selected = null;
            stopLiveAudio();
            stopAudition();
            showResult(
                `Imported ${response.entries} entries · ${response.digest} · ` +
                `fidelity ${response.fidelity.complete ? "complete" : "incomplete"}.`,
                "ok"
            );
            await refreshState();
        } catch (error) {
            report(error);
        }
    });

    $("fork-btn").addEventListener("click", async () => {
        try {
            const edit = JSON.parse($("patch-text").value);

            if (live.history)
                edit.cut = live.history.cut;

            const response = JSON.parse(await call(
                "replayBranchJson",
                JSON.stringify(edit)
            ));

            if (response.ok === false) {
                showResult(response.error, "err");
                return;
            }

            live.branch = response;
            $("branch-value").textContent = `b${response.branch}`;
            showResult(
                `Branch ${response.branch} at entry ${response.cut}\n` +
                `${response.summary}\n` +
                `first divergence: step ${response.firstStep}\n` +
                `digest raw ${response.originalDigest}\n` +
                `digest candidate ${response.digest}\n` +
                `mismatches ${response.outputMismatches} output / ${response.traceMismatches} trace` +
                (response.truncated ? "\n(audition ticks truncated)" : ""),
                "ok"
            );
        } catch (error) {
            report(error);
        }
    });

    $("compare-btn").addEventListener("click", async () => {
        try {
            const response = JSON.parse(await call("compareWithOriginalJson"));

            if (response.ok === false) {
                showResult(response.error, "err");
                return;
            }

            showResult(
                `${response.summary}\n` +
                `first divergence: step ${response.firstStep}\n` +
                `raw ${response.originalDigest}\ncandidate ${response.candidateDigest}`,
                response.equal ? "ok" : ""
            );
        } catch (error) {
            report(error);
        }
    });

    $("audition-raw").addEventListener("click", () => {
        const ticks = live.branch ? live.branch.originalTicks : live.rawTicks;
        audition(ticks, "raw").catch(report);
    });

    $("audition-candidate").addEventListener("click", () => {
        if (!live.branch) {
            showResult("Fork a branch first so there is a candidate to audition.", "err");
            return;
        }

        audition(live.branch.candidateTicks, "candidate").catch(report);
    });

    $("audition-selected").addEventListener("click", () => {
        auditionSelected().catch(report);
    });

    $("history-range").addEventListener("input", () => {
        const block = Number($("history-range").value);
        live.scrubbing = true;
        $("history-label").textContent = `scrub to block ${block}`;
    });

    $("history-range").addEventListener("change", () => {
        selectHistoryBlock(Number($("history-range").value)).catch(report);
    });

    $("history-live").addEventListener("click", returnToLive);

    $("stop-audio").addEventListener("click", () => {
        stopAudition();
        stopLiveAudio();
    });

    let audio = null;
    let audioNodes = [];

    async function ensureAudio() {
        if (!audio)
            audio = new AudioContext();

        if (audio.state === "suspended")
            await audio.resume();

        return audio;
    }

    function scheduleNote(event, at, nodes) {
        const when = Math.max(at, audio.currentTime + 0.005);
        const osc = audio.createOscillator();
        const gain = audio.createGain();
        const frequency = 440 * Math.pow(2, (event.note - 69) / 12);
        const level = Math.max(0.012, (event.velocity / 127) * 0.12);

        osc.type = ["triangle", "sine", "square", "sawtooth"][(event.track || 0) % 4];
        osc.frequency.setValueAtTime(frequency, when);
        gain.gain.setValueAtTime(0.0001, when);
        gain.gain.exponentialRampToValueAtTime(level, when + 0.008);
        gain.gain.exponentialRampToValueAtTime(0.0001, when + 0.20);
        osc.connect(gain).connect(audio.destination);
        osc.start(when);
        osc.stop(when + 0.22);
        nodes.push(osc);

        window.setTimeout(() => {
            const index = nodes.indexOf(osc);
            if (index >= 0)
                nodes.splice(index, 1);
        }, 320);
    }

    function scheduleTicks(ticks, baseTime, baseFrame, nodes, maxSeconds, maxNotes) {
        let scheduled = 0;
        const horizon = baseTime + maxSeconds;

        for (const tick of ticks) {
            const rate = tick.rate || 48000;
            const base = baseTime + (tick.start - baseFrame) / rate;

            if (base > horizon)
                break;

            const events = Array.isArray(tick.events) ? tick.events : [];

            for (const event of events) {
                if (!event.on || scheduled >= maxNotes)
                    continue;

                const at = base + (event.offset || 0) / rate;

                if (at > horizon)
                    break;

                scheduleNote(event, at, nodes);
                scheduled += 1;
            }
        }

        return scheduled;
    }

    function stopNodes(nodes) {
        for (const node of nodes) {
            try {
                node.stop();
            } catch {
                /* already stopped */
            }
        }

        nodes.length = 0;
    }

    function stopAudition() {
        stopNodes(audioNodes);
    }

    function stopLiveAudio() {
        stopNodes(live.liveNodes);
        live.playOriginFrame = null;
        live.playOriginTime = 0;
    }

    function scheduleLive(ticks) {
        if (!live.playing || !audio || !Array.isArray(ticks) || ticks.length === 0)
            return;

        const first = ticks[0];

        if (live.playOriginFrame === null) {
            live.playOriginFrame = first.start;
            live.playOriginTime = audio.currentTime + 0.08;
        }

        scheduleTicks(
            ticks,
            live.playOriginTime,
            live.playOriginFrame,
            live.liveNodes,
            10,
            256
        );
    }

    async function audition(ticks, label) {
        if (!Array.isArray(ticks) || ticks.length === 0) {
            showResult(`No ${label} ticks to audition yet.`, "err");
            return;
        }

        stopAudition();
        await ensureAudio();

        const first = ticks[0];
        const scheduled = scheduleTicks(
            ticks,
            audio.currentTime + 0.08,
            first.start,
            audioNodes,
            12,
            512
        );

        showResult(`Auditioning ${scheduled} ${label} note events.`, "ok");
    }

    function report(error) {
        badge("shell: error", "err");
        showResult(String(error && error.message ? error.message : error), "err");
    }
})();
