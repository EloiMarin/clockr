// Shared canvas plumbing for the throwaway animation lab. Renderers only
// compute and draw; this file owns DPR, resize, visibility, reduced motion and
// the requestAnimationFrame loop.

export const palette = {
    bg: "#0b0d10",
    panel: "#141820",
    ink: "#d8dce5",
    dim: "#8b93a4",
    amber: "#ffb454",
    teal: "#3fa08c",
    violet: "#9a7ff0",
    green: "#79c267",
    red: "#e0645c",
    blue: "#7d97b4",
};

export function wasmSupported()
{
    return typeof WebAssembly === "object" && typeof WebAssembly.instantiate === "function";
}

export function clear(ctx, width, height, color = palette.bg)
{
    ctx.save();
    ctx.globalCompositeOperation = "source-over";
    ctx.globalAlpha = 1;
    ctx.fillStyle = color;
    ctx.fillRect(0, 0, width, height);
    ctx.restore();
}

export function createRunner(canvas, draw)
{
    const ctx = canvas.getContext("2d");

    if (!ctx)
        return { stop() {}, reset() {}, reduced: true };

    const reduced = window.matchMedia?.("(prefers-reduced-motion: reduce)").matches ?? false;
    const dprCap = 2;

    let width = 1;
    let height = 1;
    let raf = 0;
    let last = 0;
    let visible = true;
    let stopped = false;

    function resize()
    {
        const rect = canvas.getBoundingClientRect();
        const dpr = Math.min(window.devicePixelRatio || 1, dprCap);
        width = Math.max(1, Math.round(rect.width));
        height = Math.max(1, Math.round(rect.height));
        canvas.width = Math.round(width * dpr);
        canvas.height = Math.round(height * dpr);
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    }

    function frame(now)
    {
        raf = 0;

        if (stopped || !visible || document.hidden)
            return;

        const dt = last === 0 ? 1 / 60 : Math.min((now - last) / 1000, 0.05);
        last = now;
        draw(ctx, dt, width, height);
        raf = requestAnimationFrame(frame);
    }

    function start()
    {
        if (reduced || stopped || raf || !visible || document.hidden)
            return;

        last = 0;
        raf = requestAnimationFrame(frame);
    }

    function stop()
    {
        stopped = true;

        if (raf)
            cancelAnimationFrame(raf);

        raf = 0;
    }

    function redraw()
    {
        draw(ctx, 1 / 60, width, height);
    }

    const resizeObserver = new ResizeObserver(() => {
        resize();

        if (reduced || !raf)
            redraw();
    });
    resizeObserver.observe(canvas);

    if ("IntersectionObserver" in window)
    {
        new IntersectionObserver(
            (entries) => {
                visible = entries[0]?.isIntersecting ?? true;

                if (visible)
                    start();
            },
            { rootMargin: "120px" },
        ).observe(canvas);
    }

    document.addEventListener("visibilitychange", () => {
        if (document.hidden)
        {
            if (raf)
                cancelAnimationFrame(raf);
            raf = 0;
        }
        else
        {
            start();
        }
    });

    resize();
    redraw();
    start();

    return {
        ctx,
        get width() { return width; },
        get height() { return height; },
        reduced,
        resize,
        start,
        stop,
    };
}

export function onPointer(canvas, handler)
{
    function report(event)
    {
        const rect = canvas.getBoundingClientRect();
        const x = (event.clientX - rect.left) / Math.max(1, rect.width);
        const y = (event.clientY - rect.top) / Math.max(1, rect.height);
        handler(Math.min(1, Math.max(0, x)), Math.min(1, Math.max(0, y)), event.type);
    }

    canvas.addEventListener("pointermove", report);
    canvas.addEventListener("pointerdown", report);
    canvas.addEventListener("pointerleave", () => handler(0.5, 0.5, "leave"));
}

export function rng(seed)
{
    let state = seed >>> 0;

    return function next() {
        state += 0x6d2b79f5;
        let value = Math.imul(state ^ (state >>> 15), 1 | state);
        value ^= value + Math.imul(value ^ (value >>> 7), 61 | value);
        return ((value ^ (value >>> 14)) >>> 0) / 4294967296;
    };
}
