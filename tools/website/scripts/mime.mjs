import path from "node:path";

const TYPES = new Map([
    [".css", "text/css; charset=utf-8"],
    [".gif", "image/gif"],
    [".html", "text/html; charset=utf-8"],
    [".ico", "image/x-icon"],
    [".jpeg", "image/jpeg"],
    [".jpg", "image/jpeg"],
    [".js", "text/javascript; charset=utf-8"],
    [".json", "application/json; charset=utf-8"],
    [".map", "application/json; charset=utf-8"],
    [".md", "text/markdown; charset=utf-8"],
    [".mjs", "text/javascript; charset=utf-8"],
    [".pdf", "application/pdf"],
    [".png", "image/png"],
    [".svg", "image/svg+xml"],
    [".txt", "text/plain; charset=utf-8"],
    [".wasm", "application/wasm"],
    [".webp", "image/webp"],
    [".woff", "font/woff"],
    [".woff2", "font/woff2"],
]);

export function contentType(key)
{
    const name = path.basename(key);

    if (name === "LICENSE" || name.endsWith("-LICENSE"))
        return "text/plain; charset=utf-8";

    return TYPES.get(path.extname(key).toLowerCase()) ?? "application/octet-stream";
}

export function cacheControl(key)
{
    const ext = path.extname(key).toLowerCase();

    // HTML and the debugger endpoint override must revalidate so a deploy is
    // visible immediately. Everything else is safe to cache briefly.
    if (ext === ".html" || key.endsWith("/config.js"))
        return "no-cache";

    return "public, max-age=300";
}
