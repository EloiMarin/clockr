import { createReadStream, statSync } from "node:fs";
import { createServer } from "node:http";
import { extname, join, normalize, resolve } from "node:path";

const root = resolve(process.argv[2] ?? ".");
const port = Number(process.argv[3] ?? 8000);

const types = new Map([
    [".css", "text/css; charset=utf-8"],
    [".html", "text/html; charset=utf-8"],
    [".js", "text/javascript; charset=utf-8"],
    [".json", "application/json"],
    [".mjs", "text/javascript; charset=utf-8"],
    [".svg", "image/svg+xml"],
    [".wasm", "application/wasm"],
]);

const server = createServer((request, response) =>
{
    const pathname = decodeURIComponent(new URL(request.url, "http://local").pathname);
    const relative = normalize(pathname).replace(/^(\.\.(\/|\\|$))+/, "");
    let file = join(root, relative);

    try
    {
        if (statSync(file).isDirectory())
            file = join(file, "index.html");

        response.writeHead(200, {
            "Content-Type": types.get(extname(file)) ?? "application/octet-stream",
        });
        createReadStream(file).pipe(response);
    }
    catch
    {
        response.writeHead(404, { "Content-Type": "text/plain" });
        response.end("not found\n");
    }
});

server.listen(port, "127.0.0.1", () =>
{
    console.log(`jam UI: http://127.0.0.1:${port}/ (serving ${root})`);
});
