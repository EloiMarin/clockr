// Ink Echoes renderer: a persistent canvas. Each frame each lane draws the
// stroke segment from where it was to where it is now, so the ink accumulates
// and the deep lanes finish strokes the pointer already finished drawing.
// Restart wipes the canvas.

import { createRunner, onPointer, palette } from "../lab-common.js";

const LANES = [
    { color: palette.amber, width: 2.6, alpha: 0.50 },
    { color: palette.teal, width: 2.0, alpha: 0.38 },
    { color: palette.violet, width: 1.5, alpha: 0.28 },
    { color: palette.blue, width: 1.1, alpha: 0.20 },
];

const MOVE_EPS = 0.0006;

export function mount(canvas, lib)
{
    const sketch = new lib.InkEchoes();

    const prevX = LANES.map(() => NaN);
    const prevY = LANES.map(() => NaN);
    let hasPrev = false;

    let autoT = 0;
    let userUntil = 0;

    onPointer(canvas, (x, y, type) => {
        if (type !== "leave")
        {
            userUntil = autoT + 5;
            sketch.setPointer(x, y);
        }
    });

    createRunner(canvas, (ctx, dt, w, h) => {
        autoT += dt;

        // A slow, deliberate wander draws a signature until the first real
        // gesture; a real gesture takes over for five seconds.
        if (autoT > userUntil)
        {
            sketch.setPointer(
                0.5 + 0.22 * Math.sin(autoT * 0.62) * Math.cos(autoT * 0.13),
                0.5 + 0.20 * Math.sin(autoT * 0.47 + 1.2),
            );
        }

        sketch.tick(dt);

        ctx.lineCap = "round";
        ctx.lineJoin = "round";

        for (let k = 0; k < LANES.length; k += 1)
        {
            const x = sketch.laneX(k);
            const y = sketch.laneY(k);

            if (!Number.isFinite(x) || !Number.isFinite(y))
                continue;

            if (hasPrev && Number.isFinite(prevX[k]))
            {
                const moved = Math.max(Math.abs(x - prevX[k]), Math.abs(y - prevY[k]));

                if (moved > MOVE_EPS)
                {
                    const lane = LANES[k];
                    ctx.globalAlpha = lane.alpha;
                    ctx.strokeStyle = lane.color;
                    ctx.lineWidth = lane.width;
                    ctx.beginPath();
                    ctx.moveTo(prevX[k] * w, prevY[k] * h);
                    ctx.lineTo(x * w, y * h);
                    ctx.stroke();
                }
            }

            prevX[k] = x;
            prevY[k] = y;

            hasPrev = true;
        }

        ctx.globalAlpha = 1;
    });

    return {
        reset() {
            sketch.reset();
            hasPrev = false;
            autoT = 0;
            userUntil = 0;

            const ctx = canvas.getContext("2d");
            ctx.fillStyle = palette.bg;
            ctx.fillRect(0, 0, canvas.width, canvas.height);
        },
    };
}
