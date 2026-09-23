// iOS / Android (Capacitor) ビルド。SDKとCLIはnpxで必要時に取得する。
import fs from "node:fs";
import path from "node:path";
import { ctx } from "./context.js";
import { $spawn } from "./utils.js";
import { CAPACITOR_CONFIG_NAME } from "./constants.js";
import { resolveToolPackages } from "./tool-packages.js";

/** Resolve from npm's persistent tool cache without installing into the game. */
export const getCapacitorCommand = async (): Promise<{ cli: string; env: NodeJS.ProcessEnv }> => {
    const resolved = await resolveToolPackages("capacitor");
    const packages = ["cli", "core", "ios", "android"].map((name) =>
        resolved[`@capacitor/${name}`]);
    // Keep the game's cwd for plugin discovery/hooks; supply SDKs through NODE_PATH.
    const roots = [...new Set(packages.map((file) => path.dirname(path.dirname(path.dirname(file)))))];
    return {
        "cli": path.join(path.dirname(packages[0]), "bin", "capacitor"),
        "env": { ...process.env, "NODE_PATH": [...roots, process.env.NODE_PATH].filter(Boolean).join(path.delimiter) }
    };
};

/** Await every CLI operation so failures propagate to the builder and CI. */
export const runCapacitor = async (args: string[], root = process.cwd()): Promise<void> => {
    const { cli, env } = await getCapacitorCommand();
    return new Promise((resolve, reject) => {
        const child = $spawn(process.execPath, [cli, ...args], { "cwd": root, env, "stdio": "inherit" });
        child.once("error", reject);
        child.once("close", (code, signal) => {
            if (code !== 0) {
                reject(new Error(`Capacitor ${args.join(" ")} failed (${signal || code}).`));
                return;
            }
            resolve();
        });
    });
};

const prepareNativeProject = async (): Promise<void> => {
    if (ctx.platform !== "ios" && ctx.platform !== "android") {
        throw new Error(`Unsupported Capacitor platform: ${ctx.platform}`);
    }
    const root = process.cwd();
    const file = path.join(root, CAPACITOR_CONFIG_NAME);
    const config = JSON.parse(fs.readFileSync(file, "utf8"));
    // cap add performs an initial sync, so it must see the new webDir too.
    config.webDir = path.relative(root, path.resolve(ctx.outDir, ctx.platformDir, ctx.environment)).replace(/\\/g, "/") + "/";
    fs.writeFileSync(file, JSON.stringify(config, null, 2) + "\n");
    const platformDir = path.resolve(root, config[ctx.platform]?.path ?? ctx.platform);
    if (!fs.existsSync(platformDir)) {
        await runCapacitor(["add", ctx.platform]);
    }
};

/** Keep native customizations and update web assets/dependency paths in place. */
export const syncNative = async (): Promise<void> => {
    await prepareNativeProject();
    await runCapacitor(["sync", ctx.platform]);
};

export const runNative = async (): Promise<void> => {
    await prepareNativeProject();
    await runCapacitor(["run", ctx.platform]);
};

export const openNative = async (): Promise<void> => {
    await syncNative();
    await runCapacitor(["open", ctx.platform]);
};

export const buildNative = async (): Promise<void> => {
    await syncNative();
    await runCapacitor(["build", ctx.platform]);
};
