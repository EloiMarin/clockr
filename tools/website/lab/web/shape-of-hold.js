// The Shape of a Hold renderer. The spring lives in wasm; the canvas keeps a
// short trail of held positions so the lag behind the raw pointer reads
// clearly.

import { createRunner, onPointer, palette } from "../lab-common.js";

const TRAIL_MAX = 110;
const TAU = Math.PI * 2;

function makeRing(size)
{
    return { xs: new Float32Array(size), ys: new Float32Array(size), size, count: 0, next: 0 };
}

function clearRing(ring)
{
    ring.count = 0;
    ring.next = 0;
}

function push(ring, x, y)
{
    ring.xs[ring.next] = x;
    ring.ys[ring.next] = y;
    ring.next = (ring.next + 1) % ring.size;

    if (ring.count < ring.size)
        ring.count += 1;
}

function drawTrail(ctx, ring, w, h, pad)
{
    if (ring.count < 2)
        return;

    const chunks = 7;
    const start = (ring.next - ring.count + ring.size) % ring.size;
    const spanX = Math.max(1, w - 2 * pad);
    const spanY = Math.max(1, h - 2 * pad);

    ctx.lineWidth = 1.8;
    ctx.lineCap = "round";
    ctx.lineJoin = "round";
    ctx.strokeStyle = palette.green;

    for (let c = 0; c < chunks; c += 1)
    {
        const a = Math.floor(ring.count * c / chunks);
        const b = Math.floor(ring.count * (c + 1) / chunks);

        if (b - a < 2)
            continue;

        const from = a > 0 ? a - 1 : 0;
        ctx.beginPath();

        for (let k = from; k < b; k += 1)
        {
            const idx = (start + k) % ring.size;
            const px = pad + ring.xs[idx] * spanX;
            const py = pad + ring.ys[idx] * spanY;

            if (k === from)
                ctx.moveTo(px, py);
            else
                ctx.lineTo(px, py);
        }

        const t = (c + 1) / chunks;
        ctx.globalAlpha = 0.06 + 0.5 * t * t;
        ctx.stroke();
    }

    ctx.globalAlpha = 1;
}

export function mount(canvas, lib)
{
    const sketch = new lib.ShapeOfHold();

    let autoT = 0;
    let userUntil = 0;

    onPointer(canvas, (x, y, type) => {
        if (type !== "leave")
            userUntil = autoT + 5;

        sketch.setPointer(x, y);
    });

    const trail = makeRing(TRAIL_MAX);

    createRunner(canvas, (ctx, dt, w, h) => {
        autoT += dt;

        // A gentle orbit keeps the sketch alive before the first pointer
        // gesture; a real gesture takes over for five seconds.
        if (autoT > userUntil)
        {
            sketch.setPointer(
                0.5 + 0.30 * Math.cos(autoT * 0.8) + 0.05 * Math.cos(autoT * 2.3),
                0.5 + 0.26 * Math.sin(autoT * 1.1) + 0.04 * Math.sin(autoT * 2.9),
            );
        }

        sketch.tick(dt);

        const rawX = sketch.rawX();
        const rawY = sketch.rawY();
        const heldX = sketch.heldX();
        const heldY = sketch.heldY();

        const rx = Number.isFinite(rawX) ? rawX : 0.5;
        const ry = Number.isFinite(rawY) ? rawY : 0.5;
        const hx = Number.isFinite(heldX) ? heldX : rx;
        const hy = Number.isFinite(heldY) ? heldY : ry;

        const pad = Math.min(w, h) * 0.16;
        const spanX = Math.max(1, w - 2 * pad);
        const spanY = Math.max(1, h - 2 * pad);

        const rawPx = pad + rx * spanX;
        const rawPy = pad + ry * spanY;
        const heldPx = pad + hx * spanX;
        const heldPy = pad + hy * spanY;

        ctx.save();
        ctx.globalCompositeOperation = "source-over";
        ctx.globalAlpha = 1;
        ctx.fillStyle = palette.bg;
        ctx.fillRect(0, 0, w, h);
        ctx.restore();

        push(trail, hx, hy);

        ctx.save();
        ctx.globalCompositeOperation = "lighter";
        drawTrail(ctx, trail, w, h, pad);
        ctx.restore();

        // The raw pointer: a faint, slightly larger ghost. A thin dashed
        // tether shows how far the held point is being pulled.
        ctx.save();
        ctx.globalAlpha = 0.16;
        ctx.fillStyle = palette.amber;
        ctx.beginPath();
        ctx.arc(rawPx, rawPy, 7, 0, TAU);
        ctx.fill();

        ctx.globalAlpha = 0.55;
        ctx.strokeStyle = palette.amber;
        ctx.lineWidth = 1.3;
        ctx.beginPath();
        ctx.arc(rawPx, rawPy, 7, 0, TAU);
        ctx.stroke();

        ctx.globalAlpha = 0.20;
        ctx.setLineDash([3, 7]);
        ctx.beginPath();
        ctx.moveTo(rawPx, rawPy);
        ctx.lineTo(heldPx, heldPy);
        ctx.stroke();
        ctx.setLineDash([]);
        ctx.restore();

        // The held point: calm, solid, with a soft halo and a small highlight.
        ctx.save();
        ctx.globalCompositeOperation = "lighter";
        ctx.globalAlpha = 0.14;
        ctx.fillStyle = palette.green;
        ctx.beginPath();
        ctx.arc(heldPx, heldPy, 17, 0, TAU);
        ctx.fill();

        ctx.globalAlpha = 0.95;
        ctx.beginPath();
        ctx.arc(heldPx, heldPy, 8, 0, TAU);
        ctx.fill();

        ctx.globalAlpha = 0.85;
        ctx.fillStyle = "#eaf7e2";
        ctx.beginPath();
        ctx.arc(heldPx - 1.5, heldPy - 1.5, 2.6, 0, TAU);
        ctx.fill();
        ctx.restore();

        ctx.globalAlpha = 1;
    });

    return {
        reset() {
            sketch.reset();
            clearRing(trail);
            autoT = 0;
            userUntil = 0;
        },
    };
}
