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

const server = http.createServer((request, response) => {
    const file = path.join(dir, request.url.split("?")[0]);
    fs.readFile(file, (error, data) => {
        if (error) {
            response.writeHead(404);
            response.end();
            return;
        }

        response.writeHead(200, {
            "content-type": types[path.extname(file)] || "application/octet-stream",
        });
        response.end(data);
    });
});

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

let failed = false;

function check(what, ok, detail) {
    if (!ok) {
        failed = true;
        console.log(`FAIL ${what}${detail ? `: ${detail}` : ""}`);
    } else {
        console.log(`ok ${what}`);
    }
}

function parse(text) {
    try {
        return JSON.parse(text);
    } catch {
        return null;
    }
}

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
        postMessage: (message) => posted.push(message),
        location: { href: base + "/worker.js" },
        onmessage: null,
    };
    sandbox.self = sandbox;
    sandbox.globalThis = sandbox;

    const context = vm.createContext(sandbox);
    sandbox.importScripts = (file) =>
        vm.runInContext(
            fs.readFileSync(path.join(dir, file), "utf8"),
            context,
            { filename: file }
        );

    try {
        vm.runInContext(
            fs.readFileSync(path.join(dir, "worker.js"), "utf8"),
            context,
            { filename: "worker.js" }
        );
    } catch (error) {
        console.error(`worker script failed to load: ${error}`);
        process.exit(1);
    }

    for (let i = 0; i < 100 && !posted.some((message) => message.type === "ready"); i += 1)
        await sleep(50);

    check("worker bootstrap posts ready", posted.some((message) => message.type === "ready"));
    check("worker did not fail to load the module", !posted.some((message) => message.type === "failed"));

    let nextId = 0;

    function rpc(method, ...args) {
        const id = ++nextId;
        const before = posted.length;
        context.onmessage({ data: { type: "call", id, method, args } });
        const reply = posted.slice(before).find((message) => message.type === "reply" && message.id === id);

        if (!reply)
            throw new Error(`no reply for ${method}`);
        if (!reply.ok)
            throw new Error(`${method}: ${reply.error}`);

        return reply.result;
    }

    try {
        const score = parse(rpc("defaultScoreJson"));
        check("worker returns the default score", score && score.patterns.length === 16);

        const init = parse(rpc("init", 120));
        check("worker init succeeds", init && init.ok === true);

        const intents = parse(rpc("setIntents", 1, 120, 0, 4, 1, 0.8));
        check("worker intents apply", intents && intents.intents.playing === true);

        const stepped = parse(rpc("step", 4));
        check("worker steps audio blocks", stepped && stepped.ticks.length === 4);

        const exported = parse(rpc("exportScenarioJson"));
        check("worker exports a scenario", exported && exported.version === "clockr.replay.v1");

        const branch = parse(rpc(
            "replayBranchJson",
            JSON.stringify({ patch: { ops: [{ op: "transpose", a: 0, d: 5 }] } })
        ));
        check("worker replays a branch", branch && branch.ok === true && branch.candidateTicks.length > 0);
    } catch (error) {
        check("worker RPC", false, error.message);
    }

    server.close();
    process.exit(failed ? 1 : 0);
});
