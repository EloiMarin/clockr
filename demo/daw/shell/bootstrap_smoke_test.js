// Node smoke test for the worker bootstrap itself: the exact path a browser
// takes — importScripts pulls the embind module into the worker's global
// scope, the module fetches daw_shell.wasm relative to the worker's own
// location, embind registers, and the protocol answers. Runs engine_worker.js
// inside a vm context faking a classic worker (importScripts, self.location,
// postMessage, WorkerGlobalScope) against a throwaway static file server on
// the staged build dir (argv[2]) — no DOM, no browser, same wasm bytes.
const http = require("http");
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const dir = process.argv[2];

const types = {
    ".html": "text/html",
    ".js": "text/javascript",
    ".css": "text/css",
    ".wasm": "application/wasm",
};

const server = http.createServer((req, res) => {
    const file = path.join(dir, req.url.split("?")[0]);
    fs.readFile(file, (err, data) => {
        if (err) {
            res.writeHead(404);
            res.end();
            return;
        }
        res.writeHead(200, { "content-type": types[path.extname(file)] || "application/octet-stream" });
        res.end(data);
    });
});

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

let failed = false;

const check = (what, ok) => {
    if (!ok)
        failed = true;
    console.log(`${ok ? "ok" : "FAIL"} ${what}`);
};

server.listen(0, "127.0.0.1", async () => {
    const base = `http://127.0.0.1:${server.address().port}`;
    const posted = [];

    const sandbox = {
        console,
        setTimeout,
        clearTimeout,
        setInterval,
        clearInterval,
        performance,
        TextDecoder,
        TextEncoder,
        URL,
        WebAssembly,
        WorkerGlobalScope: function () {},
        fetch: (url) => fetch(new URL(url, base + "/").href),
        postMessage: (msg) => posted.push(msg),
        location: { href: base + "/engine_worker.js" },
        onmessage: null,
    };
    sandbox.self = sandbox;
    sandbox.globalThis = sandbox;

    const ctx = vm.createContext(sandbox);
    sandbox.importScripts = (file) =>
        vm.runInContext(fs.readFileSync(path.join(dir, file), "utf8"), ctx, { filename: file });

    try {
        vm.runInContext(fs.readFileSync(path.join(dir, "engine_worker.js"), "utf8"), ctx, {
            filename: "engine_worker.js",
        });
    } catch (err) {
        console.error(`worker script failed to load: ${err}`);
        process.exit(1);
    }

    // The module fetch + instantiate is async; wait for the ready event.
    for (let i = 0; i < 100 && !posted.some((m) => m.type === "ready"); i += 1)
        await sleep(50);

    check("ready posted from the importScripts bootstrap", posted.some((m) => m.type === "ready"));

    const send = (cmd) => ctx.onmessage({ data: cmd });

    // §6.1's first block through the fake worker.
    send({ type: "set", port: "playing", value: true });
    send({ type: "set", port: "bpm", value: 120.0 });
    send({ type: "loop", start: 0.0, end: 4.0, on: true });
    send({ type: "set", port: "master", value: 0.8 });
    send({ type: "stepUi" });
    send({ type: "blockParams", n: 24000, rate: 48000.0 });
    send({ type: "stepAudio" });

    await sleep(200);

    const audio = posted.filter((m) => m.type === "audio");
    const ui = posted.filter((m) => m.type === "ui");
    check("audio tick through the worker", audio.length === 1);
    check("block 0 playhead", audio.length === 1 && audio[0].outputs.playhead === 1);
    check("ui tick through the worker", ui.length >= 1);

    server.close();
    process.exit(failed ? 1 : 0);
});
