import { spawnSync } from "node:child_process";
import { readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

export const websiteDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
export const rootDir = path.resolve(websiteDir, "../..");
export const siteDir = path.join(websiteDir, "_site");

export async function readConfig()
{
    return JSON.parse(await readFile(path.join(websiteDir, "site.config.json"), "utf8"));
}

export function run(command, args, options = {})
{
    console.log(`[website] $ ${command} ${args.join(" ")}`);

    const result = spawnSync(command, args, {
        cwd: websiteDir,
        stdio: "inherit",
        ...options,
    });

    if (result.error)
        throw new Error(`${command}: ${result.error.message}`);

    if (result.status !== 0)
        throw new Error(`${command} exited with status ${result.status}`);
}
