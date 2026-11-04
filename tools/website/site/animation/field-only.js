// Field-only layer for the documentation pages: the ambient particles, no
// cursor dial and no wasm. The field drifts on its own (the reading pages
// should not invite clicking), so this stub never reports any balls.

import { mount } from "./page-field.js";

const canvas = document.getElementById("page-field");

if (canvas)
{
    mount(canvas, { balls: () => [] });
    document.body.classList.add("animation-ready");
}
