// The lab's page-level arrangement, mirroring the shape the landing page will
// take: the ambient field on the fixed bottom canvas (behind all content) and
// the Cursor Clocks dial on the fixed top overlay. Clicks anywhere on the page
// launch a hop ball, and the field reacts to it.

import { mount as mountDial } from "./cursor-clocks.js";
import { mount as mountField } from "./page-field.js";

export function mountPage(lib)
{
    const fieldCanvas = document.getElementById("page-field");
    const dialCanvas = document.getElementById("page-dial");

    if (!fieldCanvas || !dialCanvas)
        throw new Error("page canvases are missing from the document");

    const dial = mountDial(dialCanvas, lib, { windowPointer: true });
    const field = mountField(fieldCanvas, dial);

    return {
        reset()
        {
            dial.reset();
            field.reset();
        },
    };
}
