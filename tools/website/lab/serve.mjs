#!/usr/bin/env node
// Tiny static server for the throwaway lab. Source files are served directly
// (so JS/CSS edits need only a refresh); the wasm module comes from _build/.

import { readFile, stat } from "node:fs/promises";
import { createServer } from "node:http";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { contentType } from "../scripts/mime.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const buildRoot = path.join(here, "_build");
const args = process.argv.slice(2);
const portIndex = args.indexOf("--port");
const port = Number(portIndex >= 0 ? args[portIndex + 1] : process.env.PORT || 4180);

async function fileInfo(file)
{
    try
    {
        const info = await stat(file);
        return info.isFile() ? info : null;
    }
    catch
    {
        return null;
    }
}

createServer(async (request, response) => {
    const url = new URL(request.url ?? "/", `http://${request.headers.host ?? "localhost"}`);
    let pathname = decodeURIComponent(url.pathname);

    if (pathname.endsWith("/"))
        pathname += "index.html";

    const generated = pathname === "/lab_shell.js" || pathname === "/lab_shell.wasm";
    const root = generated ? buildRoot : here;
    const file = path.resolve(root, `.${pathname}`);

    if (file !== root && !file.startsWith(root + path.sep))
    {
        response.writeHead(403).end("forbidden");
        return;
    }

    let info = await fileInfo(file);
    let target = file;

    if (!info && !generated)
    {
        target = path.join(here, "index.html");
        info = await fileInfo(target);
    }

    if (!info)
    {
        response.writeHead(404).end("run ./build.sh first");
        return;
    }

    const body = await readFile(target);
    response.writeHead(200, {
        "Content-Type": contentType(target),
        "Cache-Control": "no-cache",
        "Content-Length": body.length,
    });
    response.end(body);
}).listen(port, "127.0.0.1", () => {
    console.log(`[lab] http://127.0.0.1:${port}/`);
});
