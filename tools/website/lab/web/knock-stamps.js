// Knock & Stamp renderer: the orbit is violet; each click knocks it (the halo
// flashes and the figure widens) and stamps an amber ghost of exactly where
// the orb was on that step. Ghost stamping means the JS keeps the last N
// stamps; the wasm-held one is the newest.

import { createRunner, onPointer, palette } from "../lab-common.js";

const TAU = Math.PI * 2;
const STAMP_MAX = 12;
const STAMP_LIFE = 5.0;

export function mount(canvas, lib)
{
    const sketch = new lib.KnockStamps();

    const stamps = []; // { x, y, born, age }
    let lastCount = -1;

    onPointer(canvas, (_x, _y, type) => {
        if (type === "pointerdown")
            sketch.pulse();
    });

    createRunner(canvas, (ctx, dt, w, h) => {
        sketch.tick(dt);

        let ox = sketch.orbX();
        let oy = sketch.orbY();
        let knock = sketch.knock();
        const snapX = sketch.snapX();
        const snapY = sketch.snapY();
        const count = sketch.stampCount();

        if (!Number.isFinite(ox))
            ox = 0.5;
        if (!Number.isFinite(oy))
            oy = 0.5;
        if (!Number.isFinite(knock))
            knock = 0;

        if (Number.isFinite(count) && lastCount >= 0 && count > lastCount && count > 0 && Number.isFinite(snapX) && Number.isFinite(snapY))
        {
            stamps.push({ x: snapX, y: snapY, age: 0 });

            while (stamps.length > STAMP_MAX)
                stamps.shift();
        }

        if (Number.isFinite(count))
            lastCount = count;

        ctx.fillStyle = palette.bg;
        ctx.fillRect(0, 0, w, h);

        const px = ox * w;
        const py = oy * h;

        // Stamps: amber ghosts that fade out, with a dotted tether to the last.
        ctx.save();
        for (let i = stamps.length - 1; i >= 0; i -= 1)
        {
            const s = stamps[i];
            s.age += dt;
            const a = Math.max(0, 1 - s.age / STAMP_LIFE);

            if (a <= 0)
            {
                stamps.splice(i, 1);
                continue;
            }

            const sx = s.x * w;
            const sy = s.y * h;

            ctx.globalAlpha = 0.10 + 0.35 * a * a;
            ctx.strokeStyle = palette.amber;
            ctx.lineWidth = 1.3;
            ctx.beginPath();
            ctx.arc(sx, sy, 5 + 3 * a, 0, TAU);
            ctx.stroke();

            ctx.globalAlpha = 0.2 + 0.65 * a * a;
            ctx.fillStyle = palette.amber;
            ctx.beginPath();
            ctx.arc(sx, sy, 2.2, 0, TAU);
            ctx.fill();
        }

        if (stamps.length > 0 && Number.isFinite(snapX) && Number.isFinite(snapY))
        {
            const last = stamps[stamps.length - 1];
            ctx.globalAlpha = 0.22;
            ctx.strokeStyle = palette.amber;
            ctx.setLineDash([2, 7]);
            ctx.lineWidth = 1;
            ctx.beginPath();
            ctx.moveTo(last.x * w, last.y * h);
            ctx.lineTo(px, py);
            ctx.stroke();
            ctx.setLineDash([]);
            ctx.globalAlpha = 1;
        }

        // Counter.
        if (Number.isFinite(count) && count > 0)
        {
            ctx.globalAlpha = 0.55;
            ctx.fillStyle = palette.dim;
            ctx.font = "11px ui-monospace, monospace";
            ctx.fillText(`stamped ${Math.round(count)}`, 10, h - 10);
        }
        ctx.restore();

        // The orbit itself: guide ellipse, knock halo, gem.
        ctx.save();
        ctx.strokeStyle = palette.violet;
        ctx.lineWidth = 2;
        ctx.globalAlpha = 0.08 + 0.05 * knock;
        ctx.beginPath();
        ctx.ellipse(w / 2, h / 2, (0.30 + 0.08 * knock) * w, (0.24 + 0.06 * knock) * h, 0, 0, TAU);
        ctx.stroke();

        ctx.globalCompositeOperation = "lighter";
        ctx.globalAlpha = 0.10 + 0.4 * knock;
        ctx.fillStyle = palette.violet;
        ctx.beginPath();
        ctx.arc(px, py, 14 + 10 * knock, 0, TAU);
        ctx.fill();

        ctx.globalAlpha = 0.95;
        ctx.beginPath();
        ctx.arc(px, py, 6.5, 0, TAU);
        ctx.fill();

        ctx.globalAlpha = 0.85;
        ctx.fillStyle = "#efe6ff";
        ctx.beginPath();
        ctx.arc(px - 1.4, py - 1.4, 2.3, 0, TAU);
        ctx.fill();
        ctx.restore();
    });

    return {
        reset() {
            sketch.reset();
            stamps.length = 0;
            lastCount = -1;
        },
    };
}
