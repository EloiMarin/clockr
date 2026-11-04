// Node smoke test for the in-browser debugger transport: runs the real worker
// bootstrap in a vm with a BroadcastChannel, sends the debugger's `hello`, and
// checks the graph/event stream plus pause/resume. The module directory arrives
// as argv[2]; no DOM and no browser, the same wasm bytes and worker script.
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

async function waitFor(predicate, timeoutMs = 2500) {
    const deadline = Date.now() + timeoutMs;

    while (Date.now() < deadline) {
        if (predicate())
            return true;
        await sleep(25);
    }

    return predicate();
}

let failed = false;

const check = (what, ok) => {
    if (!ok)
        failed = true;
    console.log(`${ok ? "ok" : "FAIL"} ${what}`);
};

server.listen(0, "127.0.0.1", async () => {
    const base = `http://127.0.0.1:${server.address().port}`;
    const posted = [];
    const channel = new BroadcastChannel("clockr.debug.local");
    const frames = [];

    channel.onmessage = (event) => frames.push(event.data);

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
        BroadcastChannel,
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

    check(
        "ready posted from the worker bootstrap",
        await waitFor(() => posted.some((message) => message.type === "ready")));

    const send = (cmd) => ctx.onmessage({ data: cmd });

    send({ type: "set", port: "playing", value: true });
    send({ type: "stepUi" });
    send({ type: "autoAudio", on: true });
    send({ type: "autoUi", on: true });

    channel.postMessage({ type: "hello" });

    check(
        "debugger handshake answered",
        await waitFor(() => frames.some((message) => message.type === "welcome")));

    const graph = frames.find((message) => message.type === "graph")?.graph;
    check(
        "graph description broadcast",
        graph?.name === "dawmulator"
            && graph.nodes.length > 0
            && graph.types.some((type) => type.name === "daw.loop_region"));
    check(
        "example list broadcast",
        frames.some((message) => message.type === "examples"
            && message.examples.active === "dawmulator"));

    check(
        "event batches broadcast",
        await waitFor(() => frames.some((message) => message.type === "batch"
            && message.batch.events.some((event) => event.kind === "node_tick_end"))));

    channel.postMessage({ type: "pause" });
    await sleep(120);

    const pausedAt = frames.filter((message) => message.type === "batch").length;
    await sleep(180);
    check(
        "pause stops the trace stream",
        frames.filter((message) => message.type === "batch").length === pausedAt);

    channel.postMessage({ type: "resume" });
    check(
        "resume restarts the trace stream",
        await waitFor(() => frames.filter((message) => message.type === "batch").length > pausedAt));

    send({ type: "autoAudio", on: false });
    send({ type: "autoUi", on: false });

    channel.close();
    server.close();
    process.exit(failed ? 1 : 0);
});
