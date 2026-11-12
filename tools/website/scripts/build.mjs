#!/usr/bin/env node
// Stage the deployable site into tools/website/_site: the landing page, the
// demo page (DAWmulator first, debugger on a switch), the documentation pages,
// the 404 page, the license and the landing-page animation (wasm shell +
// renderers, when emscripten is available). When no in-browser pair is staged,
// an externally hosted live pair can fill in through DEBUGGER_URL and DAW_URL.
//
//   node scripts/build.mjs
//   DEBUGGER_URL=https://debugger.example.com DAW_URL=https://daw.example.com npm run build

import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { copyFile, cp, mkdir, readdir, readFile, rm, writeFile } from "node:fs/promises";
import path from "node:path";

import { readConfig, rootDir, run, siteDir, websiteDir } from "./lib.mjs";
import { renderDocs } from "./docs.mjs";

const ANIMATION_MARKUP = `<canvas id="page-field" class="page-canvas page-field" aria-hidden="true"></canvas>
<canvas id="page-dial" class="page-canvas page-dial" aria-hidden="true"></canvas>
<script type="module" src="./animation/page.js"></script>`;

const LAB_MARKUP = `  <section class="lab-showcase" aria-label="Widget outtakes">
    <p id="lab-status" class="lab-status">loading wasm…</p>
    <div id="lab-grid" class="lab-grid"></div>
  </section>
  <script src="/lab/lab_shell.js"></script>
  <script type="module" src="/lab/lab.js"></script>`;

const LAB_FALLBACK = `  <section class="notfound-hero">
    <p class="lede">The lab prototypes are not part of this build.</p>
    <p><a class="button ghost" href="/">Back to the home page</a></p>
  </section>`;

function log(message)
{
    console.log(`[website] ${message}`);
}

function escapeHtml(value)
{
    return String(value)
        .replaceAll("&", "&amp;")
        .replaceAll("<", "&lt;")
        .replaceAll(">", "&gt;")
        .replaceAll('"', "&quot;")
        .replaceAll("'", "&#39;");
}

function demoPane({
    title,
    description,
    liveUrl,
    localUrl,
    envName,
    iframeTitle,
    paneClass = "",
    iframeId = "",
    defer = false,
})
{
    const paneClasses = `demo-pane${paneClass ? ` ${paneClass}` : ""}`;

    if (!liveUrl && !localUrl)
    {
        return `<article class="${paneClasses}">
  <div class="demo-pane-head">
    <div>
      <h2>${title}</h2>
      <p>${description}</p>
    </div>
  </div>
  <div class="placeholder">
    <p>${title} is not part of this build.</p>
    <p>Deploy the optional live stack (or set <code>${envName}</code> when
    building), or build the in-browser runtime with Emscripten, to show it here.</p>
  </div>
</article>`;
    }

    const live = liveUrl ? escapeHtml(liveUrl) : "";
    const local = localUrl ? escapeHtml(localUrl) : "";
    const preferred = local || live;
    const source = defer ? `data-src="${preferred}"` : `src="${preferred}"`;
    const id = iframeId ? ` id="${iframeId}"` : "";

    return `<article class="${paneClasses}">
  <div class="demo-pane-head">
    <div>
      <h2>${title}</h2>
      <p>${description}</p>
    </div>
    <a class="demo-open" href="${preferred}" target="_blank" rel="noopener">Full screen ↗</a>
  </div>
  <div class="demo-frame">
    <iframe${id} ${source} title="${iframeTitle}" loading="eager"></iframe>
  </div>
</article>`;
}

function dawPane(liveUrl, localUrl)
{
    return demoPane({
        title: "DAWmulator",
        description: "A transport, scheduler and voice on real ui and audio clocks — in this browser.",
        liveUrl,
        localUrl,
        envName: "DAW_URL",
        iframeTitle: "DAWmulator",
    });
}

function debuggerPane(liveUrl, localUrl)
{
    return demoPane({
        title: "Debugger",
        description: "Graph, timeline and node cursor over the same program's trace.",
        liveUrl,
        localUrl,
        envName: "DEBUGGER_URL",
        iframeTitle: "DAWmulator debugger",
        paneClass: "demo-pane-debugger",
        iframeId: "debugger-frame",
        defer: true,
    });
}

function mirrorLink(config)
{
    if (!config.mirrorUrl)
        return "";

    const safe = escapeHtml(config.mirrorUrl);
    return `<span class="dim">·</span> <a href="${safe}" target="_blank" rel="noopener">Mirror</a>`;
}

async function stageLanding(config, animation)
{
    const source = path.join(websiteDir, "site");
    const template = await readFile(path.join(source, "index.template.html"), "utf8");

    const values = {
        name: escapeHtml(config.name),
        headline: escapeHtml(config.headline),
        description: escapeHtml(config.description),
        sourceName: escapeHtml(config.sourceName),
        sourceUrl: escapeHtml(config.sourceUrl),
        mirrorLink: mirrorLink(config),
        animation,
    };

    let html = template;

    for (const [key, value] of Object.entries(values))
        html = html.replaceAll(`{{${key}}}`, value);

    await writeFile(path.join(siteDir, "index.html"), html);
    await copyFile(path.join(source, "style.css"), path.join(siteDir, "style.css"));

    for (const asset of ["clockr-mark.svg", "clockr-logo.svg"])
        await copyFile(path.join(source, asset), path.join(siteDir, asset));

    // Self-hosted latin subsets for the display/body/mono families.
    const fontsSource = path.join(source, "fonts");
    const fontsDest = path.join(siteDir, "fonts");

    await mkdir(fontsDest, { recursive: true });

    for (const entry of await readdir(fontsSource, { withFileTypes: true }))
    {
        if (entry.isFile())
            await copyFile(path.join(fontsSource, entry.name), path.join(fontsDest, entry.name));
    }

    // Keep an extension on the file: the static host treats extensionless
    // paths as directory URLs.
    await copyFile(path.join(rootDir, "LICENSE.md"), path.join(siteDir, "LICENSE.txt"));

    log("staged landing page");
}

async function stageDemo(config, local)
{
    const source = path.join(websiteDir, "site");
    const template = await readFile(path.join(source, "demo.template.html"), "utf8");
    const dawUrl = process.env.DAW_URL ?? "";
    const debuggerUrl = process.env.DEBUGGER_URL ?? "";
    const localDawUrl = local.daw ? "/daw/" : "";
    const localDebuggerUrl = local.debugger ? "/debugger/?local=1" : "";

    const values = {
        name: escapeHtml(config.name),
        sourceName: escapeHtml(config.sourceName),
        sourceUrl: escapeHtml(config.sourceUrl),
        dawPane: dawPane(dawUrl, localDawUrl),
        debuggerPane: debuggerPane(debuggerUrl, localDebuggerUrl),
    };

    let html = template;

    for (const [key, value] of Object.entries(values))
        html = html.replaceAll(`{{${key}}}`, value);

    const demoDir = path.join(siteDir, "demo");
    await mkdir(demoDir, { recursive: true });
    await writeFile(path.join(demoDir, "index.html"), html);

    const embedded = [
        localDawUrl ? "in-browser DAWmulator" : "",
        localDebuggerUrl ? "in-browser debugger" : "",
        dawUrl ? "live DAWmulator" : "",
        debuggerUrl ? "live debugger" : "",
    ].filter(Boolean);

    log(`staged demo page${embedded.length ? ` (${embedded.join(" + ")})` : " (no demos)"}`);
}

async function stageDocs(config)
{
    await renderDocs(config);
    log("staged documentation pages under /docs");
}

async function stageAnimation()
{
    const destination = path.join(siteDir, "animation");
    const source = path.join(websiteDir, "site", "animation");

    await mkdir(destination, { recursive: true });

    // The renderers are plain JS and also power the docs field, which needs
    // no wasm, so they are always staged.
    for (const entry of await readdir(source, { withFileTypes: true }))
    {
        if (entry.isFile())
            await copyFile(path.join(source, entry.name), path.join(destination, entry.name));
    }

    const emcmake = spawnSync("emcmake", ["--version"], { stdio: "ignore" });

    if (emcmake.error)
    {
        log("emscripten not found: docs get the field only; the landing-page "
            + "dial is skipped (install emsdk or run ci/install-emsdk.sh)");
        return "";
    }

    const buildDir = path.join(websiteDir, "animation", "_build");

    run("emcmake", [
        "cmake",
        "-S", "animation",
        "-B", "animation/_build",
        "-DCMAKE_BUILD_TYPE=Release",
    ]);
    run("cmake", ["--build", "animation/_build", "--target", "clockr_page"]);

    await copyFile(path.join(buildDir, "clockr_page.js"), path.join(destination, "clockr_page.js"));
    await copyFile(path.join(buildDir, "clockr_page.wasm"), path.join(destination, "clockr_page.wasm"));

    log("staged the landing-page animation (wasm shell + renderers)");
    return ANIMATION_MARKUP;
}

async function stageLab()
{
    const emcmake = spawnSync("emcmake", ["--version"], { stdio: "ignore" });

    if (emcmake.error)
    {
        log("emscripten not found: the 404 lab gallery is skipped");
        return false;
    }

    run("emcmake", [
        "cmake",
        "-S", "lab",
        "-B", "lab/_build",
        "-DCMAKE_BUILD_TYPE=Release",
    ]);
    run("cmake", ["--build", "lab/_build", "--target", "lab_shell"]);

    const source = path.join(websiteDir, "lab");
    const buildDir = path.join(source, "_build");
    const destination = path.join(siteDir, "lab");
    const webDestination = path.join(destination, "web");

    await mkdir(webDestination, { recursive: true });
    await copyFile(path.join(buildDir, "lab_shell.js"), path.join(destination, "lab_shell.js"));
    await copyFile(path.join(buildDir, "lab_shell.wasm"), path.join(destination, "lab_shell.wasm"));
    await copyFile(path.join(source, "lab.js"), path.join(destination, "lab.js"));
    await copyFile(path.join(source, "lab-common.js"), path.join(destination, "lab-common.js"));

    for (const entry of await readdir(path.join(source, "web"), { withFileTypes: true }))
    {
        if (entry.isFile())
            await copyFile(path.join(source, "web", entry.name), path.join(webDestination, entry.name));
    }

    log("staged the lab prototypes for the 404 page");
    return true;
}

async function stageDaw()
{
    const emcmake = spawnSync("emcmake", ["--version"], { stdio: "ignore" });

    if (emcmake.error)
    {
        log("emscripten not found: the in-browser DAWmulator is skipped "
            + "(install emsdk or run ci/install-emsdk.sh)");
        return false;
    }

    const buildDir = path.join(websiteDir, ".cache", "daw-wasm");

    run("emcmake", [
        "cmake",
        "-S", rootDir,
        "-B", buildDir,
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCLOCKR_BUILD_TESTS=OFF",
        "-DCLOCKR_BUILD_EXAMPLES=OFF",
        "-DCLOCKR_BUILD_DEBUGGER=OFF",
        "-DCLOCKR_BUILD_DEMO=ON",
        "-DCLOCKR_BUILD_DEMO_NATIVE=OFF",
    ]);
    run("cmake", ["--build", buildDir, "--target", "daw_shell", "--parallel"]);

    const built = path.join(buildDir, "demo", "daw", "shell");
    const destination = path.join(siteDir, "daw");

    await mkdir(destination, { recursive: true });

    for (const file of [
        "daw_shell.js",
        "daw_shell.wasm",
        "index.html",
        "style.css",
        "app.js",
        "engine_worker.js",
    ])
    {
        await copyFile(path.join(built, file), path.join(destination, file));
    }

    log("staged the in-browser DAWmulator (wasm shell + debug trace producer)");
    return true;
}

async function stageDebuggerUi()
{
    const source = path.join(rootDir, "tools", "web-debugger", "web");
    const destination = path.join(siteDir, "debugger");

    await mkdir(destination, { recursive: true });

    for (const file of [
        "app.js",
        "config.js",
        "index.html",
        "layout.js",
        "protobuf.js",
        "schema.js",
        "style.css",
    ])
    {
        await copyFile(path.join(source, file), path.join(destination, file));
    }

    await cp(path.join(source, "vendor"), path.join(destination, "vendor"), {
        recursive: true,
    });

    log("staged the debugger UI for the in-browser transport");
    return true;
}

async function stageNotFound(config, lab)
{
    const template = await readFile(path.join(websiteDir, "site", "404.template.html"), "utf8");

    const html = template
        .replaceAll("{{name}}", escapeHtml(config.name))
        .replaceAll("{{lab}}", lab ? LAB_MARKUP : LAB_FALLBACK);

    await writeFile(path.join(siteDir, "404.html"), html);
    log("staged the 404 page");
}

async function stampStyleVersion()
{
    const css = await readFile(path.join(siteDir, "style.css"));
    const version = createHash("sha256").update(css).digest("hex").slice(0, 8);

    async function stamp(dir)
    {
        const entries = await readdir(dir, { withFileTypes: true });

        for (const entry of entries)
        {
            const full = path.join(dir, entry.name);

            if (entry.isDirectory())
            {
                await stamp(full);
                continue;
            }

            if (!entry.name.endsWith(".html"))
                continue;

            const html = await readFile(full, "utf8");
            const stamped = html.replace(
                /href="([^"]*style\.css)"/g,
                (_, href) => `href="${href}?v=${version}"`,
            );

            if (stamped !== html)
                await writeFile(full, stamped);
        }
    }

    await stamp(siteDir);
    log(`stamped stylesheet v=${version}`);
}

async function main()
{
    const config = await readConfig();

    await rm(siteDir, { recursive: true, force: true });
    await mkdir(siteDir, { recursive: true });

    const animation = await stageAnimation();
    const lab = await stageLab();
    const daw = await stageDaw();
    const debuggerUi = daw ? await stageDebuggerUi() : false;

    await stageLanding(config, animation);
    await stageDemo(config, { daw, debugger: debuggerUi });
    await stageDocs(config);
    await stageNotFound(config, lab);
    await stampStyleVersion();

    log(`done: ${path.relative(rootDir, siteDir)}`);
}

main().catch((error) => {
    console.error(`[website] ${error.message}`);
    process.exitCode = 1;
});
