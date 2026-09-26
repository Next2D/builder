import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { getTemplateDir } from "./utils.js";
import type { IElectronConfig } from "./electron-config.js";
import { stageNativeBridge } from "./electron-native.js";

/** Packager cleans its temporary root; never share that root with concurrent builds. */
export const withElectronPackagerTemp = async <T>(use: (directory: string) => Promise<T>): Promise<T> => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "next2d-packager-"));
    try {
        return await use(dir);
    } finally {
        fs.rmSync(dir, { "recursive": true, "force": true });
    }
};

/** The builder pins the runtime; games need neither Electron nor its npm dependency tree. */
export const getElectronVersion = (): string => {
    const pkg = JSON.parse(fs.readFileSync(path.join(getTemplateDir("electron"), "package.json"), "utf8"));
    const version = pkg.devDependencies.electron;
    if (typeof version !== "string" || !/^\d+\.\d+\.\d+$/.test(version)) {
        throw new Error("The builder's Electron template must pin an exact runtime version.");
    }
    return version;
};

/** No user files are written or removed. Each build gets an isolated, disposable host. */
export const withElectronHost = async <T>(
    root: string, resources: string, config: IElectronConfig, use: (directory: string) => Promise<T>, target?: string
): Promise<T> => {
    if (!fs.statSync(path.join(resources, "index.html"), { "throwIfNoEntry": false })?.isFile()) {
        throw new Error(`Electron web entry point is missing: ${resources}/index.html`);
    }
    const game = JSON.parse(fs.readFileSync(path.join(root, "package.json"), "utf8"));
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "next2d-electron-"));
    try {
        for (const file of ["index.js", "local-assets.cjs", "native-bridge.cjs", "preload.cjs"]) {
            fs.copyFileSync(path.join(getTemplateDir("electron"), file), path.join(dir, file));
        }
        const pkg = {
            "name": game.name, "version": game.version, "description": config.description,
            "private": true, "type": "commonjs", "main": "index.js"
        };
        fs.writeFileSync(path.join(dir, "package.json"), `${JSON.stringify(pkg, null, 2)}\n`);
        fs.cpSync(resources, path.join(dir, "resources"), { "recursive": true });
        const nativeBridge = config.nativeBridge ? stageNativeBridge(root, dir, config.nativeBridge, target ?? "") : undefined;
        const runtime = { "appId": config.appId, "appName": config.appName, "window": config.window, "icon": "", nativeBridge };
        if (config.icons.linux) {
            fs.copyFileSync(path.resolve(root, config.icons.linux), path.join(dir, "resources/desktop-icon.png"));
            runtime.icon = "desktop-icon.png";
        }
        fs.writeFileSync(path.join(dir, "runtime-config.json"), `${JSON.stringify(runtime, null, 2)}\n`);
        return await use(dir);
    } finally {
        fs.rmSync(dir, { "recursive": true, "force": true });
    }
};
