// Stirred Field renderer: a slow, faint particle field fills the canvas; the
// fling ball flies over it. The wasm recipe drives both layers' coupling —
// flinging fast raises `stir`, which quickens the field. The ball is drawn as
// an obstacle with circulation: nearby particles deflect around it and spin
// with the stir, so fast throws carve a swirling wake.
//
// Two host-side layers ride the network rather than re-deriving it:
// - the swim brain, which publishes a small spring-target offset while the
//   ball is free (autonomous darts that ride the flow, dodges when the
//   pointer trespasses);
// - the sound toggle: the recipe folds pitch/level for the scene hum; the
//   worklet is only ever a consumer of those values (plus one-shot events).

import { createRunner, onPointer, palette, rng } from "../lab-common.js";

const TAU = Math.PI * 2;
const COUNT = 880;
const SEED = 0x5f3759df;

// Obstacle/circulation around the ball (canvas units).
const OBSTACLE_R = 600; // inverse-square falloff scale
const CORE_R = 0.075; // hard core: particles are pushed out of here

const COLORS = [palette.blue, palette.teal, palette.violet];

function finiteOr(value, fallback)
{
    return Number.isFinite(value) ? value : fallback;
}

export function mount(canvas, lib)
{
    const sketch = new lib.StirredField();

    const x = new Float32Array(COUNT);
    const y = new Float32Array(COUNT);
    const wobble = new Float32Array(COUNT);
    const drift = new Float32Array(COUNT);
    const tint = new Uint8Array(COUNT);

    let needsClear = true;
    let lastWidth = 0;
    let lastHeight = 0;

    let down = false;
    let pointerX = 0.5;
    let pointerY = 0.5;
    let autoT = 0;
    let userUntil = 0;

    let lastVx = 0;
    let lastVy = 0;
    const ripples = [];

    // Swim brain: the autonomous little being. `dart` carries a target
    // offset; darts start every few seconds (biased along the field) and
    // also fire as dodges when the pointer strays near the ball while free.
    let swimX = 0;
    let swimY = 0;
    let dart = null;
    let nextDart = 2.5;
    const swamRandom = rng(0x9e3779b9);

    // The heart is honest about the tide: restlessness decides how soon and
    // how far the next dart fires. warm ver.
    function appetite(gap)
    {
        const r = Math.min(1, Math.max(0, finiteOr(sketch.restlessness(), 0.5)));

        return gap * (1.3 - 0.8 * r);
    }

    function startDart(at, dirX, dirY, mag)
    {
        const len = Math.hypot(dirX, dirY) || 1;

        dart = {
            from: at,
            dur: 0.30 + swamRandom() * 0.35,
            ox: (dirX / len) * mag,
            oy: (dirY / len) * mag,
        };
    }

    function swimBrain(dt, time, ballX, ballY, free, heading)
    {
        if (!free)
        {
            dart = null;
            swimX += (0 - swimX) * Math.min(1, dt * 22);
            swimY += (0 - swimY) * Math.min(1, dt * 22);
            sketch.setSwim(swimX, swimY);
            return;
        }

        if (dart)
        {
            const k = (time - dart.from) / dart.dur;

            if (k >= 1)
            {
                dart = null;
                nextDart = time + appetite(2.2 + swamRandom() * 4.3);
            }
        }

        if (!dart && time > nextDart)
        {
            // Mostly swim along the field, sometimes cut out of it. A
            // restless tide both shortens the wait (appetite above) and
            // lengthens the lunge.
            const boost = 1 + 0.55 * Math.min(1, Math.max(0, finiteOr(sketch.restlessness(), 0.5)));
            const sway = swamRandom() < 0.78;
            const a = heading + (sway ? 0 : 0.85 * (swamRandom() - 0.5));
            const along = (sway ? 0.06 + 0.10 * swamRandom() : 0.12 + 0.07 * swamRandom()) * boost;

            startDart(time, Math.cos(a + Math.PI / 6), Math.sin(a + Math.PI / 6), along);
            postEvent("dart", 520 + swamRandom() * 480, 0.11, 0.2, 0.35);
        }

        if (dart)
        {
            const k = Math.min(1, Math.max(0, (time - dart.from) / dart.dur));
            // A chase-elevator envelope: fast rise, gentle fall.
            const env = k < 0.3 ? k / 0.3 : 1 - (k - 0.3) / 0.7 * 0.75;

            swimX = dart.ox * env;
            swimY = dart.oy * env;
        }
        else
        {
            // Between darts the offset relaxes to zero, pool-free.
            swimX += (0 - swimX) * Math.min(1, dt * 6);
            swimY += (0 - swimY) * Math.min(1, dt * 6);
        }

        sketch.setSwim(swimX, swimY);
    }

    // Audio: at rest the scene hums at a whisper; stirring raises the pitch
    // and the flow. All synth slopes live in the worklet; the page only
    // forwards folded state and interaction events.
    let audio = null;
    const soundButton = document.createElement("button");

    soundButton.className = "lab-sound";
    soundButton.type = "button";
    soundButton.textContent = "sound off";

    function postEvent(kind, hz, g, dur, amp)
    {
        audio?.node?.port.postMessage({ t: "e", kind, hz, g, d: dur, a: amp });
    }

    soundButton.addEventListener("click", async () => {
        if (audio)
        {
            const armed = audio;

            armed.master.gain.setTargetAtTime(0, armed.ctx.currentTime, 0.04);
            setTimeout(() => armed.node.disconnect(), 120);
            await armed.ctx.close();

            audio = null;
            soundButton.textContent = "sound off";
            return;
        }

        soundButton.disabled = true;

        try
        {
            const ctx = new AudioContext();
            const master = new GainNode(ctx, { gain: 0 });

            await ctx.audioWorklet.addModule(new URL("./sonic-worklet.js", import.meta.url));

            const node = new AudioWorkletNode(ctx, "lab-scene", {
                numberOfInputs: 0,
                numberOfOutputs: 1,
                outputChannelCount: [2],
            });

            node.connect(master);
            master.connect(ctx.destination);
            master.gain.setTargetAtTime(0.85, ctx.currentTime + 0.05, 0.15);
            await ctx.resume();

            audio = { ctx, node, master };
            soundButton.textContent = "sound on";
        }
        catch
        {
            soundButton.textContent = "sound n/a";
            return;
        }
        finally
        {
            soundButton.disabled = false;
        }
    });

    canvas.parentElement.appendChild(soundButton);

    // Lab-only screenshot hook: `#demo=stir` scripts a grab, a fast swing and
    // a release, so a static capture shows the stirred wake. Inert otherwise.
    const demoStir = /demo=stir/.test(window.location.hash ?? "");

    function seed()
    {
        const random = rng(SEED);

        for (let i = 0; i < COUNT; i += 1)
        {
            x[i] = random();
            y[i] = random();
            wobble[i] = random() * TAU;
            drift[i] = 0.75 + random() * 0.5;

            const pick = random();
            tint[i] = pick < 0.5 ? 0 : pick < 0.8 ? 1 : 2;
        }
    }

    seed();

    const trailX = new Float32Array(64);
    const trailY = new Float32Array(64);
    let trailCount = 0;
    let trailNext = 0;

    function release()
    {
        if (!down)
            return;

        down = false;
        sketch.setPointer(pointerX, pointerY, false);

        const throwSpeed = Math.hypot(lastVx, lastVy);

        if (throwSpeed > 0.16)
            postEvent("throw", 170 + swamRandom() * 70, Math.min(0.5, 0.2 + throwSpeed * 0.45), 0.5, 0.35);
    }

    onPointer(canvas, (px, py, type) => {
        if (type === "pointerdown")
        {
            down = true;
            postEvent("grab", 96, 0.3, 0.24, 0.5);
        }

        if (type === "leave")
            down = false;

        if (type !== "leave")
        {
            pointerX = px;
            pointerY = py;
            userUntil = autoT + 5;
        }

        sketch.setPointer(pointerX, pointerY, down);
    });

    window.addEventListener("pointerup", release, { passive: true });

    createRunner(canvas, (ctx, dt, w, h) => {
        autoT += dt;

        // Idle: the target wanders, dragging the ball (and its stir) through
        // the quiet field.
        if (autoT > userUntil)
        {
            pointerX = 0.5 + 0.28 * Math.sin(autoT * 0.21);
            pointerY = 0.5 + 0.22 * Math.sin(autoT * 0.29 + 1.4);
            sketch.setPointer(pointerX, pointerY, false);
        }

        if (demoStir)
        {
            document.title = "demo-stir";

            // 0.9-2.6s: grabbed, dragged in a fast arc; release mid-arc so any
            // capture inside the window sees the ball displaced and stirred.
            if (autoT > 0.9 && autoT < 2.6)
            {
                const a = (autoT - 0.9) * 13;
                pointerX = 0.44 + 0.20 * Math.cos(a - 2.4);
                pointerY = 0.5 + 0.17 * Math.sin(a - 2.4);
                down = autoT < 2.3;
                sketch.setPointer(pointerX, pointerY, down);
                userUntil = autoT + 20;
            }
        }

        sketch.tick(dt);

        let bx = sketch.ballX();
        let by = sketch.ballY();
        let vx = sketch.velX();
        let vy = sketch.velY();
        const stir = finiteOr(sketch.stir(), 0);
        const angle = finiteOr(sketch.angle(), 0);
        const curl = Math.min(2.2, Math.max(0, finiteOr(sketch.curl(), 1)));
        const scale = Math.min(6, Math.max(0.25, finiteOr(sketch.scale(), 1.8)));
        const speed = Math.min(320, Math.max(0, finiteOr(sketch.speed(), 34)));
        const stirVis = Math.min(1.6, Math.max(0, stir));
        if (!Number.isFinite(bx))
            bx = 0.5;
        if (!Number.isFinite(by))
            by = 0.5;
        if (!Number.isFinite(vx))
            vx = 0;
        if (!Number.isFinite(vy))
            vy = 0;

        // Dodge: the being shies away from a pointer straying close while it
        // swims free; a grab overrides everything, as before.
        if (!down && dart === null)
        {
            const pdx = bx - pointerX;
            const pdy = by - pointerY;
            const pd = Math.hypot(pdx, pdy);

            if (pd < 0.15 && autoT < userUntil)
                startDart(autoT, pdx, pdy, 0.16 + 0.06 * (1 - pd / 0.15));
        }

        swimBrain(dt, autoT, bx, by, !down, angle);

        // Audio: the recipe's folded scene state, forwarded as-is; the
        // worklet slews. Events go out as one-shots. The deep kick's period
        // is the heartbeat of the scene: slow always, shorter as the tide's
        // restlessness rises or the ball stirs.
        if (audio)
        {
            const ap = sketch.audioPitch();

            if (Number.isFinite(ap))
            {
                const restlessness = Math.min(1, Math.max(0, finiteOr(sketch.restlessness(), 0.5)));

                audio.node.port.postMessage({
                    t: "p",
                    p: ap,
                    l: Math.max(0, Math.min(0.9, finiteOr(sketch.audioLevel(), 0.04))),
                    m: Math.min(1, speed / 240),
                    q: Math.min(16, Math.max(4.5, (12.5 - 4.8 * restlessness) / (1 + 0.45 * stirVis))),
                });
            }
        }

        // Field wash: dense, faint trails, tinted by the tide's warmth. The
        // tint slides between a cool night shade and a warm dusk shade, a
        // couple of paint layers deep — the weather you didn't change.
        const warmth = Math.min(1, Math.max(0, finiteOr(sketch.warmth(), 0.5)));
        ctx.save();
        ctx.globalAlpha = 1;
        ctx.globalCompositeOperation = "source-over";
        ctx.fillStyle = needsClear || w !== lastWidth || h !== lastHeight
            ? palette.bg
            : `rgba(${(11 + 3 * warmth).toFixed(0)}, ${(12 - 1 * warmth).toFixed(0)}, ${(16 - 7 * warmth).toFixed(0)}, 0.075)`;
        ctx.fillRect(0, 0, w, h);
        if (needsClear)
        {
            needsClear = false;
            lastWidth = w;
            lastHeight = h;
        }
        ctx.restore();

        // Bounce ripples: a fast axis flipping sign means a wall hit.
        if ((lastVx > 0.35 && vx < 0) || (lastVx < -0.35 && vx > 0) || (lastVy > 0.35 && vy < 0) || (lastVy < -0.35 && vy > 0))
        {
            if (ripples.length < 12)
                ripples.push({ x: bx, y: by, t: 0 });

            postEvent("hit", 88 + swamRandom() * 60, Math.min(0.4, 0.15 + Math.hypot(vx, vy) * 0.28), 0.36, 0.55);
        }
        lastVx = vx;
        lastVy = vy;

        // A floor keeps the whisper-level drift from stalling entirely; stir
        // and the ball's local field take over from there. Denser strokes:
        // a wider nib and slightly brighter paint, a slower step — thickness
        // buys the slowness back.
        const stepScale = Math.max(0.42, speed * dt * Math.max(0.65, w / 480));

        const ballPx = bx * w;
        const ballPy = by * h;

        ctx.lineCap = "round";
        ctx.lineWidth = Math.max(1.35, w / 480);

        // The faint field: particles flow around the ball as an obstacle with
        // circulation that scales with the stir.
        for (let i = 0; i < COUNT; i += 1)
        {
            const px = x[i];
            const py = y[i];
            const phase = wobble[i];

            const fieldX = px * scale * TAU + phase;
            const fieldY = py * scale * TAU * 0.82 - phase * 0.6;

            let heading = angle
                + Math.sin(fieldX) * curl
                + Math.cos(fieldY) * curl * 0.64;

            // Ball influence: inverse-square, radial repulsion plus
            // tangential circulation ∝ stir.
            const dx = px - bx;
            const dy = py - by;
            const r2 = dx * dx + dy * dy;
            const influence = 1 / (1 + r2 * OBSTACLE_R);
            const perp = Math.atan2(dy, dx) + Math.PI / 2;
            const swirl = 2.6 * influence * (0.35 + stirVis);
            const repel = 1.9 * influence;
            heading += Math.cos(perp) * swirl - dx * 3.0 * repel - dy * 3.0 * repel * 0.4;

            const step = stepScale * drift[i] * (1 + 2.4 * influence);
            let nx = px + Math.cos(heading) * step / w;
            let ny = py + Math.sin(heading) * step / h;

            // The hard core: slide the particle back onto the boundary.
            const bdx = nx - bx;
            const bdy = ny - by;
            const br = Math.hypot(bdx, bdy);
            if (br < CORE_R)
            {
                const rn = br < 1e-6 ? 1 : CORE_R / br;
                nx = bx + bdx * rn;
                ny = by + bdy * rn;
            }

            if (!Number.isFinite(nx) || !Number.isFinite(ny))
            {
                nx = 0.5;
                ny = 0.5;
                x[i] = nx;
                y[i] = ny;
                continue;
            }

            let wrapped = nx < 0 || nx >= 1 || ny < 0 || ny >= 1;
            nx -= Math.floor(nx);
            ny -= Math.floor(ny);

            x[i] = nx;
            y[i] = ny;

            if (wrapped)
                continue;

            ctx.globalAlpha = 0.115 + drift[i] * 0.07 + 0.10 * influence;
            ctx.strokeStyle = COLORS[tint[i]];
            ctx.beginPath();
            ctx.moveTo(px * w, py * h);
            ctx.lineTo(nx * w, ny * h);
            ctx.stroke();
        }

        // Bounce ripples (over the field, under the ball).
        ctx.save();
        ctx.globalCompositeOperation = "lighter";
        for (let i = ripples.length - 1; i >= 0; i -= 1)
        {
            const r = ripples[i];
            r.t += dt;

            if (r.t > 0.5)
            {
                ripples.splice(i, 1);
                continue;
            }

            const k = r.t / 0.5;
            ctx.globalAlpha = 0.40 * (1 - k);
            ctx.strokeStyle = palette.amber;
            ctx.lineWidth = 1.5;
            ctx.beginPath();
            ctx.arc(r.x * w, r.y * h, 3 + 15 * k, 0, TAU);
            ctx.stroke();
        }
        ctx.restore();

        // The ball: trail, then the disc, like Fling but a touch brighter so
        // it sits above the field.
        trailX[trailNext] = ballPx;
        trailY[trailNext] = ballPy;
        trailNext = (trailNext + 1) % trailX.length;
        if (trailCount < trailX.length)
            trailCount += 1;

        if (down)
        {
            ctx.save();
            ctx.globalAlpha = 0.5;
            ctx.strokeStyle = palette.amber;
            ctx.lineWidth = 1.4;
            ctx.beginPath();
            ctx.arc(pointerX * w, pointerY * h, 6, 0, TAU);
            ctx.stroke();

            ctx.setLineDash([3, 6]);
            ctx.globalAlpha = 0.3;
            ctx.beginPath();
            ctx.moveTo(pointerX * w, pointerY * h);
            ctx.lineTo(ballPx, ballPy);
            ctx.stroke();
            ctx.setLineDash([]);
            ctx.restore();
        }

        ctx.save();
        ctx.globalCompositeOperation = "lighter";
        const start = (trailNext - trailCount + trailX.length) % trailX.length;

        for (let i = 1; i < trailCount; i += 1)
        {
            const a0 = (start + i - 1 + trailX.length) % trailX.length;
            const a1 = (start + i + trailX.length) % trailX.length;
            const t = i / trailCount;

            ctx.globalAlpha = 0.02 + 0.12 * t * t;
            ctx.strokeStyle = palette.teal;
            ctx.lineWidth = 1 + 0.9 * t;
            ctx.beginPath();
            ctx.moveTo(trailX[a0], trailY[a0]);
            ctx.lineTo(trailX[a1], trailY[a1]);
            ctx.stroke();
        }

        ctx.globalAlpha = 0.08 + 0.06 * stirVis;
        ctx.fillStyle = palette.teal;
        ctx.beginPath();
        ctx.arc(ballPx, ballPy, 15, 0, TAU);
        ctx.fill();

        ctx.globalAlpha = 0.96;
        ctx.beginPath();
        ctx.arc(ballPx, ballPy, 7, 0, TAU);
        ctx.fill();

        ctx.globalAlpha = 0.85;
        ctx.fillStyle = "#e2f6ef";
        ctx.beginPath();
        ctx.arc(ballPx - 1.5, ballPy - 1.5, 2.4, 0, TAU);
        ctx.fill();
        ctx.restore();
    });

    // The spirit clock keeps its own time: a slow timer, not the rAF loop,
    // so the tide drifts even when the tab is hidden or the canvas idle.
    let tideLast = performance.now();
    const tideTimer = setInterval(() => {
        const now = performance.now();

        sketch.stepTide((now - tideLast) / 1000);
        tideLast = now;
    }, 2000);

    function stopTide()
    {
        clearInterval(tideTimer);
    }

    return {
        reset() {
            sketch.reset();
            seed();
            trailCount = 0;
            trailNext = 0;
            ripples.length = 0;
            lastVx = 0;
            lastVy = 0;
            down = false;
            autoT = 0;
            userUntil = 0;
            pointerX = 0.5;
            pointerY = 0.5;
            dart = null;
            swimX = 0;
            swimY = 0;
            needsClear = true;
        },
        stop: stopTide,
    };
}
