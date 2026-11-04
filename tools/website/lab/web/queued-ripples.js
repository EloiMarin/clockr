import { createRunner, onPointer, palette } from "../lab-common.js";

const TAU = Math.PI * 2;
const MAX_RINGS = 96;
const RING_LIFE = 1.5;

function finite(value, fallback)
{
    return Number.isFinite(value) ? value : fallback;
}

export function mount(canvas, lib)
{
    const sketch = new lib.QueuedRipples();

    const rings = Array.from({ length: MAX_RINGS }, () => ({
        x: 0.5,
        y: 0.5,
        age: 0,
        life: RING_LIFE,
        active: false,
    }));

    let ringCursor = 0;
    let lastDrained = Math.round(finite(sketch.drained(), 0));
    let elapsed = 0;
    let autoT = 0;
    let nextAuto = 0.8;
    let userUntil = 0;

    function spawn(x, y)
    {
        const ring = rings[ringCursor];
        ringCursor = (ringCursor + 1) % MAX_RINGS;
        ring.x = x;
        ring.y = y;
        ring.age = 0;
        ring.life = RING_LIFE;
        ring.active = true;
    }

    function draw(ctx, dt, w, h)
    {
        const step = Number.isFinite(dt) ? Math.min(Math.max(dt, 0), 0.05) : 1 / 60;
        elapsed += step;

        ctx.save();
        ctx.globalCompositeOperation = "source-over";
        ctx.globalAlpha = 1;
        ctx.fillStyle = palette.bg;
        ctx.fillRect(0, 0, w, h);
        ctx.restore();

        // Auto-queue a slow wandering event stream; a real pointer takes over
        // the live ring (and can add clicks) for five seconds.
        autoT += step;

        const autoX = 0.5 + 0.28 * Math.cos(autoT * 0.8);
        const autoY = 0.5 + 0.24 * Math.sin(autoT * 1.3);

        if (autoT > userUntil)
            sketch.setPointer(autoX, autoY);

        // Keep the queued stream running even while the live pointer is under
        // the user's control; the point of the sketch is drained events.
        if (autoT >= nextAuto)
        {
            sketch.push(autoX, autoY);
            nextAuto = autoT + 1.05 + 0.3 * Math.sin(autoT * 2);
        }

        sketch.tick(step);

        const drained = Math.round(finite(sketch.drained(), lastDrained));

        while (lastDrained < drained)
        {
            spawn(finite(sketch.eventX(), 0.5), finite(sketch.eventY(), 0.5));
            lastDrained += 1;
        }

        const minDim = Math.max(1, Math.min(w, h));

        ctx.save();
        ctx.lineCap = "round";

        for (let i = 0; i < MAX_RINGS; i += 1)
        {
            const ring = rings[i];

            if (!ring.active)
                continue;

            ring.age += step;

            if (ring.age >= ring.life)
            {
                ring.active = false;
                continue;
            }

            const t = ring.age / ring.life;
            const ease = 1 - (1 - t) ** 3;
            const radius = (0.035 + 0.32 * ease) * minDim;
            const x = ring.x * w;
            const y = ring.y * h;
            const fade = (1 - t) * (1 - t);

            ctx.strokeStyle = palette.amber;
            ctx.globalAlpha = fade * 0.16;
            ctx.lineWidth = 8 * (1 - t) + 1;
            ctx.beginPath();
            ctx.arc(x, y, radius, 0, TAU);
            ctx.stroke();

            ctx.globalAlpha = fade * 0.9;
            ctx.lineWidth = Math.max(1, 2.2 * (1 - t));
            ctx.beginPath();
            ctx.arc(x, y, radius, 0, TAU);
            ctx.stroke();

            ctx.fillStyle = palette.amber;
            ctx.globalAlpha = fade * 0.55;
            ctx.beginPath();
            ctx.arc(x, y, 1.4 + fade * 1.4, 0, TAU);
            ctx.fill();
        }

        const liveX = finite(sketch.latestX(), 0.5) * w;
        const liveY = finite(sketch.latestY(), 0.5) * h;
        const pulse = 0.5 + 0.5 * Math.sin(elapsed * 3.4);

        ctx.strokeStyle = palette.teal;
        ctx.globalAlpha = 0.62;
        ctx.lineWidth = 1.4;
        ctx.beginPath();
        ctx.arc(liveX, liveY, 8 + pulse * 4, 0, TAU);
        ctx.stroke();

        ctx.globalAlpha = 0.22;
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.arc(liveX, liveY, 16 + pulse * 7, 0, TAU);
        ctx.stroke();

        ctx.fillStyle = palette.teal;
        ctx.globalAlpha = 0.95;
        ctx.beginPath();
        ctx.arc(liveX, liveY, 2.6, 0, TAU);
        ctx.fill();

        ctx.restore();

        const count = Math.round(finite(sketch.count(), 0));
        const depth = Math.round(finite(sketch.queueDepth(), 0));
        const font = Math.max(11, Math.round(h * 0.032));

        ctx.save();
        ctx.font = `600 ${font}px ui-monospace, "SF Mono", Menlo, monospace`;
        ctx.textAlign = "left";
        ctx.textBaseline = "top";
        ctx.fillStyle = palette.dim;
        ctx.globalAlpha = 0.9;
        ctx.fillText(`events ${count}`, 14, 12);

        if (depth > 0)
        {
            ctx.fillStyle = palette.amber;
            ctx.globalAlpha = 0.95;
            ctx.fillText(`queued ${depth}`, 14, 12 + font + 4);
        }

        if (count === 0)
        {
            ctx.fillStyle = palette.dim;
            ctx.globalAlpha = 0.6;
            ctx.textBaseline = "bottom";
            ctx.fillText("click to queue", 14, h - 12);
        }

        ctx.restore();
    }

    const runner = createRunner(canvas, draw);

    onPointer(canvas, (x, y, type) => {
        userUntil = autoT + 5;

        if (type === "pointerdown")
            sketch.push(x, y);

        sketch.setPointer(x, y);
    });

    function reset()
    {
        sketch.reset();

        for (let i = 0; i < MAX_RINGS; i += 1)
            rings[i].active = false;

        ringCursor = 0;
        lastDrained = 0;
        elapsed = 0;
        autoT = 0;
        nextAuto = 0.8;
        userUntil = 0;
        runner.resize?.();

        if (runner.reduced && runner.ctx)
            draw(runner.ctx, 0, runner.width, runner.height);
    }

    return { reset };
}
