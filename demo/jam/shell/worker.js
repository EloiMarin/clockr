/*global importScripts, postMessage, onmessage, self, JamShell */

(function (global) {
    "use strict";

    importScripts("jam_shell.js");

    const allowed = new Set([
        "createScore",
        "defaultScoreJson",
        "init",
        "setIntents",
        "setScoreJson",
        "setPatchJson",
        "setStep",
        "step",
        "getStateJson",
        "exportScenarioJson",
        "importScenarioJson",
        "replayBranchJson",
        "replayBlockJson",
        "compareWithOriginalJson",
        "reset",
    ]);

    let shell = null;
    const waiting = [];

    function reply(id, ok, result, error) {
        postMessage({ type: "reply", id, ok, result, error });
    }

    function dispatch(message) {
        if (!allowed.has(message.method)) {
            reply(message.id, false, null, `unknown shell method '${message.method}'`);
            return;
        }

        try {
            const args = Array.isArray(message.args) ? message.args : [];
            const result = shell[message.method](...args);
            reply(message.id, true, result, null);
        } catch (error) {
            reply(
                message.id,
                false,
                null,
                String(error && error.message ? error.message : error)
            );
        }
    }

    JamShell().then((module) => {
        shell = new module.JamShell();
        postMessage({ type: "ready" });

        for (const message of waiting)
            dispatch(message);

        waiting.length = 0;
    }).catch((error) => {
        postMessage({ type: "failed", error: String(error) });
    });

    global.onmessage = (event) => {
        const message = event.data;

        if (!message || message.type !== "call")
            return;

        if (shell === null)
            waiting.push(message);
        else
            dispatch(message);
    };
})(typeof self !== "undefined" ? self : globalThis);
