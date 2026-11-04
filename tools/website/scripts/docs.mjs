#!/usr/bin/env node
// Render the repository's documentation into _site/docs: every design doc
// under doc/ and GLOSSARY.md at the repo root, plus the media assets that sit
// beside them (slides, diagrams, images). The landing page links here; each
// page carries the landing page's styling, a genre breadcrumb, an "on this
// page" table of contents, and rewritten cross-links. Links that leave the
// documented set fall back to the configured source browser, so the reader
// always lands somewhere canonical.
//
//   node scripts/docs.mjs   (invoked by scripts/build.mjs)

import hljs from "highlight.js";
import { Marked } from "marked";
import { copyFile, mkdir, readdir, readFile, rm, writeFile } from "node:fs/promises";
import path from "node:path";

import { readConfig, rootDir, siteDir } from "./lib.mjs";

function log(message)
{
    console.log(`[website] ${message}`);
}

const GENRE_NAMES = new Map([
    ["learn", "Learn — walkthroughs"],
    ["explanation", "Explanation — design reasoning"],
    ["reference", "Reference — semantics and sources"],
    ["how-to", "How-to — operating the library"],
    ["archive", "Archive — talks and history"],
    ["glossary", "Glossary — the vocabulary"],
]);

// The docs' code fences are mostly unlabeled, so labeled fences use their
// language and unlabeled ones auto-detect within the languages the docs use.
// Every block is rendered with the Solarized Dark token classes (styled in
// site/style.css).
const AUTO_LANGUAGES = ["cpp", "bash", "json", "cmake", "yaml"];

function renderCode({ text, lang })
{
    const language = (lang ?? "").trim().toLowerCase();
    let value;
    let detected = language;

    if (language === "")
    {
        const auto = hljs.highlightAuto(text, AUTO_LANGUAGES);
        value = auto.value;
        detected = auto.language ?? "";
    }
    else if (hljs.getLanguage(language))
        value = hljs.highlight(text, { language }).value;
    else
    {
        value = hljs.highlight(text, { language: "plaintext" }).value;
        detected = "text";
    }

    const classes = detected ? `hljs language-${detected}` : "hljs";
    return `<pre><code class="${classes}">${value}</code></pre>`;
}

const marked = new Marked({ gfm: true, async: false });
marked.use({ renderer: { code: renderCode } });

// Walk doc/ depth-first and return every file accepted by `keep`.
async function walkTree(dir, keep)
{
    const entries = [];

    for (const entry of (await readdir(dir, { withFileTypes: true })).sort((a, b) => a.name.localeCompare(b.name)))
    {
        if (entry.name.startsWith("."))
            continue;

        const full = path.join(dir, entry.name);

        if (entry.isDirectory())
            entries.push(...(await walkTree(full, keep)));
        else if (keep(entry.name))
            entries.push(full);
    }

    return entries;
}

// Source path (repo-relative, posix) -> site path. The docs index is the
// rendered reading map (doc/README.md); everything else keeps its tree
// position under docs/ with .md swapped for .html.
async function collectSources()
{
    const pages = new Map();

    for (const file of await walkTree(path.join(rootDir, "doc"), (name) => name.endsWith(".md")))
        pages.set(pathToSource(file), null);

    pages.set("GLOSSARY.md", null);

    for (const source of [...pages.keys()])
    {
        if (source === "doc/README.md")
            pages.set(source, "docs/index.html");
        else if (source === "GLOSSARY.md")
            pages.set(source, "docs/glossary.html");
        else if (source.startsWith("doc/"))
            pages.set(source, `docs/${source.slice("doc/".length).replace(/\.md$/, ".html")}`);
        else
            throw new Error(`unmapped documentation source: ${source}`);
    }

    return pages;
}

// Media that travels with the published pages. Source trees that live under
// doc/ (the examples) are deliberately not copied.
const ASSET_EXTENSIONS = new Set([".gif", ".jpeg", ".jpg", ".pdf", ".png", ".svg", ".webp"]);

async function collectAssets()
{
    const assets = new Set();
    const keep = (name) => ASSET_EXTENSIONS.has(path.extname(name).toLowerCase());

    for (const file of await walkTree(path.join(rootDir, "doc"), keep))
        assets.add(pathToSource(file));

    return assets;
}

function pathToSource(file)
{
    return path.relative(rootDir, file).split(path.sep).join("/");
}

function assetToSite(source)
{
    if (!source.startsWith("doc/"))
        throw new Error(`unmapped documentation asset: ${source}`);

    return `docs/${source.slice("doc/".length)}`;
}

// Genre for the index grouping and the breadcrumb.
function genreOf(source)
{
    if (source === "GLOSSARY.md")
        return "glossary";
    return source.startsWith("doc/") ? source.split("/")[1] : source.split("/")[0];
}

// Resolve an in-text markdown reference to its published location. In-set
// files get their page; everything else (CONTRIBUTING.md, module READMEs,
// demo sources) falls back to a source-browser blob link.
function resolveHref(value, sourceDir, config, pages, assets)
{
    if (value.startsWith("mailto:") || value.startsWith("http://") || value.startsWith("https://"))
        return { href: value };

    const [rawTarget, fragment = ""] = value.split("#", 2);

    if (rawTarget === "")
        return { href: fragment ? `#${fragment}` : value };

    const target = path.posix.normalize(path.posix.join(sourceDir, rawTarget))
        .replace(/^\/+/, "")
        .replace(/\/+$/, "");

    if (target === "" && fragment)
        return { href: `#${fragment}` };

    if (pages.has(target))
        return { href: `/${pages.get(target)}${fragment ? `#${fragment}` : ""}` };

    if (assets.has(target))
        return { href: `/${assetToSite(target)}${fragment ? `#${fragment}` : ""}` };

    const remote = `${config.sourceBlobBase}/${target}${fragment ? `#${fragment}` : ""}`;
    return { href: remote };
}

function slugify(text)
{
    return decodeEntities(text.replace(/<[^>]*>/g, ""))
        .toLocaleLowerCase()
        .trim()
        .split(/[^\p{L}\p{N}]+/u)
        .filter(Boolean)
        .join("-");
}

function injectAnchors(html)
{
    const seen = new Map();

    return html.replace(/<(h[1-4])(\s[^>]*)?>([\s\S]*?)<\/\1>/g, (whole, tag, attrs, inner) => {
        if (attrs !== undefined || !inner.trim())
            return whole;

        let slug = slugify(inner) || "section";
        const count = seen.get(slug) ?? 0;
        seen.set(slug, count + 1);

        if (count > 0)
            slug = `${slug}-${count}`;

        return `<${tag} id="${slug}">${inner}</${tag}>`;
    });
}

function collectHeadings(html)
{
    const headings = [];

    for (const match of html.matchAll(/<h([23]) id="([^"]+)">([\s\S]*?)<\/h\1>/g))
        headings.push({ level: Number(match[1]), id: match[2], text: inlineText(match[3]) });

    return headings;
}

const ENTITIES = new Map([
    ["amp", "&"],
    ["lt", "<"],
    ["gt", ">"],
    ["quot", '"'],
    ["apos", "'"],
    ["nbsp", " "],
]);

function decodeEntities(text)
{
    return text
        .replace(/&#(\d+);/g, (_, n) => String.fromCodePoint(Number(n)))
        .replace(/&#x([0-9a-f]+);/gi, (_, hex) => String.fromCodePoint(parseInt(hex, 16)))
        .replace(/&(\w+);/g, (whole, name) => ENTITIES.get(name) ?? whole);
}

function inlineText(html)
{
    return decodeEntities(html.replace(/<[^>]*>/g, "")).trim();
}

// First prose line of a file, links stripped, as the index's one-liner.
function extractLede(md)
{
    for (const raw of md.split("\n"))
    {
        const line = raw.trim();

        if (line === "" || line.startsWith("#") || line.startsWith("|") || line === "---")
            continue;

        const text = line
            .replace(/\[([^\]]*)\]\([^)]*\)/g, "$1")
            .replace(/[*_`]*[*_`]/g, "")
            .replace(/\s+/g, " ")
            .trim();

        return text.length > 96 ? `${text.slice(0, 95).trimEnd()}…` : text;
    }

    return "";
}

function renderProse(md, config, pages, assets, sourcePath)
{
    const sourceDir = path.posix.dirname(`/${sourcePath}`);

    let html;
    try
    {
        html = marked.parse(md);
    }
    catch (error)
    {
        throw new Error(`markdown render failed for ${sourcePath}: ${error.message}`);
    }

    html = html.replace(/href="([^"]*)"/g, (whole, value) => {
        const decoded = value.replace(/&amp;/g, "&");

        if (decoded.startsWith("#"))
            return whole;

        const resolved = resolveHref(decoded, sourceDir, config, pages, assets);
        return `href="${resolved.href.replaceAll("&", "&amp;")}"`;
    });

    return html;
}

function escapeHtml(value)
{
    return String(value)
        .replaceAll("&", "&amp;")
        .replaceAll("<", "&lt;")
        .replaceAll(">", "&gt;")
        .replaceAll('"', "&quot;");
}

function pageShell(config, { title, genre, toc, body })
{
    const crumbHtml = genre
        ? `<nav class="crumbs"><a href="/docs/">Docs</a><span> / </span><span>${escapeHtml(GENRE_NAMES.get(genre) ?? genre)}</span></nav>`
        : '<nav class="crumbs"><a href="/docs/">Docs</a></nav>';

    const tocHtml = toc.length > 0
        ? `<aside class="pagetoc"><p class="toc-title">On this page</p>${
              toc.map((h) => `<a href="#${h.id}" class="toc-${h.level}">${escapeHtml(h.text)}</a>`).join("")
          }</aside>`
        : "";

    return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${escapeHtml(title)} — ${escapeHtml(config.name)} docs</title>
<meta name="description" content="${escapeHtml(config.description)}">
<link rel="icon" href="/clockr-mark.svg" type="image/svg+xml">
<link rel="stylesheet" href="/style.css">
</head>
<body>

<canvas id="page-field" class="page-canvas page-field" aria-hidden="true"></canvas>
<script type="module" src="/animation/field-only.js"></script>

<header class="top">
  <a class="brand" href="/"><img src="/clockr-logo.svg" alt="${escapeHtml(config.name)}" width="404" height="128"></a>
  <nav class="nav" aria-label="Page">
    <a href="/docs/">Index</a>
    <a class="nav-code" href="${escapeHtml(config.sourceUrl)}" target="_blank" rel="noopener">Code</a>
  </nav>
</header>

<main class="docspage">
${crumbHtml}
<div class="docsgrid${toc.length > 0 ? " withtoc" : ""}">
<article class="prose">
${body}
</article>
${tocHtml}
</div>
</main>

<footer class="site-foot">
  <p>Generated from the repository's <code>doc/</code> tree.
  <a class="footer-code" href="${escapeHtml(config.sourceUrl)}" target="_blank" rel="noopener">Source code</a>.</p>
</footer>

</body>
</html>
`;
}

function renderIndexExtra(pages, titles, ledes)
{
    const groups = new Map();

    for (const [source] of pages)
    {
        if (source === "doc/README.md")
            continue; // this page IS the index

        const genre = genreOf(source);
        groups.set(genre, (groups.get(genre) ?? []).concat(source));
    }

    const order = ["learn", "explanation", "reference", "how-to", "archive", "glossary"];
    const parts = [];

    for (const genre of order)
    {
        const sources = groups.get(genre);
        if (!sources?.length)
            continue;

        parts.push(`<h2 class="indexhead">${escapeHtml(GENRE_NAMES.get(genre) ?? genre)}</h2>`);
        parts.push('<ul class="docindex">');

        for (const source of sources)
        {
            const href = `/${pages.get(source)}`;
            const lede = ledes.get(source);
            parts.push(`<li><p class="docindex-title"><a href="${href}">${escapeHtml(titles.get(source))}</a></p>` +
                (lede ? `<p class="docindex-lede">${escapeHtml(lede)}</p>` : "") +
                "</li>");
        }

        parts.push("</ul>");
    }

    return parts.join("\n");
}

export async function renderDocs(config)
{
    const pages = await collectSources();
    const assets = await collectAssets();

    const docsDir = path.join(siteDir, "docs");
    await rm(docsDir, { recursive: true, force: true });
    await mkdir(docsDir, { recursive: true });

    for (const source of assets)
    {
        const site = assetToSite(source);
        await mkdir(path.join(siteDir, path.dirname(site)), { recursive: true });
        await copyFile(path.join(rootDir, ...source.split("/")), path.join(siteDir, site));
        log(`staged /${site}`);
    }

    const titles = new Map();
    const ledes = new Map();
    let indexHtml = null;

    for (const [source, site] of pages)
    {
        const md = await readFile(path.join(rootDir, ...source.split("/")), "utf8");
        const body = injectAnchors(renderProse(md, config, pages, assets, source));
        const title = inlineText((body.match(/<h1 id="[^"]+">([\s\S]*?)<\/h1>/) ?? [])[1] ?? "") || source;
        const toc = collectHeadings(body);

        titles.set(source, title);
        ledes.set(source, extractLede(md));

        if (site === "docs/index.html")
        {
            indexHtml = body;
            continue;
        }

        await mkdir(path.join(siteDir, path.dirname(site)), { recursive: true });
        await writeFile(path.join(siteDir, site), pageShell(config, { title, genre: genreOf(source), toc, body }));
        log(`rendered /${site}`);
    }

    const index = pageShell(config, {
        title: "Documentation",
        genre: null,
        toc: collectHeadings(indexHtml),
        body: `${indexHtml}\n<hr>${renderIndexExtra(pages, titles, ledes)}`,
    });

    await writeFile(path.join(docsDir, "index.html"), index);
    log(`staged ${pages.size} pages under /docs${assets.size > 0 ? ` (${assets.size} assets)` : ""}`);
}

if (import.meta.url === `file://${process.argv[1]}`)
    renderDocs(await readConfig()).catch((error) => {
        console.error(`[website] ${error.message}`);
        process.exitCode = 1;
    });
