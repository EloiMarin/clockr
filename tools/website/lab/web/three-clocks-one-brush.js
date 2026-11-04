// Three Clocks, One Brush renderer: a persistent canvas that fades very slowly,
// three translucent strokes, and a small brush head per clock. The stepped
// brush is the one whose coordinates arrive through a `latest` crossing from
// the worker clock; its line segments appear only on worker frames.

import { createRunner, palette } from "../lab-common.js";

const TAU = Math.PI * 2;
const COLORS = [palette.amber, palette.teal, palette.violet];
const WIDTHS = [1.3, 2.0, 1.6];
const HEADS = [1.6, 2.2, 1.8];

function finite(value, fallback)
{
    return Number.isFinite(value) ? value : fallback;
}

export function mount(canvas, lib)
{
    const sketch = new lib.ThreeClocksOneBrush();
    const points = [
        { x: 0, y: 0, valid: false },
        { x: 0, y: 0, valid: false },
        { x: 0, y: 0, valid: false },
    ];

    function clearCanvas()
    {
        const ctx = canvas.getContext("2d");

        if (!ctx)
            return;

        ctx.save();
        ctx.setTransform(1, 0, 0, 1, 0, 0);
        ctx.fillStyle = palette.bg;
        ctx.fillRect(0, 0, canvas.width, canvas.height);
        ctx.restore();
    }

    function paint(ctx, index, x, y, pad, innerW, innerH)
    {
        const ux = Math.min(1, Math.max(0, x));
        const uy = Math.min(1, Math.max(0, y));
        const px = pad + ux * innerW;
        const py = pad + uy * innerH;
        const point = points[index];

        if (point.valid)
        {
            ctx.save();
            ctx.strokeStyle = COLORS[index];
            ctx.globalAlpha = 0.5;
            ctx.lineWidth = WIDTHS[index];
            ctx.lineCap = "round";
            ctx.lineJoin = "round";
            ctx.beginPath();
            ctx.moveTo(point.x, point.y);
            ctx.lineTo(px, py);
            ctx.stroke();
            ctx.restore();
        }

        point.x = px;
        point.y = py;
        point.valid = true;

        ctx.save();
        ctx.fillStyle = COLORS[index];
        ctx.globalAlpha = 0.55;
        ctx.shadowBlur = 9;
        ctx.shadowColor = COLORS[index];
        ctx.beginPath();
        ctx.arc(px, py, HEADS[index], 0, TAU);
        ctx.fill();
        ctx.restore();
    }

    const draw = (ctx, dt, w, h) => {
        sketch.tick(dt);

        // A very slow fade lets strokes pile up into a painting while the
        // oldest marks eventually recede into the background.
        ctx.save();
        ctx.globalAlpha = 0.012;
        ctx.fillStyle = palette.bg;
        ctx.fillRect(0, 0, w, h);
        ctx.restore();

        const pad = Math.min(w, h) * 0.035;
        const innerW = w - pad * 2;
        const innerH = h - pad * 2;

        paint(ctx, 0, finite(sketch.x0(), 0.5), finite(sketch.y0(), 0.5), pad, innerW, innerH);
        paint(ctx, 1, finite(sketch.x1(), 0.5), finite(sketch.y1(), 0.5), pad, innerW, innerH);
        paint(ctx, 2, finite(sketch.x2(), 0.5), finite(sketch.y2(), 0.5), pad, innerW, innerH);
    };

    const runner = createRunner(canvas, draw);

    function reset()
    {
        sketch.reset();

        for (let i = 0; i < points.length; ++i)
            points[i].valid = false;

        clearCanvas();
        draw(runner.ctx, 0, runner.width, runner.height);
        runner.start();
    }

    return { reset };
}
