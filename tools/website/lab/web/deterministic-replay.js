import { createRunner, palette } from "../lab-common.js";

const TAU = Math.PI * 2;
const MAX_POINTS = 480;
const HEAD_POINTS = 90;
const TRAIL_LIFE = 5.8;

function finite(value, fallback)
{
    return Number.isFinite(value) ? value : fallback;
}

export function mount(canvas, lib)
{
    const sketch = new lib.DeterministicReplay();

    const xs = new Float64Array(MAX_POINTS);
    const ys = new Float64Array(MAX_POINTS);
    const births = new Float64Array(MAX_POINTS);

    let head = -1;
    let count = 0;
    let elapsed = 0;

    function append(x, y)
    {
        head = (head + 1) % MAX_POINTS;
        xs[head] = x;
        ys[head] = y;
        births[head] = elapsed;
        count = Math.min(count + 1, MAX_POINTS);
    }

    function indexAt(offset)
    {
        return (head - count + 1 + offset + MAX_POINTS) % MAX_POINTS;
    }

    function paintBackground(ctx, w, h)
    {
        ctx.save();
        ctx.globalCompositeOperation = "source-over";
        ctx.globalAlpha = 1;
        ctx.fillStyle = palette.bg;
        ctx.fillRect(0, 0, w, h);
        ctx.restore();
    }

    function strokeTrail(ctx, from, to, size, offsetX, offsetY, color, alpha, width)
    {
        if (to - from < 1)
            return;

        ctx.beginPath();

        let started = false;

        for (let k = from; k <= to; k += 1)
        {
            const index = indexAt(k);
            const x = offsetX + xs[index] * size;
            const y = offsetY + ys[index] * size;

            if (!Number.isFinite(x) || !Number.isFinite(y))
            {
                started = false;
                continue;
            }

            if (started)
                ctx.lineTo(x, y);
            else
            {
                ctx.moveTo(x, y);
                started = true;
            }
        }

        ctx.strokeStyle = color;
        ctx.globalAlpha = alpha;
        ctx.lineWidth = width;
        ctx.stroke();
    }

    function drawPoint(ctx, x, y, color, radius)
    {
        ctx.fillStyle = color;

        ctx.globalAlpha = 0.10;
        ctx.beginPath();
        ctx.arc(x, y, radius * 4.2, 0, TAU);
        ctx.fill();

        ctx.globalAlpha = 0.22;
        ctx.beginPath();
        ctx.arc(x, y, radius * 2.1, 0, TAU);
        ctx.fill();

        ctx.globalAlpha = 0.95;
        ctx.beginPath();
        ctx.arc(x, y, radius, 0, TAU);
        ctx.fill();
    }

    function draw(ctx, dt, w, h)
    {
        const step = Number.isFinite(dt) ? Math.min(Math.max(dt, 0), 0.05) : 1 / 60;
        elapsed += step;

        paintBackground(ctx, w, h);

        sketch.tick(step);

        const x = finite(sketch.x(), 0.5);
        const y = finite(sketch.y(), 0.5);
        const x2 = finite(sketch.x2(), x);
        const y2 = finite(sketch.y2(), y);

        append(x, y);

        const size = Math.min(w, h) * 0.78;
        const offsetX = (w - size) * 0.5;
        const offsetY = (h - size) * 0.5;

        let first = 0;

        while (first < count && elapsed - births[indexAt(first)] > TRAIL_LIFE)
            first += 1;

        ctx.save();
        ctx.lineCap = "round";
        ctx.lineJoin = "round";

        strokeTrail(
            ctx,
            first,
            count - 1,
            size,
            offsetX,
            offsetY,
            palette.teal,
            0.20,
            Math.max(1, Math.min(w, h) / 520),
        );

        strokeTrail(
            ctx,
            Math.max(first, count - HEAD_POINTS),
            count - 1,
            size,
            offsetX,
            offsetY,
            palette.amber,
            0.82,
            Math.max(1.2, Math.min(w, h) / 300),
        );

        const px = offsetX + x * size;
        const py = offsetY + y * size;
        const qx = offsetX + x2 * size;
        const qy = offsetY + y2 * size;

        ctx.strokeStyle = palette.violet;
        ctx.globalAlpha = 0.28;
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(px, py);
        ctx.lineTo(qx, qy);
        ctx.stroke();

        drawPoint(ctx, qx, qy, palette.violet, Math.max(2.2, size * 0.007));
        drawPoint(ctx, px, py, palette.amber, Math.max(2.8, size * 0.009));

        ctx.restore();
    }

    const runner = createRunner(canvas, draw);

    function reset()
    {
        sketch.reset();

        head = -1;
        count = 0;
        elapsed = 0;

        if (runner.ctx)
        {
            if (runner.reduced)
                draw(runner.ctx, 0, runner.width, runner.height);
            else
                paintBackground(runner.ctx, runner.width, runner.height);
        }
    }

    return { reset };
}
