// Clock Loom renderer: three lanes, a phase marker per clock, continuous
// `latest` fibres and discrete `queued` beads. All phase/position decisions
// live in wasm; this file only draws (and lightly smooths the sampled fibre
// endpoints so `latest` reads as continuous rather than jumpy).

import { createRunner, palette } from "../lab-common.js";

const TAU = Math.PI * 2;
const DASH = [2, 7];
const BEAD_COUNT = 8;

function finite(value, fallback)
{
    return Number.isFinite(value) ? value : fallback;
}

// Wrap-aware approach in turns: move `current` toward `target` along the
// shorter arc of the unit circle.
function approach(current, target, amount)
{
    if (!Number.isFinite(current))
        return target;

    if (!Number.isFinite(target))
        return current;

    let delta = target - current;
    delta -= Math.round(delta);

    const next = current + delta * amount;

    return next - Math.floor(next);
}

function lane(ctx, left, right, y)
{
    ctx.fillStyle = "rgba(20, 24, 32, 0.55)";
    ctx.fillRect(left - 10, y - 15, right - left + 20, 30);

    ctx.strokeStyle = "rgba(139, 147, 164, 0.16)";
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(left, y);
    ctx.lineTo(right, y);

    for (let i = 0; i <= 4; ++i)
    {
        const x = left + ((right - left) * i) / 4;
        ctx.moveTo(x, y - 3);
        ctx.lineTo(x, y + 3);
    }

    ctx.stroke();
}

function trail(ctx, phase, y, left, span, color)
{
    ctx.save();
    ctx.strokeStyle = color;
    ctx.lineWidth = 2;
    ctx.lineCap = "round";

    for (let i = 0; i < 9; ++i)
    {
        const head = phase - i * 0.006;
        const tail = head - 0.006;

        if (tail < 0)
            break;

        ctx.globalAlpha = 0.30 * (1 - i / 9);
        ctx.beginPath();
        ctx.moveTo(left + tail * span, y);
        ctx.lineTo(left + head * span, y);
        ctx.stroke();
    }

    ctx.restore();
}

function marker(ctx, x, y, color)
{
    ctx.save();
    ctx.strokeStyle = color;
    ctx.shadowBlur = 12;
    ctx.shadowColor = color;
    ctx.lineWidth = 2.5;
    ctx.lineCap = "round";
    ctx.beginPath();
    ctx.moveTo(x, y - 9);
    ctx.lineTo(x, y + 9);
    ctx.stroke();

    ctx.fillStyle = color;
    ctx.beginPath();
    ctx.arc(x, y, 2.2, 0, TAU);
    ctx.fill();
    ctx.restore();
}

function fibre(ctx, x0, y0, x1, y1, color)
{
    const mid = (y0 + y1) * 0.5;
    const dir = x1 >= x0 ? 1 : -1;
    const bow = 20;

    ctx.save();
    ctx.strokeStyle = color;
    ctx.globalAlpha = 0.55;
    ctx.lineWidth = 1.4;
    ctx.setLineDash(DASH);
    ctx.shadowBlur = 9;
    ctx.shadowColor = color;
    ctx.beginPath();
    ctx.moveTo(x0, y0);
    ctx.bezierCurveTo(x0 + dir * bow, mid, x1 - dir * bow, mid, x1, y1);
    ctx.stroke();
    ctx.restore();
}

export function mount(canvas, lib)
{
    const sketch = new lib.ClockLoom();

    const beads = [];

    for (let i = 0; i < BEAD_COUNT; ++i)
        beads.push({ active: false, life: 0 });

    let lastPulse = NaN;
    let dispU = NaN;
    let dispW = NaN;
    let flash = 0;

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

    function spawnBead()
    {
        let slot = null;

        for (let i = 0; i < beads.length; ++i)
        {
            if (!beads[i].active)
            {
                slot = beads[i];
                break;
            }
        }

        if (!slot)
            slot = beads[0];

        slot.active = true;
        slot.life = 0;
    }

    const draw = (ctx, dt, w, h) => {
        sketch.tick(dt);

        const aPhase = finite(sketch.aPhase(), 0);
        const uPhase = finite(sketch.uPhase(), 0);
        const wPhase = finite(sketch.wPhase(), 0);
        const uSample = finite(sketch.uSample(), uPhase);
        const wSample = finite(sketch.wSample(), wPhase);
        const pulse = sketch.pulse();

        const left = w * 0.20;
        const right = w * 0.95;
        const span = right - left;
        const yA = h * 0.26;
        const yU = h * 0.50;
        const yW = h * 0.74;

        ctx.fillStyle = palette.bg;
        ctx.fillRect(0, 0, w, h);

        lane(ctx, left, right, yA);
        lane(ctx, left, right, yU);
        lane(ctx, left, right, yW);

        // Fibre endpoints are eased toward the sampled value; the worker lane
        // samples only every fourth frame, so this keeps the thread continuous
        // while the marker itself still shows the true phase.
        dispU = approach(dispU, uSample, 1 - Math.exp(-dt * 14));
        dispW = approach(dispW, wSample, 1 - Math.exp(-dt * 8));

        const ax = left + aPhase * span;
        const ux = left + uPhase * span;
        const wx = left + wPhase * span;
        const sux = left + dispU * span;
        const swx = left + dispW * span;

        fibre(ctx, ax, yA, sux, yU, palette.teal);
        fibre(ctx, ux, yU, swx, yW, palette.teal);

        // A queued event is a bead falling down its own channel; a fresh
        // (finite, changed) pulse starts one, an empty drain reads NaN.
        const qx = left + span * 0.72;

        ctx.save();
        ctx.strokeStyle = "rgba(255, 180, 84, 0.20)";
        ctx.lineWidth = 1;
        ctx.setLineDash([2, 8]);
        ctx.beginPath();
        ctx.moveTo(qx, yU);
        ctx.lineTo(qx, yW);
        ctx.stroke();
        ctx.restore();

        if (Number.isFinite(pulse) && pulse !== lastPulse)
        {
            lastPulse = pulse;
            spawnBead();
        }

        for (let i = 0; i < beads.length; ++i)
        {
            const bead = beads[i];

            if (!bead.active)
                continue;

            bead.life += dt / 0.52;

            if (bead.life >= 1)
            {
                bead.active = false;
                flash = 1;
                continue;
            }

            const t = bead.life;
            const eased = t * t * (3 - 2 * t);
            const bx = qx + Math.sin(t * Math.PI) * 8;
            const by = yU + (yW - yU) * eased;
            const radius = 2.2 + Math.sin(t * Math.PI) * 1.8;

            ctx.save();
            ctx.fillStyle = palette.amber;
            ctx.globalAlpha = 0.85;
            ctx.shadowBlur = 12;
            ctx.shadowColor = palette.amber;
            ctx.beginPath();
            ctx.arc(bx, by, radius, 0, TAU);
            ctx.fill();
            ctx.restore();
        }

        if (flash > 0)
        {
            ctx.save();
            ctx.strokeStyle = palette.amber;
            ctx.globalAlpha = flash * 0.8;
            ctx.lineWidth = 1.5;
            ctx.shadowBlur = 10;
            ctx.shadowColor = palette.amber;
            ctx.beginPath();
            ctx.arc(qx, yW, 5 + (1 - flash) * 13, 0, TAU);
            ctx.stroke();
            ctx.restore();

            flash = Math.max(0, flash - dt * 2.2);
        }

        trail(ctx, aPhase, yA, left, span, palette.amber);
        trail(ctx, uPhase, yU, left, span, palette.teal);
        trail(ctx, wPhase, yW, left, span, palette.violet);

        marker(ctx, ax, yA, palette.amber);
        marker(ctx, ux, yU, palette.teal);
        marker(ctx, wx, yW, palette.violet);

        ctx.save();
        ctx.font = "11px ui-monospace, SFMono-Regular, Menlo, monospace";
        ctx.textBaseline = "middle";
        ctx.textAlign = "left";
        ctx.fillStyle = palette.dim;
        ctx.fillText("audio ×3", 10, yA);
        ctx.fillText("ui ×1", 10, yU);
        ctx.fillText("worker ÷4", 10, yW);

        ctx.font = "10px ui-monospace, SFMono-Regular, Menlo, monospace";
        ctx.fillStyle = "rgba(63, 160, 140, 0.65)";
        ctx.fillText("latest", left + 4, yU - 20);
        ctx.textAlign = "right";
        ctx.fillStyle = "rgba(255, 180, 84, 0.65)";
        ctx.fillText("queued", qx - 8, (yU + yW) * 0.5);
        ctx.restore();
    };

    const runner = createRunner(canvas, draw);

    function reset()
    {
        sketch.reset();

        lastPulse = NaN;
        dispU = NaN;
        dispW = NaN;
        flash = 0;

        for (let i = 0; i < beads.length; ++i)
            beads[i].active = false;

        clearCanvas();
        draw(runner.ctx, 0, runner.width, runner.height);
        runner.start();
    }

    return { reset };
}
