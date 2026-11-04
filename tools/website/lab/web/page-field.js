// Full-page ambient flow field for the lab's "landing-page arrangement": a
// fixed, bottom-layer canvas (behind all content) whose particles are
// stirred by the hop balls launched in Cursor Clocks. There is no big blob
// and no cursor obstacle — only the launched values disturb the flow.
//
// Trails are an explicit, bounded position history, not a persistent canvas:
// every particle remembers TRAIL positions and each age is drawn with an
// alpha that reaches exactly zero. (A persistent `destination-out` wash
// multiplies alpha asymptotically and sticks at 1/255 in 8-bit alpha, which
// left permanent ghost traces behind slow particles.)
//
// Cost: ~320–1000 particles; physics is one pass; calm frames draw only dots
// (three grouped fills) plus short-history length checks; interaction adds
// grouped trail strokes. No per-particle allocation.
//
// The field has no state in wasm; it consumes the dial's renderer state.

import { createRunner } from "../lab-common.js";

const COLORS = ["#7d97b4", "#8fb9a8", "#b0a3d6"];
const TAU = Math.PI * 2;

const BALL_R = 170; // influence radius (px)
const PUSH = 26; // radial push, px/s at influence 1
const SWIRL = 30; // tangential swirl, px/s at influence 1
const DRIFT = 5; // ambient drift, px/s

const TRAIL = 26; // history frames per particle
const TRAIL_ALPHA = 0.18; // newest segment alpha
const TRAIL_MIN = 0.25; // px; shorter segments are ambient jitter, skipped
const SEAM_LEN = 0.12; // fraction of min(w, h); longer means a wrap seam
const SEAM_ALPHA_SCALE = 0.35; // dim the seams only (field brightness intact)
const SEAM_WIDTH = 0.6; // px, thinner than a normal trail segment
const DOT_ALPHA = 0.35; // calm-state "stars"
const DOT_R = 0.7;

export function mount(canvas, dial)
{
    let x = null;
    let y = null;

    // Ring buffer of past positions, per particle: [i * TRAIL + slot].
    let hx = null;
    let hy = null;
    let head = 0;

    let count = 0;
    let lastW = 0;
    let lastH = 0;
    let elapsed = 0;
    let seed = 0x9e3779b9;

    function random()
    {
        seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;

        return seed / 4294967296;
    }

    function reseed(w, h)
    {
        count = Math.round(Math.min(1000, Math.max(320, (w * h) / 1600)));

        x = new Float32Array(count);
        y = new Float32Array(count);
        hx = new Float32Array(count * TRAIL);
        hy = new Float32Array(count * TRAIL);
        head = 0;

        for (let i = 0; i < count; ++i)
        {
            const xi = random() * w;
            const yi = random() * h;

            x[i] = xi;
            y[i] = yi;

            // Seed the whole history at the starting point, so the first
            // frames do not draw trails from (0, 0).
            for (let k = 0; k < TRAIL; ++k)
            {
                hx[i * TRAIL + k] = xi;
                hy[i * TRAIL + k] = yi;
            }
        }

        lastW = w;
        lastH = h;
    }

    const draw = (ctx, dt, w, h) => {
        if (w !== lastW || h !== lastH)
            reseed(w, h);

        elapsed += dt;

        // Nothing persists: every frame is composed from scratch, so there
        // is never an alpha residue to round in the first place.
        ctx.clearRect(0, 0, w, h);

        const balls = dial.balls();

        // Physics pass: a slow ambient drift plus a push-and-swirl around
        // every launched ball.
        for (let i = 0; i < count; ++i)
        {
            const xi = x[i];
            const yi = y[i];

            let vx = DRIFT * Math.sin(yi * 0.004 + elapsed * 0.13);
            let vy = DRIFT * Math.cos(xi * 0.004 + elapsed * 0.11);

            for (let b = 0; b < balls.length; ++b)
            {
                const ball = balls[b];
                const dx = xi - ball.x;
                const dy = yi - ball.y;
                const d2 = dx * dx + dy * dy + 900;
                const k = Math.min(2.6, (BALL_R * BALL_R) / d2) * ball.strength;
                const inv = 1 / Math.sqrt(d2);
                const nx = dx * inv;
                const ny = dy * inv;

                vx += (nx * PUSH - ny * SWIRL) * k;
                vy += (ny * PUSH + nx * SWIRL) * k;
            }

            let nx = xi + vx * dt;
            let ny = yi + vy * dt;

            // Wrap with a small margin so nothing pops in mid-screen.
            //
            // NOTE: the wrap seam is intentionally visible. When a ball
            // shoves particles across an edge, the segment from the old
            // position to the wrapped one spans the viewport; the long
            // streaks this produces during interaction are a kept aesthetic,
            // not a bug to "fix". Do not break the trail at the seam without
            // asking.
            if (nx < -8)
                nx += w + 16;
            else if (nx > w + 8)
                nx -= w + 16;

            if (ny < -8)
                ny += h + 16;
            else if (ny > h + 8)
                ny -= h + 16;

            x[i] = nx;
            y[i] = ny;

            hx[i * TRAIL + head] = nx;
            hy[i * TRAIL + head] = ny;
        }

        head = (head + 1) % TRAIL;

        ctx.save();
        ctx.globalCompositeOperation = "lighter";
        ctx.lineCap = "round";

        // Trails: grouped by age (alpha) then tint. The newest sample is
        // `head - 1`; older samples walk backwards around the ring. The
        // quadratic alpha reaches zero exactly at the oldest age.
        //
        // A wrap seam is a segment far longer than any real movement (a
        // particle crossing a viewport edge). Seams get their own pass with a
        // dimmer, thinner stroke: the streak effect stays, but it no longer
        // lifts the field's overall brightness. Tune SEAM_* to taste.
        const newest = (head - 1 + TRAIL) % TRAIL;
        const min2 = TRAIL_MIN * TRAIL_MIN;
        const seam = Math.min(w, h) * SEAM_LEN;
        const seam2 = seam * seam;

        for (let a = 0; a < TRAIL - 1; ++a)
        {
            const ia = (newest - a + TRAIL) % TRAIL;
            const ib = (ia - 1 + TRAIL) % TRAIL;
            const age = a / (TRAIL - 1);
            const alpha = TRAIL_ALPHA * (1 - age) * (1 - age);

            if (alpha < 0.004)
                break;

            for (let c = 0; c < COLORS.length; ++c)
            {
                ctx.strokeStyle = COLORS[c];
                ctx.lineCap = "round";

                // Ordinary short trail segments.
                ctx.beginPath();
                let any = false;

                for (let i = c; i < count; i += COLORS.length)
                {
                    const o = i * TRAIL;
                    const x0 = hx[o + ia];
                    const y0 = hy[o + ia];
                    const x1 = hx[o + ib];
                    const y1 = hy[o + ib];
                    const dx = x0 - x1;
                    const dy = y0 - y1;
                    const d2 = dx * dx + dy * dy;

                    if (d2 < min2 || d2 >= seam2)
                        continue;

                    ctx.moveTo(x1, y1);
                    ctx.lineTo(x0, y0);
                    any = true;
                }

                if (any)
                {
                    ctx.globalAlpha = alpha;
                    ctx.lineWidth = 0.8;
                    ctx.stroke();
                }

                // Wrap-seam segments, deliberately dimmer and thinner.
                ctx.beginPath();
                let seams = false;

                for (let i = c; i < count; i += COLORS.length)
                {
                    const o = i * TRAIL;
                    const x0 = hx[o + ia];
                    const y0 = hy[o + ia];
                    const x1 = hx[o + ib];
                    const y1 = hy[o + ib];
                    const dx = x0 - x1;
                    const dy = y0 - y1;

                    if (dx * dx + dy * dy < seam2)
                        continue;

                    ctx.moveTo(x1, y1);
                    ctx.lineTo(x0, y0);
                    seams = true;
                }

                if (seams)
                {
                    ctx.globalAlpha = alpha * SEAM_ALPHA_SCALE;
                    ctx.lineWidth = SEAM_WIDTH;
                    ctx.stroke();
                }
            }
        }

        // Calm state: every particle is a soft "star" (one fill per tint).
        for (let c = 0; c < COLORS.length; ++c)
        {
            ctx.fillStyle = COLORS[c];
            ctx.globalAlpha = DOT_ALPHA;
            ctx.beginPath();

            for (let i = c; i < count; i += COLORS.length)
            {
                ctx.moveTo(x[i] + DOT_R, y[i]);
                ctx.arc(x[i], y[i], DOT_R, 0, TAU);
            }

            ctx.fill();
        }

        ctx.restore();
    };

    const runner = createRunner(canvas, draw);

    return {
        reset()
        {
            lastW = 0;
            lastH = 0;

            if (runner.ctx)
                runner.ctx.clearRect(0, 0, runner.width, runner.height);

            runner.start();
        },
    };
}
