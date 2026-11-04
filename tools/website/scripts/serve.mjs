#!/usr/bin/env node
// A tiny static server for tools/website/_site. Useful for checking the
// staged site (including the wasm demo) locally.
//
//   node scripts/serve.mjs [--port 4173]

import { createReadStream } from "node:fs";
import { stat } from "node:fs/promises";
import http from "node:http";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { contentType } from "./mime.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const siteDir = path.resolve(here, "..", "_site");
const args = process.argv.slice(2);
const portIndex = args.indexOf("--port");
const port = Number(portIndex >= 0 ? args[portIndex + 1] : process.env.PORT || 4173);

function resolveRequest(pathname)
{
    const requested = path.resolve(siteDir, `.${pathname}`);

    if (requested !== siteDir && !requested.startsWith(siteDir + path.sep))
        return null;

    return requested;
}

async function fileExists(file)
{
    try
    {
        return (await stat(file)).isFile();
    }
    catch
    {
        return false;
    }
}

const server = http.createServer(async (request, response) => {
    const url = new URL(request.url ?? "/", `http://${request.headers.host ?? "localhost"}`);
    let pathname = decodeURIComponent(url.pathname);

    if (!pathname.endsWith("/") && !path.extname(pathname))
    {
        const candidate = resolveRequest(pathname);

        if (candidate && (await fileExists(path.join(candidate, "index.html"))))
        {
            response.writeHead(301, { Location: `${pathname}/` });
            response.end();
            return;
        }
    }

    if (pathname.endsWith("/"))
        pathname += "index.html";

    const file = resolveRequest(pathname);
    let status = 200;

    if (!file || !(await fileExists(file)))
    {
        status = 404;
        pathname = "/404.html";
    }

    const target = status === 404 ? path.join(siteDir, "404.html") : file;

    response.writeHead(status, {
        "Content-Type": contentType(pathname),
        "Cache-Control": "no-cache",
        "Content-Length": (await stat(target)).size,
    });

    if (request.method === "HEAD")
    {
        response.end();
        return;
    }

    createReadStream(target).pipe(response);
});

server.listen(port, "127.0.0.1", () => {
    console.log(`[website] serving ${siteDir} at http://127.0.0.1:${port}/`);
});
