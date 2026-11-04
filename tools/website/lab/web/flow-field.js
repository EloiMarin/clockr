import { createRunner, onPointer, palette, rng } from "../lab-common.js";

const TAU = Math.PI * 2;
const COUNT = 260;
const SEED = 0x5f3759df;

function finite(value, fallback)
{
    return Number.isFinite(value) ? value : fallback;
}

export function mount(canvas, lib)
{
    const sketch = new lib.FlowField();

    const x = new Float32Array(COUNT);
    const y = new Float32Array(COUNT);
    const wobble = new Float32Array(COUNT);
    const drift = new Float32Array(COUNT);
    const tint = new Uint8Array(COUNT);

    const colors = [palette.teal, palette.violet, palette.amber];

    let needsClear = true;
    let lastWidth = 0;
    let lastHeight = 0;

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
            tint[i] = pick < 0.56 ? 0 : pick < 0.83 ? 1 : 2;
        }
    }

    function draw(ctx, dt, w, h)
    {
        ctx.globalAlpha = 1;
        ctx.globalCompositeOperation = "source-over";

        if (needsClear || w !== lastWidth || h !== lastHeight)
        {
            ctx.fillStyle = palette.bg;
            ctx.fillRect(0, 0, w, h);
            needsClear = false;
            lastWidth = w;
            lastHeight = h;
        }
        else
        {
            ctx.fillStyle = "rgba(11, 13, 16, 0.115)";
            ctx.fillRect(0, 0, w, h);
        }

        sketch.tick(dt);

        const angle = finite(sketch.angle(), 0);
        const curl = Math.min(2.4, Math.max(0, finite(sketch.curl(), 1)));
        const scale = Math.min(6, Math.max(0.25, finite(sketch.scale(), 2)));
        const speed = Math.min(320, Math.max(0, finite(sketch.speed(), 96)));
        const warpX = Math.min(0.7, Math.max(-0.7, finite(sketch.warpX(), 0)));
        const warpY = Math.min(0.7, Math.max(-0.7, finite(sketch.warpY(), 0)));

        const targetX = 0.5 + warpX;
        const targetY = 0.5 + warpY;
        const stepScale = speed * dt * Math.max(0.65, w / 480);

        ctx.lineCap = "round";
        ctx.lineWidth = Math.max(1, w / 620);

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

            const dx = px - targetX;
            const dy = py - targetY;
            const pull = 0.72 / (1 + (dx * dx + dy * dy) * 34);
            heading += (-dy * 2.2 - dx * 0.8) * pull;

            const pxStep = Math.cos(heading) * stepScale * drift[i];
            const pyStep = Math.sin(heading) * stepScale * drift[i];

            let nx = px + pxStep / w;
            let ny = py + pyStep / h;
            let wrapped = false;

            if (!Number.isFinite(nx) || !Number.isFinite(ny))
            {
                nx = 0.5;
                ny = 0.5;
                wrapped = true;
            }
            else
            {
                wrapped = nx < 0 || nx >= 1 || ny < 0 || ny >= 1;
                nx -= Math.floor(nx);
                ny -= Math.floor(ny);
            }

            x[i] = nx;
            y[i] = ny;

            if (wrapped)
                continue;

            ctx.globalAlpha = 0.34 + drift[i] * 0.22;
            ctx.strokeStyle = colors[tint[i]];
            ctx.beginPath();
            ctx.moveTo(px * w, py * h);
            ctx.lineTo(nx * w, ny * h);
            ctx.stroke();
        }

        ctx.globalAlpha = 1;
    }

    seed();

    const runner = createRunner(canvas, draw);

    onPointer(canvas, (px, py) => sketch.setPointer(px, py));

    function reset()
    {
        sketch.reset();
        seed();
        needsClear = true;
        runner.resize?.();
    }

    return { reset };
}
