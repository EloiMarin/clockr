// Throwaway animation lab: builds a card per sketch, loads the shared wasm
// module, and mounts each renderer. A renderer failure only marks its own
// card, so one broken sketch cannot take down the page.

import { wasmSupported } from "./lab-common.js";

const ANIMATIONS = [
    {
        id: "signal-bloom",
        title: "Signal Bloom",
        tags: "scan · delay · latest",
        blurb: "A harmonograph whose phase and feedback live in the wasm recipe. Move the pointer to bend it.",
    },
    {
        id: "clock-loom",
        title: "Clock Loom",
        tags: "audio · ui · worker",
        blurb: "Three lanes tick at different rates; threads cross through latest and queued links.",
    },
    {
        id: "queued-ripples",
        title: "Queued Ripples",
        tags: "queued · events",
        blurb: "Click to queue events. Rings expand as the consumer drains them; none are dropped.",
    },
    {
        id: "shape-of-hold",
        title: "The Shape of a Hold",
        tags: "scan · feedback",
        blurb: "A held spring follows the pointer while a faint ghost shows the raw input.",
    },
    {
        id: "three-clocks-one-brush",
        title: "Three Clocks, One Brush",
        tags: "regions · resampling",
        blurb: "Three brushes paint at their own clock rates, sampling each other through links.",
    },
    {
        id: "deterministic-replay",
        title: "Deterministic Replay",
        tags: "determinism · reset",
        blurb: "The same recipe and seed draw exactly the same figure after a reset.",
    },
    {
        id: "flow-field",
        title: "Flow Field",
        tags: "latest · smoothing",
        blurb: "Wasm computes a slowly turning field; a small particle cloud renders it.",
    },
    {
        id: "fling",
        title: "Fling",
        tags: "scan · spring · user struct",
        blurb: "Press to grab, release to throw: the held spring's velocity becomes the fling, the walls bounce it, the slack spring reels it back.",
    },
    {
        id: "ink-echoes",
        title: "Ink Echoes",
        tags: "delay · fan-out",
        blurb: "Draw with the pointer; four parallel delay lanes repaint the same gesture at increasing time depths.",
    },
    {
        id: "knock-stamps",
        title: "Knock & Stamp",
        tags: "event · stepper",
        blurb: "Click to pulse the orbit: the knock winds it up, and the exact position on the firing step is stamped as a fading ghost.",
    },
    {
        id: "stirred-field",
        title: "Stirred Field",
        tags: "regions · spirit · loop · audio",
        blurb: "Three clocks: the ball obeys your hand, the field pushes back on its own cadence, and the ball's stir and the field's drift feed each other above and below. Underneath, a spirit tide no pointer can touch — restlessness, warmth and calm drift over minutes; leave it alone and it explores, comes back changed, and hums if you let it.",
    },
];

function card(animation)
{
    const article = document.createElement("article");
    article.className = "lab-card";
    article.dataset.anim = animation.id;
    article.innerHTML = `
        <header class="lab-card-head">
            <div>
                <h2>${animation.title}</h2>
                <span class="lab-tag">${animation.tags}</span>
            </div>
            <button class="lab-restart" type="button">restart</button>
        </header>
        <div class="lab-canvas-wrap"><canvas id="anim-${animation.id}" aria-label="${animation.title}"></canvas></div>
        <p class="lab-blurb">${animation.blurb}</p>
        <p class="lab-error" hidden></p>
    `;
    return article;
}

async function main()
{
    const grid = document.getElementById("lab-grid");
    const status = document.getElementById("lab-status");

    for (const animation of ANIMATIONS)
        grid.append(card(animation));

    if (!wasmSupported())
    {
        status.textContent = "WebAssembly is unavailable; the sketches are disabled.";
        document.body.classList.add("no-wasm");
        return;
    }

    if (typeof window.LabShell !== "function")
    {
        status.textContent = "lab_shell.js did not load; build the lab first.";
        document.body.classList.add("no-wasm");
        return;
    }

    let lib;

    try
    {
        lib = await window.LabShell();
    }
    catch (error)
    {
        status.textContent = `wasm module failed: ${error.message ?? error}`;
        document.body.classList.add("no-wasm");
        return;
    }

    const hasPageLayers = Boolean(
        document.getElementById("page-field") && document.getElementById("page-dial"));

    status.textContent = `wasm ready · ${ANIMATIONS.length} sketches`
        + (hasPageLayers ? " · cursor field live" : "");

    // Page-level arrangement (as planned for the landing page): the ambient
    // field on the bottom canvas and the Cursor Clocks dial on the overlay.
    // The 404 gallery omits the layers, so mount only when they are present.
    if (hasPageLayers)
    {
        try
        {
            const { mountPage } = await import("./web/page-experience.js");
            mountPage(lib);
        }
        catch (error)
        {
            status.textContent = `page experience failed: ${error.message ?? error}`;
            console.error("page experience", error);
        }
    }

    for (const animation of ANIMATIONS)
    {
        const article = grid.querySelector(`[data-anim="${animation.id}"]`);
        const canvas = document.getElementById(`anim-${animation.id}`);

        try
        {
            const renderer = await import(`./web/${animation.id}.js`);
            const controller = renderer.mount(canvas, lib);

            article.querySelector(".lab-restart").addEventListener("click", () => {
                controller?.reset?.();
            });
        }
        catch (error)
        {
            article.classList.add("failed");
            const message = article.querySelector(".lab-error");
            message.hidden = false;
            message.textContent = String(error.message ?? error);
            console.error(animation.id, error);
        }
    }
}

main().catch((error) => {
    const status = document.getElementById("lab-status");
    status.textContent = String(error.message ?? error);
    console.error(error);
});
