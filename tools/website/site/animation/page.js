// Boot the landing-page animation: the ambient field on the fixed bottom
// canvas and the Cursor Clocks dial above it — both behind the page content.
// Progressive enhancement: if wasm or the shell is unavailable, the page is
// left exactly as it was.

import { mount as mountDial } from "./cursor-clocks.js";
import { mount as mountField } from "./page-field.js";
import { wasmSupported } from "./runner.js";

function loadShell()
{
    const src = new URL("./clockr_page.js", import.meta.url).href;

    return new Promise((resolve, reject) => {
        const script = document.createElement("script");

        script.src = src;
        script.onload = resolve;
        script.onerror = () => reject(new Error("could not load the wasm shell"));
        document.head.append(script);
    });
}

async function start()
{
    const fieldCanvas = document.getElementById("page-field");
    const dialCanvas = document.getElementById("page-dial");

    if (!fieldCanvas || !dialCanvas || !wasmSupported())
        return;

    await loadShell();

    if (typeof window.ClockrPage !== "function")
        throw new Error("wasm shell did not initialise");

    const lib = await window.ClockrPage();
    const coarse = window.matchMedia?.("(pointer: coarse)").matches ?? false;

    if (coarse)
    {
        // No cursor on touch: a dial would just sit where the last tap landed.
        // Let a finger stir the field directly instead.
        let touch = null;

        const track = (event) => {
            if (event.pointerType !== "touch")
                return;

            touch = (event.type === "pointerup" || event.type === "pointercancel")
                ? null
                : { x: event.clientX, y: event.clientY };
        };

        for (const type of ["pointerdown", "pointermove", "pointerup", "pointercancel"])
            window.addEventListener(type, track, { passive: true });

        mountField(fieldCanvas, {
            balls: () => touch ? [{ x: touch.x, y: touch.y, strength: 1.4 }] : [],
        });
    }
    else
    {
        const dial = mountDial(dialCanvas, lib);
        mountField(fieldCanvas, dial);
    }

    document.body.classList.add("animation-ready");
}

start().catch((error) => {
    // The animation is decoration: never surface a failure to the reader.
    console.warn("[clockr] page animation unavailable:", error.message ?? error);
});
