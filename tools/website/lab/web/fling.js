// Fling renderer: JS only draws — the throw, the walls and the reel-back all
// live in the wasm recipe. The faint amber ring is the spring target; release
// while the strong spring is stretched and the held velocity throws the ball.

import { createRunner, onPointer, palette } from "../lab-common.js";

const TAU = Math.PI * 2;
const WALL = 0.02; // mirrors the wasm wall margin
const FALLBACK = 0.5;

export function mount(canvas, lib)
{
    const sketch = new lib.Fling();

    let down = false;
    let pointerX = 0.5;
    let pointerY = 0.5;
    let autoT = 0;
    let userUntil = 0;

    const trailX = new Float32Array(96);
    const trailY = new Float32Array(96);
    let trailCount = 0;
    let trailNext = 0;

    let lastVx = 0;
    let lastVy = 0;
    const ripples = []; // { x, y, t }

    function release()
    {
        if (!down)
            return;

        down = false;
        sketch.setPointer(pointerX, pointerY, false);
    }

    onPointer(canvas, (x, y, type) => {
        if (type === "pointerdown")
            down = true;

        if (type === "leave")
            down = false;

        if (type !== "leave")
        {
            pointerX = x;
            pointerY = y;
            userUntil = autoT + 5;
        }

        sketch.setPointer(pointerX, pointerY, down);
    });

    window.addEventListener("pointerup", release, { passive: true });

    createRunner(canvas, (ctx, dt, w, h) => {
        autoT += dt;

        // Idle: the target wanders slowly, so the slack spring meanders.
        if (autoT > userUntil)
        {
            pointerX = 0.5 + 0.30 * Math.sin(autoT * 0.23);
            pointerY = 0.5 + 0.24 * Math.sin(autoT * 0.31 + 1.4);
            sketch.setPointer(pointerX, pointerY, false);
        }

        sketch.tick(dt);

        let bx = sketch.ballX();
        let by = sketch.ballY();
        let vx = sketch.velX();
        let vy = sketch.velY();
        const tx = sketch.targetX();
        const ty = sketch.targetY();

        if (!Number.isFinite(bx))
            bx = FALLBACK;
        if (!Number.isFinite(by))
            by = FALLBACK;
        if (!Number.isFinite(vx))
            vx = 0;
        if (!Number.isFinite(vy))
            vy = 0;

        // Bounce ripples: a fast axis flipping sign means a wall hit.
        if ((lastVx > 0.35 && vx < 0) || (lastVx < -0.35 && vx > 0) || (lastVy > 0.35 && vy < 0) || (lastVy < -0.35 && vy > 0))
        {
            if (ripples.length < 16)
                ripples.push({ x: bx, y: by, t: 0 });
        }
        lastVx = vx;
        lastVy = vy;

        ctx.fillStyle = palette.bg;
        ctx.fillRect(0, 0, w, h);

        const px = bx * w;
        const py = by * h;
        const tpx = (Number.isFinite(tx) ? tx : FALLBACK) * w;
        const tpy = (Number.isFinite(ty) ? ty : FALLBACK) * h;

        // The walls, so the bounces have something to push off of.
        ctx.save();
        ctx.strokeStyle = palette.dim;
        ctx.globalAlpha = 0.28;
        ctx.lineWidth = 1.5;
        ctx.strokeRect(WALL * w, WALL * h, (1 - 2 * WALL) * w, (1 - 2 * WALL) * h);
        ctx.restore();

        // Spring target: a faint ring; grabbed, a dashed tether to the ball.
        ctx.save();
        ctx.globalAlpha = down ? 0.65 : 0.3;
        ctx.strokeStyle = palette.amber;
        ctx.lineWidth = 1.4;
        ctx.beginPath();
        ctx.arc(tpx, tpy, down ? 6 : 4, 0, TAU);
        ctx.stroke();

        if (down)
        {
            ctx.setLineDash([3, 6]);
            ctx.globalAlpha = 0.35;
            ctx.beginPath();
            ctx.moveTo(tpx, tpy);
            ctx.lineTo(px, py);
            ctx.stroke();
            ctx.setLineDash([]);
        }
        ctx.restore();

        // Trail.
        trailX[trailNext] = px;
        trailY[trailNext] = py;
        trailNext = (trailNext + 1) % trailX.length;
        if (trailCount < trailX.length)
            trailCount += 1;

        ctx.save();
        ctx.globalCompositeOperation = "lighter";
        const start = (trailNext - trailCount + trailX.length) % trailX.length;

        for (let i = 1; i < trailCount; i += 1)
        {
            const a0 = (start + i - 1 + trailX.length) % trailX.length;
            const a1 = (start + i + trailX.length) % trailX.length;
            const t = i / trailCount;

            ctx.globalAlpha = 0.03 + 0.25 * t * t;
            ctx.strokeStyle = palette.teal;
            ctx.lineWidth = 1 + 1.6 * t;
            ctx.beginPath();
            ctx.moveTo(trailX[a0], trailY[a0]);
            ctx.lineTo(trailX[a1], trailY[a1]);
            ctx.stroke();
        }

        // Motion streak: short, proportional to velocity.
        const streak = Math.min(60, Math.hypot(vx * w, vy * h) * 0.06);
        if (streak > 2)
        {
            const inv = streak / Math.max(1e-4, Math.hypot(vx * w, vy * h));
            ctx.globalAlpha = 0.4;
            ctx.strokeStyle = palette.green;
            ctx.lineWidth = 2.4;
            ctx.beginPath();
            ctx.moveTo(px - vx * w * inv, py - vy * h * inv);
            ctx.lineTo(px, py);
            ctx.stroke();
        }

        // Bounce ripples.
        for (let i = ripples.length - 1; i >= 0; i -= 1)
        {
            const r = ripples[i];
            r.t += dt;

            if (r.t > 0.55)
            {
                ripples.splice(i, 1);
                continue;
            }

            const k = r.t / 0.55;
            ctx.globalAlpha = 0.5 * (1 - k);
            ctx.strokeStyle = palette.amber;
            ctx.lineWidth = 1.6;
            ctx.beginPath();
            ctx.arc(r.x * w, r.y * h, 3 + 16 * k, 0, TAU);
            ctx.stroke();
        }
        ctx.restore();

        // The ball.
        ctx.save();
        ctx.globalCompositeOperation = "lighter";
        ctx.globalAlpha = 0.16;
        ctx.fillStyle = palette.teal;
        ctx.beginPath();
        ctx.arc(px, py, 15, 0, TAU);
        ctx.fill();

        ctx.globalAlpha = 0.95;
        ctx.beginPath();
        ctx.arc(px, py, 7, 0, TAU);
        ctx.fill();

        ctx.globalAlpha = 0.85;
        ctx.fillStyle = "#e2f6ef";
        ctx.beginPath();
        ctx.arc(px - 1.5, py - 1.5, 2.4, 0, TAU);
        ctx.fill();
        ctx.restore();
    });

    return {
        reset() {
            sketch.reset();
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

            const ctx = canvas.getContext("2d");
            ctx.fillStyle = palette.bg;
            ctx.fillRect(0, 0, canvas.width, canvas.height);
        },
    };
}
