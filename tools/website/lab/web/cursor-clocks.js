// Cursor Clocks renderer: Clock Loom's three rates as concentric clock
// faces pinned exactly under the cursor (no drift, no self-movement). All
// the temporal decisions still live in wasm: phases, latest crossings
// (dashed threads), and the click hop — one value crossing inner → middle →
// outer, scheduled at each crossing.
//
// The hop is staged like a hand-off: the value flies in from the cursor tip
// in the direction the cursor last moved, sticks onto the inner ring, and
// each ring's own hand (its clock's orbiting head) must *touch* it before it
// travels the dashed spoke to the next ring — the inner spinner's hand, then
// the middle hand, then the worker dial, which finally sends it off outward
// to fade away. The waits make the queued scheduling at the rings visible.

import { createRunner, onPointer } from "../lab-common.js";

// Softer, warmer variants than the shared lab palette: low saturation so the
// widget reads as a friendly orbit, not an instrument.
const soft = {
    audio: "#e7bd93",
    ui: "#8fb9a8",
    worker: "#b0a3d6",
};

const TAU = Math.PI * 2;

function finite(value, fallback)
{
    return Number.isFinite(value) ? value : fallback;
}

function pointAt(cx, cy, radius, angle)
{
    return [cx + Math.cos(angle) * radius, cy + Math.sin(angle) * radius];
}

// Signed shortest-arc distance between two angles.
function angDist(a, b)
{
    let d = (a - b) % TAU;

    if (d < -Math.PI)
        d += TAU;

    if (d > Math.PI)
        d -= TAU;

    return Math.abs(d);
}

// The angle at which a ray from (x, y) along (dirX, dirY) hits the ring of
// radius r centered at (cx, cy). If the ray misses (or points away), falls
// back toward the centre so the stick point always resolves.
function stickAngle(cx, cy, x, y, dirX, dirY, r)
{
    const ox = x - cx;
    const oy = y - cy;
    const c = ox * ox + oy * oy - r * r;

    let dx = dirX;
    let dy = dirY;
    let t;

    for (let attempt = 0; attempt < 2; ++attempt)
    {
        const b = ox * dx + oy * dy;
        const disc = b * b - c;

        if (disc >= 0)
        {
            // Nearest forward root: the value sticks at the *entry* point,
            // not after crossing the ring.
            t = -b - Math.sqrt(disc);

            if (t < 0)
                t = -b + Math.sqrt(disc);

            break;
        }

        // The ray misses the ring: re-aim straight at the centre and retry.
        const len = Math.hypot(ox, oy) || 1;
        dx = -ox / len;
        dy = -oy / len;
        t = undefined;
    }

    if (t === undefined || t < 0)
        t = Math.max(0, -(ox * dx + oy * dy));

    return Math.atan2(y + dy * t - cy, x + dx * t - cx);
}

// The dial follows the visitor everywhere, so it is sized in pixels rather
// than with the viewport: "big but subtle" — roughly 78 / 150 / 222 px at
// the cap, with proportionally spaced dots so large rings stay airy.
function ringRadii(w, h)
{
    const unit = Math.min(Math.min(w, h), 600);

    return { audio: unit * 0.13, ui: unit * 0.25, worker: unit * 0.37 };
}

// Soft dotted orbits, each with its own dot phase — no radial ticks, no dial
// face: the widget should read as a gentle orbit, not a targeting reticle.
function ring(ctx, cx, cy, radius, color, phase)
{
    // Dot spacing grows with the ring so a big dial stays airy, not busy.
    const spacing = Math.max(5, radius * 0.12);

    ctx.save();
    ctx.strokeStyle = color;

    // A faint continuous circumference under the dots: without it the marks
    // read as specks floating around the cursor rather than orbits.
    ctx.globalAlpha = 0.08;
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.arc(cx, cy, radius, 0, TAU);
    ctx.stroke();

    ctx.globalAlpha = 0.20;
    ctx.lineWidth = 1.2;
    ctx.lineCap = "round";
    ctx.setLineDash([0, spacing]);
    ctx.lineDashOffset = phase * spacing;
    ctx.beginPath();
    ctx.arc(cx, cy, radius, 0, TAU);
    ctx.stroke();
    ctx.restore();
}

// A fading arc behind the head, drawn additively so two trails crossing over
// each other brighten the Stirred Field way.
function trail(ctx, cx, cy, radius, angle, span, color, width)
{
    const segments = 14;

    ctx.save();
    ctx.globalCompositeOperation = "lighter";
    ctx.strokeStyle = color;
    ctx.lineCap = "round";

    for (let i = 0; i < segments; ++i)
    {
        const t0 = angle - (span * i) / segments;
        const t1 = angle - (span * (i + 1)) / segments;

        ctx.globalAlpha = 0.13 * (1 - i / segments);
        ctx.lineWidth = width * (1 - 0.6 * (i / segments));
        ctx.beginPath();
        ctx.arc(cx, cy, radius, t1, t0);
        ctx.stroke();
    }

    ctx.restore();
}

function head(ctx, cx, cy, radius, angle, color, size)
{
    const [x, y] = pointAt(cx, cy, radius, angle);

    ctx.save();
    ctx.fillStyle = color;
    ctx.globalAlpha = 0.9;
    ctx.shadowBlur = 9;
    ctx.shadowColor = color;
    ctx.beginPath();
    ctx.arc(x, y, size, 0, TAU);
    ctx.fill();
    ctx.restore();
}

// A latest crossing: dashed thread bowing inward between the producer's head
// and the sampled echo on the consumer's ring.
function fibre(ctx, cx, cy, r0, a0, r1, a1, color)
{
    const [x0, y0] = pointAt(cx, cy, r0, a0);
    const [x1, y1] = pointAt(cx, cy, r1, a1);

    // Wrap-aware midpoint of the two angles, along the shorter arc.
    let d = (a1 - a0) % TAU;

    if (d < -Math.PI)
        d += TAU;

    if (d > Math.PI)
        d -= TAU;

    const [xm, ym] = pointAt(cx, cy, Math.min(r0, r1) * 0.68, a0 + d * 0.5);

    ctx.save();
    ctx.strokeStyle = color;
    ctx.globalAlpha = 0.13;
    ctx.lineWidth = 0.9;
    ctx.setLineDash([1.5, 6]);
    ctx.shadowBlur = 4;
    ctx.shadowColor = color;
    ctx.beginPath();
    ctx.moveTo(x0, y0);
    ctx.quadraticCurveTo(xm, ym, x1, y1);
    ctx.stroke();
    ctx.restore();

    // The sampled echo: a small hollow circle where the consumer's clock sees
    // the producer. The outer one visibly hops every fourth frame.
    ctx.save();
    ctx.strokeStyle = color;
    ctx.globalAlpha = 0.30;
    ctx.lineWidth = 0.9;
    ctx.beginPath();
    ctx.arc(x1, y1, 2, 0, TAU);
    ctx.stroke();
    ctx.restore();
}

export function mount(canvas, lib, options = {})
{
    const sketch = new lib.CursorClocks();

    // Exact cursor position in canvas fraction; it never drifts and never
    // moves on its own. Starts centred until the first gesture.
    let centerX = 0.5;
    let centerY = 0.5;

    // Direction of the last cursor movement (canvas-fraction units per
    // axis). The click value flies off in this direction.
    let dirX = 1;
    let dirY = 0;
    let lastX = null;
    let lastY = null;

    // One hand-off ball per click value, staged: fly → waitAudio → toMid →
    // waitMid → toOut → waitOut → exit.
    const sparks = new Map();

    // Dense 1-based ids assigned in click order; the wasm counter publishes
    // them in the same order, so spark states pair up with hopU/hopW events.
    let pushSeq = 0;

    // Hop events that have fired, by click id (each crossing delivers each
    // id once; set membership gates the hand-off advances).
    const hopUFired = new Set();
    const hopWFired = new Set();

    let lastHopU = NaN;
    let lastHopW = NaN;
    let elapsed = 0;

    function clearCanvas()
    {
        const ctx = canvas.getContext("2d");

        if (!ctx)
            return;

        ctx.save();
        ctx.setTransform(1, 0, 0, 1, 0, 0);
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        ctx.restore();
    }

    function handlePointer(x, y, type, event)
    {
        if (type !== "leave")
        {
            centerX = x;
            centerY = y;
        }
        else
        {
            lastX = null;
            lastY = null;
        }

        if (type === "pointermove")
        {
            if (lastX !== null && lastY !== null)
            {
                const dx = x - lastX;
                const dy = y - lastY;
                const len = Math.hypot(dx, dy);

                if (len > 0.002)
                {
                    dirX = dx / len;
                    dirY = dy / len;
                }
            }

            lastX = x;
            lastY = y;
        }

        if (type === "pointerdown")
        {
            // Page-wide, a click on a control must not also launch a value.
            if (options.windowPointer && event?.target?.closest?.(
                "a, button, input, select, textarea, [role='button']"))
                return;

            const rect = canvas.getBoundingClientRect();
            const xPx = x * rect.width;
            const yPx = y * rect.height;
            const cx = centerX * rect.width;
            const cy = centerY * rect.height;
            const { audio: rAudio } = ringRadii(rect.width, rect.height);

            // Scale the direction into pixel units, so the fly-in matches the
            // cursor's actual motion on screen.
            let fx = dirX * rect.width;
            let fy = dirY * rect.height;
            const flen = Math.hypot(fx, fy) || 1;

            fx /= flen;
            fy /= flen;

            const angle = stickAngle(cx, cy, xPx, yPx, fx, fy, rAudio);

            // The fly-in is expressed in ring-local space (radius 0 at the
            // cursor tip -> rAudio on the ring at `angle`), never in absolute
            // pixels: the dial is pinned to the cursor, so the ball must ride
            // along when the pointer moves mid-flight.
            sparks.set(++pushSeq, {
                angle,
                state: "fly",
                t: 0,
                radius: 0,
                fade: 1,
            });

            sketch.push();
        }
    }

    if (options.windowPointer)
    {
        // Full-page mode: the dial follows the pointer over the whole page,
        // so the listeners live on the window (the overlay canvas has
        // pointer-events: none).
        const report = (event) => {
            const rect = canvas.getBoundingClientRect();
            const x = (event.clientX - rect.left) / Math.max(1, rect.width);
            const y = (event.clientY - rect.top) / Math.max(1, rect.height);

            handlePointer(
                Math.min(1, Math.max(0, x)),
                Math.min(1, Math.max(0, y)),
                event.type,
                event);
        };

        window.addEventListener("pointermove", report, { passive: true });
        window.addEventListener("pointerdown", report, { passive: true });
    }
    else
    {
        onPointer(canvas, (x, y, type) => handlePointer(x, y, type, null));
    }

    const draw = (ctx, dt, w, h) => {
        elapsed += dt;
        sketch.tick(dt);

        const aPhase = finite(sketch.aPhase(), 0);
        const uPhase = finite(sketch.uPhase(), 0);
        const wPhase = finite(sketch.wPhase(), 0);
        const uSample = finite(sketch.uSample(), uPhase);
        const wSample = finite(sketch.wSample(), wPhase);
        const hopU = sketch.hopU();
        const hopW = sketch.hopW();

        const cx = centerX * w;
        const cy = centerY * h;

        const { audio: rAudio, ui: rUi, worker: rWorker } = ringRadii(w, h);

        const aAngle = aPhase * TAU;
        const uAngle = uPhase * TAU;
        const wAngle = wPhase * TAU;
        const uSampleAngle = uSample * TAU;
        const wSampleAngle = wSample * TAU;

        // New hop events (each crossing delivers each id once; dedup by id).
        if (Number.isFinite(hopU) && hopU !== lastHopU)
        {
            lastHopU = hopU;
            hopUFired.add(Math.round(hopU));
        }

        if (Number.isFinite(hopW) && hopW !== lastHopW)
        {
            lastHopW = hopW;
            hopWFired.add(Math.round(hopW));
        }

        // Transparent, not filled: on the site this canvas will float over the
        // page, so only the dial itself may paint.
        ctx.clearRect(0, 0, w, h);

        ring(ctx, cx, cy, rAudio, soft.audio, 0);
        ring(ctx, cx, cy, rUi, soft.ui, 2.3);
        ring(ctx, cx, cy, rWorker, soft.worker, 4.1);

        // The latest crossings, inner → middle and middle → outer.
        fibre(ctx, cx, cy, rAudio, aAngle, rUi, uSampleAngle, soft.ui);
        fibre(ctx, cx, cy, rUi, uAngle, rWorker, wSampleAngle, soft.ui);

        trail(ctx, cx, cy, rAudio, aAngle, 0.85, soft.audio, 1.4);
        trail(ctx, cx, cy, rUi, uAngle, 0.70, soft.ui, 1.4);
        trail(ctx, cx, cy, rWorker, wAngle, 0.55, soft.worker, 1.6);

        head(ctx, cx, cy, rAudio, aAngle, soft.audio, 2.2);
        head(ctx, cx, cy, rUi, uAngle, soft.ui, 2.4);
        head(ctx, cx, cy, rWorker, wAngle, soft.worker, 2.6);

        // The staged hand-off balls. Each advance is gated by the crossing
        // event AND by the current ring's own hand touching the ball.
        for (const [id, spark] of sparks)
        {
            if (spark.state === "fly")
            {
                spark.t += dt / 0.35;

                const k = Math.min(1, spark.t);
                const eased = k * k * (3 - 2 * k);

                spark.radius = rAudio * eased;

                if (spark.t >= 1)
                {
                    spark.radius = rAudio;
                    spark.state = "waitAudio";
                }
            }
            else if (spark.state === "waitAudio")
            {
                if (angDist(aAngle, spark.angle) < 0.05 && hopUFired.has(id))
                {
                    spark.state = "toMid";
                    spark.t = 0;
                }
            }
            else if (spark.state === "toMid")
            {
                spark.t += dt / 0.25;
                spark.radius = rAudio + (rUi - rAudio) * Math.min(1, spark.t);

                if (spark.t >= 1)
                {
                    spark.radius = rUi;
                    spark.state = "waitMid";
                }
            }
            else if (spark.state === "waitMid")
            {
                if (angDist(uAngle, spark.angle) < 0.05 && hopWFired.has(id))
                {
                    spark.state = "toOut";
                    spark.t = 0;
                }
            }
            else if (spark.state === "toOut")
            {
                spark.t += dt / 0.25;
                spark.radius = rUi + (rWorker - rUi) * Math.min(1, spark.t);

                if (spark.t >= 1)
                {
                    spark.radius = rWorker;
                    spark.state = "waitOut";
                }
            }
            else if (spark.state === "waitOut")
            {
                if (angDist(wAngle, spark.angle) < 0.06)
                    spark.state = "exit";
            }
            else if (spark.state === "exit")
            {
                spark.radius += rWorker * 3 * dt;
                spark.fade -= dt / 0.9;

                if (spark.fade <= 0)
                {
                    sparks.delete(id);
                    continue;
                }
            }

            const exiting = spark.state === "exit";
            const [px, py] = pointAt(cx, cy, spark.radius, spark.angle);

            const ballColor = spark.state === "waitOut" || exiting ? soft.worker :
                spark.state === "waitMid" ? soft.ui : soft.audio;
            const alpha = spark.state === "fly" ? 0.85 : exiting ? Math.max(0, spark.fade) : 0.9;

            // A slow breathing pulse while it waits to be picked up: alive,
            // not mechanical.
            const waiting = !exiting && spark.state !== "fly";
            const size = 2 * (waiting ? 1 + 0.16 * Math.sin(elapsed * 5 + id) : 1);

            ctx.save();
            ctx.fillStyle = ballColor;
            ctx.globalAlpha = alpha;
            ctx.shadowBlur = 9;
            ctx.shadowColor = ballColor;
            ctx.beginPath();
            ctx.arc(px, py, size, 0, TAU);
            ctx.fill();
            ctx.restore();
        }
    };

    const runner = createRunner(canvas, draw);

    function reset()
    {
        sketch.reset();

        sparks.clear();
        hopUFired.clear();
        hopWFired.clear();
        pushSeq = 0;
        lastHopU = NaN;
        lastHopW = NaN;

        clearCanvas();
        draw(runner.ctx, 0, runner.width, runner.height);
        runner.start();
    }

    return {
        reset,

        // The launched balls in canvas (viewport) pixels, for the ambient
        // field layer. `strength` carries the ball's state so the field can
        // react harder while a value is flying and fade as it exits.
        balls()
        {
            const result = [];

            for (const spark of sparks.values())
            {
                const strength = spark.state === "exit"
                    ? Math.max(0, spark.fade)
                    : spark.state === "fly" ? 1.3 : 1;

                result.push({
                    x: centerX * runner.width + Math.cos(spark.angle) * spark.radius,
                    y: centerY * runner.height + Math.sin(spark.angle) * spark.radius,
                    strength,
                });
            }

            return result;
        },
    };
}
