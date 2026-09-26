// Electron desktop packages are unpacked application folders, ready for SteamPipe.
import pc from "./colors.js";
import fs from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";
import { pathToFileURL } from "node:url";
import { createElectronPackagerOptions, readElectronConfig } from "./electron-config.js";
import type { ElectronOS } from "./electron-config.js";
import { ctx } from "./context.js";
import { $spawn } from "./utils.js";
import { getElectronVersion, withElectronHost, withElectronPackagerTemp } from "./electron-host.js";
import { findSteamLaunch, recordSteamPackage, writeSharedSteamDepots } from "./steam.js";
import { resolveToolPackages } from "./tool-packages.js";

export const resolveElectronTarget = (platform: string, arch = "", preview = false): {
    platform: "win32" | "darwin" | "linux";
    arch: "x64" | "arm64" | "universal";
} => {
    const os = platform.replace(/^steam:/, "");
    const target = { "windows": "win32", "macos": "darwin", "linux": "linux" }[os];
    if (target !== "win32" && target !== "darwin" && target !== "linux") {
        throw new Error(`Unsupported Electron platform: ${platform}`);
    }
    const cpu = arch || (preview ? process.arch : target === "darwin" ? "universal" : "x64");
    if (cpu !== "x64" && cpu !== "arm64" && cpu !== "universal") {
        throw new Error(`Unsupported Electron architecture: ${cpu}`);
    }
    if (cpu === "universal" && target !== "darwin") {
        throw new Error("--arch universal is only supported for macOS.");
    }
    if (preview && (target !== process.platform || cpu !== process.arch)) {
        throw new Error("Electron preview must use the host OS and architecture.");
    }
    return { "platform": target, "arch": cpu };
};

const run = (command: string, args: string[], cwd: string): Promise<void> =>
    new Promise((resolve, reject) => {
        const child = $spawn(command, args, { cwd, "stdio": "inherit" });
        child.once("error", reject);
        child.once("close", (code, signal) => {
            if (code !== 0) {
                reject(new Error(`${command} failed (${signal || code}).`));
                return;
            }
            resolve();
        });
    });

export const buildElectron = async (): Promise<void> => {
    const root = process.cwd();
    const config = readElectronConfig(root);
    const os = ctx.platform.replace(/^steam:/, "") as ElectronOS;
    const target = resolveElectronTarget(ctx.platform, ctx.arch || (ctx.preview ? "" : config.architectures[os]), ctx.preview);
    if (target.arch === "universal" && process.platform !== "darwin") {
        throw new Error("Build universal macOS applications on macOS.");
    }
    const steam = ctx.platform.startsWith("steam:");
    const version: string = JSON.parse(fs.readFileSync(path.join(root, "package.json"), "utf8")).version;
    const steamRoot = path.resolve(ctx.outDir, "steam");
    const outDir = path.resolve(ctx.outDir, ctx.platformDir, "build", ctx.environment);
    if (steam) {
        // A failed rebuild must not leave a shared manifest pointing at partially replaced files.
        fs.rmSync(path.join(outDir, "steam-package.json"), { "force": true });
        writeSharedSteamDepots(steamRoot, ctx.environment, config, version);
    }
    const outputPaths = await withElectronHost(root, ctx.buildDir, config, async (dir) => {
        const options = createElectronPackagerOptions(root, config, os, path.join(dir, "resources"));
        if (steam && os === "macos" && !options.osxNotarize) {
            console.log(pc.yellow("Local macOS build: unsigned/unnotarized builds must not be submitted for Steam release. See docs/steam.md."));
        }
        console.log(pc.green(`Packaging Electron (${target.platform}/${target.arch})`));
        const packages = await resolveToolPackages("electron");
        const require = createRequire(packages["@electron/packager"]);
        const { packager } = await import(pathToFileURL(require.resolve("@electron/packager")).href);
        // The generated host has no npm dependencies or native addons. Packager downloads
        // the pinned Electron runtime directly and uses its normal download cache.
        return await withElectronPackagerTemp(async (tmpdir) => await packager({
            ...options, dir, ...target,
            tmpdir,
            "out": outDir,
            "electronVersion": getElectronVersion(),
            "overwrite": true,
            "prune": false
        }) as string[]);
    }, `${os}-${target.arch}`);
    if (steam) {
        for (const content of outputPaths) {
            recordSteamPackage(content, ctx.platform, target.arch, config, version);
        }
        writeSharedSteamDepots(steamRoot, ctx.environment, config, version);
    }
    console.log(pc.green(`Finished Electron export: ${outDir}`));
    if (ctx.preview) {
        const content = outputPaths[0];
        const launch = findSteamLaunch(content, `steam:${os}`, config.executableName);
        const executable = target.platform === "darwin"
            ? path.join(content, launch, "Contents/MacOS", config.executableName)
            : path.join(content, launch);
        await run(executable, [], root);
    }
};
