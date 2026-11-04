// Signal Bloom renderer. The recipe owns all motion; this file only collects
// the head / feedback / ghost points into ring buffers and draws them.

import { createRunner, onPointer, palette } from "../lab-common.js";

const HEAD_MAX = 480;
const FEED_MAX = 110;
const GHOST_MAX = 20;
const TAU = Math.PI * 2;

function hexToRgb(hex)
{
    const n = parseInt(hex.slice(1), 16);
    return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}

function ramp(from, to, steps)
{
    const a = hexToRgb(from);
    const b = hexToRgb(to);
    const out = [];

    for (let i = 0; i < steps; i += 1)
    {
        const t = steps === 1 ? 1 : i / (steps - 1);
        out.push(`rgb(${Math.round(a[0] + (b[0] - a[0]) * t)},${Math.round(a[1] + (b[1] - a[1]) * t)},${Math.round(a[2] + (b[2] - a[2]) * t)})`);
    }

    return out;
}

const HEAD_COLORS = ramp("#1f5a53", palette.amber, 8);
const FEED_COLORS = ramp("#123b37", palette.teal, 4);

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

function drawRing(ctx, ring, cx, cy, scale, colors, width, maxAlpha)
{
    if (ring.count < 2)
        return;

    const chunks = colors.length;
    const start = (ring.next - ring.count + ring.size) % ring.size;

    ctx.lineWidth = width;
    ctx.lineCap = "round";
    ctx.lineJoin = "round";

    for (let c = 0; c < chunks; c += 1)
    {
        const a = Math.floor(ring.count * c / chunks);
        const b = Math.floor(ring.count * (c + 1) / chunks);

        if (b - a < 2)
            continue;

        // Start one point early so neighbouring chunks share a segment.
        const from = a > 0 ? a - 1 : 0;
        ctx.beginPath();

        for (let k = from; k < b; k += 1)
        {
            const idx = (start + k) % ring.size;
            const px = cx + ring.xs[idx] * scale;
            const py = cy + ring.ys[idx] * scale;

            if (k === from)
                ctx.moveTo(px, py);
            else
                ctx.lineTo(px, py);
        }

        const t = (c + 1) / chunks;
        ctx.globalAlpha = maxAlpha * (0.08 + 0.92 * t * t);
        ctx.strokeStyle = colors[c];
        ctx.stroke();
    }

    ctx.globalAlpha = 1;
}

export function mount(canvas, lib)
{
    const sketch = new lib.SignalBloom();

    onPointer(canvas, (x, y) => sketch.setPointer(x, y));

    const head = makeRing(HEAD_MAX);
    const feed = makeRing(FEED_MAX);
    const ghost = makeRing(GHOST_MAX);
    let lastGhostX = NaN;
    let lastGhostY = NaN;

    createRunner(canvas, (ctx, dt, w, h) => {
        sketch.tick(dt);

        const headX = sketch.headX();
        const headY = sketch.headY();
        const feedX = sketch.feedX();
        const feedY = sketch.feedY();
        const ghostX = sketch.ghostX();
        const ghostY = sketch.ghostY();
        const rawPhase = sketch.phase();

        const hx = Number.isFinite(headX) ? headX : 0;
        const hy = Number.isFinite(headY) ? headY : 0;
        const fx = Number.isFinite(feedX) ? feedX : hx;
        const fy = Number.isFinite(feedY) ? feedY : hy;
        const gx = Number.isFinite(ghostX) ? ghostX : hx;
        const gy = Number.isFinite(ghostY) ? ghostY : hy;
        const phase = Number.isFinite(rawPhase) ? rawPhase : 0;

        const cx = w * 0.5;
        const cy = h * 0.5;
        const scale = Math.min(w, h) * 0.46;

        ctx.save();
        ctx.globalCompositeOperation = "source-over";
        ctx.globalAlpha = 1;
        ctx.fillStyle = palette.bg;
        ctx.fillRect(0, 0, w, h);
        ctx.restore();

        push(head, hx, hy);
        push(feed, fx, fy);

        if (!Number.isFinite(lastGhostX) || Math.hypot(gx - lastGhostX, gy - lastGhostY) > 1e-4)
        {
            push(ghost, gx, gy);
            lastGhostX = gx;
            lastGhostY = gy;
        }

        ctx.save();
        ctx.globalCompositeOperation = "lighter";

        drawRing(ctx, head, cx, cy, scale, HEAD_COLORS, 1.6, 0.62);
        drawRing(ctx, feed, cx, cy, scale, FEED_COLORS, 2.4, 0.52);

        // The ghost's samples step because the worker clock only runs every
        // fourth frame: leave a short dotted history rather than a smooth line.
        const ghostStart = (ghost.next - ghost.count + ghost.size) % ghost.size;
        ctx.fillStyle = palette.teal;

        for (let k = 0; k < ghost.count; k += 1)
        {
            const idx = (ghostStart + k) % ghost.size;
            const t = (k + 1) / Math.max(1, ghost.count);
            ctx.globalAlpha = 0.10 + 0.55 * t;
            ctx.beginPath();
            ctx.arc(cx + ghost.xs[idx] * scale, cy + ghost.ys[idx] * scale, 1 + 1.6 * t, 0, TAU);
            ctx.fill();
        }

        // A fine tether from the bright head to the lagging ghost.
        ctx.globalAlpha = 0.12;
        ctx.strokeStyle = palette.teal;
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(cx + hx * scale, cy + hy * scale);
        ctx.lineTo(cx + gx * scale, cy + gy * scale);
        ctx.stroke();

        const gpx = cx + gx * scale;
        const gpy = cy + gy * scale;
        ctx.globalAlpha = 0.7;
        ctx.lineWidth = 1.5;
        ctx.strokeStyle = palette.teal;
        ctx.beginPath();
        ctx.arc(gpx, gpy, 5.5, 0, TAU);
        ctx.stroke();

        const wobble = 0.5 + 0.5 * Math.sin(phase);
        const hpx = cx + hx * scale;
        const hpy = cy + hy * scale;

        ctx.globalAlpha = 0.16;
        ctx.fillStyle = palette.amber;
        ctx.beginPath();
        ctx.arc(hpx, hpy, 11 + 4 * wobble, 0, TAU);
        ctx.fill();

        ctx.globalAlpha = 0.95;
        ctx.beginPath();
        ctx.arc(hpx, hpy, 4, 0, TAU);
        ctx.fill();

        ctx.globalAlpha = 0.9;
        ctx.fillStyle = "#fff4dc";
        ctx.beginPath();
        ctx.arc(hpx, hpy, 1.8, 0, TAU);
        ctx.fill();

        ctx.restore();
        ctx.globalAlpha = 1;
    });

    return {
        reset() {
            sketch.reset();
            clearRing(head);
            clearRing(feed);
            clearRing(ghost);
            lastGhostX = NaN;
            lastGhostY = NaN;
        },
    };
}
