// Canvas plumbing for the landing-page animation. Renderers only compute and
// draw; this owns DPR, resize, visibility/offscreen pause, reduced motion and
// the requestAnimationFrame loop.
//
// The same contract as the lab's helpers, trimmed to what the page needs so
// the deployed bundle stays self-contained.

export function wasmSupported()
{
    return typeof WebAssembly === "object" && typeof WebAssembly.instantiate === "function";
}

export function createRunner(canvas, draw)
{
    const ctx = canvas.getContext("2d");

    if (!ctx)
        return { stop() {}, reduced: true };

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

    new ResizeObserver(() => {
        resize();

        if (reduced || !raf)
            redraw();
    }).observe(canvas);

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
