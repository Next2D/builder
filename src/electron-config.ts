import fs from "node:fs";
import path from "node:path";
import { getTemplateDir } from "./utils.js";
import { resolveDescription } from "./description.js";
import { validateNativeBridge } from "./electron-native.js";
import type { INativeBridgeConfig } from "./electron-native.js";

export type ElectronOS = "windows" | "macos" | "linux";
export interface IElectronConfig {
    nativeBridge?: INativeBridgeConfig;
    appId: string;
    appName: string;
    description: string;
    executableName: string;
    companyName: string;
    icons: Partial<Record<ElectronOS, string>>;
    architectures: Partial<Record<ElectronOS, string>>;
    window: { width: number; height: number; fullscreen: boolean };
    steam?: { appId: string | null; depots: Partial<Record<ElectronOS, string | null>>; branch?: string };
    macos: { sign: boolean; notarize: boolean; localNetworkUsageDescription?: string };
}

export const validateSteamId = (value: unknown): string => {
    const id = String(value);
    if (!/^[1-9][0-9]*$/.test(id) || !Number.isSafeInteger(Number(id)) || Number(id) > 4294967295) {
        throw new Error("Steam App ID and Depot IDs must be positive 32-bit integers.");
    }
    return id;
};

export const readElectronConfig = (root: string): IElectronConfig => {
    const pkg = JSON.parse(fs.readFileSync(path.join(root, "package.json"), "utf8"));
    const file = path.resolve(root, process.env.NEXT2D_ELECTRON_CONFIG_FILE || "electron.config.json");
    if (process.env.NEXT2D_ELECTRON_CONFIG_FILE && !fs.existsSync(file)) {
        throw new Error("NEXT2D_ELECTRON_CONFIG_FILE does not exist.");
    }
    const input = fs.existsSync(file) ? JSON.parse(fs.readFileSync(file, "utf8")) : {};
    const config: IElectronConfig = {
        "appId": input.appId ?? `app.next2d.${pkg.name}`,
        "appName": input.appName ?? pkg.name,
        "description": resolveDescription(input.description, pkg.description),
        "executableName": input.executableName ?? pkg.name,
        "companyName": input.companyName ?? pkg.author?.name ?? input.appName ?? pkg.name,
        "icons": {
            "windows": path.join(getTemplateDir("electron"), "icons/icon.ico"),
            "macos": path.join(getTemplateDir("electron"), "icons/icon.icns"),
            "linux": path.join(getTemplateDir("electron"), "icons/icon.png"),
            ...input.icons
        },
        "architectures": input.architectures ?? {},
        "window": { "width": 1280, "height": 720, "fullscreen": false, ...input.window },
        "macos": { "sign": false, "notarize": false, ...input.macos },
        "steam": input.steam,
        "nativeBridge": input.nativeBridge
    };
    if (typeof config.appId !== "string" || !/^[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+$/.test(config.appId)) {
        throw new Error("electron.config.json appId must be a reverse-domain bundle identifier.");
    }
    for (const [key, value] of [["appName", config.appName], ["executableName", config.executableName]]) {
        if (typeof value !== "string" || !value.trim() || /[<>:"/\\|?*\x00-\x1f]/.test(value) || /[. ]$/.test(value) || value === "..") {
            throw new Error(`electron.config.json ${key} must be a valid portable file name.`);
        }
    }
    if (typeof config.companyName !== "string" || !config.companyName.trim()) {
        throw new Error("electron.config.json companyName must be a nonempty string.");
    }
    for (const dimension of [config.window.width, config.window.height]) {
        if (!Number.isInteger(dimension) || dimension < 1 || dimension > 16384) {
            throw new Error("Electron window width/height must be integers between 1 and 16384.");
        }
    }
    for (const value of [config.window.fullscreen, config.macos.sign, config.macos.notarize]) {
        if (typeof value !== "boolean") {
            throw new Error("Electron fullscreen/sign/notarize options must be booleans.");
        }
    }
    if (config.macos.notarize && !config.macos.sign) {
        throw new Error("macos.notarize requires macos.sign.");
    }
    const localNetwork = config.macos.localNetworkUsageDescription;
    if (localNetwork !== undefined && (typeof localNetwork !== "string" || !localNetwork.trim()
        || localNetwork.length > 1000 || localNetwork.includes("\0"))) {
        throw new Error("macos.localNetworkUsageDescription must be a nonempty string of at most 1000 characters.");
    }
    for (const os of ["windows", "macos", "linux"] as const) {
        const icon = config.icons[os];
        if (icon !== undefined) {
            const extension = { "windows": ".ico", "macos": ".icns", "linux": ".png" }[os];
            if (typeof icon !== "string" || path.extname(icon).toLowerCase() !== extension || !fs.statSync(path.resolve(root, icon), { "throwIfNoEntry": false })?.isFile()) {
                throw new Error(`electron.config.json icons.${os} must point to an existing ${extension} file (relative to the project root).`);
            }
        }
        const arch = config.architectures[os];
        if (arch !== undefined && !(os === "macos" ? ["x64", "arm64", "universal"] : ["x64", "arm64"]).includes(arch)) {
            throw new Error(`Invalid electron.config.json architectures.${os}.`);
        }
    }
    if (config.steam) {
        config.steam.appId = config.steam.appId === null || config.steam.appId === undefined ? null : validateSteamId(config.steam.appId);
        const depots = config.steam.depots ?? {};
        const ids = Object.values(depots).filter((value) => value !== null).map(validateSteamId);
        if (ids.length && !config.steam.appId) {
            throw new Error("Set steam.appId before configuring Steam Depot IDs.");
        }
        config.steam.depots = depots;
    }
    validateNativeBridge(config.nativeBridge);
    return config;
};

/** JSON is the source of game metadata; secrets stay in the environment/keychain. */
export const createElectronPackagerOptions = (
    root: string, config: IElectronConfig, os: ElectronOS, resources: string
) => {
    const pkg = JSON.parse(fs.readFileSync(path.join(root, "package.json"), "utf8"));
    const release = process.env.NEXT2D_STEAM_RELEASE === "1";
    const sign = config.macos.sign || release;
    const notarize = config.macos.notarize || release;
    if (os === "macos" && sign && !process.env.APPLE_SIGNING_IDENTITY) {
        throw new Error("macOS signing requires APPLE_SIGNING_IDENTITY.");
    }
    if (os === "macos" && notarize && !process.env.APPLE_NOTARY_PROFILE) {
        throw new Error("macOS notarization requires APPLE_NOTARY_PROFILE (notarytool keychain profile).");
    }
    return {
        "name": config.appName,
        "executableName": config.executableName,
        "appBundleId": config.appId,
        "appVersion": pkg.version,
        "win32metadata": { "CompanyName": config.companyName, "FileDescription": config.description },
        "asar": true,
        "extraResource": [resources, ...config.nativeBridge ? [path.join(path.dirname(resources), "native")] : []],
        "ignore": [/^\/native(?:\/|$)/, /^\/resources(?:\/|$)/, /^\/icons(?:\/|$)/, /^\/forge\.config\.[cm]?js$/, /^\/entitlements\.plist$/, /(?:^|\/)steam_appid\.txt$/],
        ...os !== "linux" && config.icons[os] ? { "icon": path.resolve(root, config.icons[os]) } : {},
        ...os === "macos" && config.macos.localNetworkUsageDescription ? {
            "extendInfo": { "NSLocalNetworkUsageDescription": config.macos.localNetworkUsageDescription },
            "extendHelperInfo": { "NSLocalNetworkUsageDescription": config.macos.localNetworkUsageDescription }
        } : {},
        ...os === "macos" && sign ? {
            "osxSign": {
                "identity": process.env.APPLE_SIGNING_IDENTITY,
                // Packager otherwise swallows signing failures and attempts notarization.
                "continueOnError": false,
                // Static web/localization data is sealed by its enclosing bundle.
                // Keep native sidecars and libraries outside these exclusions.
                "ignore": ["/Contents/Resources/resources/", "\\.lproj/locale\\.pak$"],
                "optionsForFile": () => ({
                    "hardenedRuntime": true,
                    "entitlements": path.join(getTemplateDir("electron"), "entitlements.plist")
                })
            }
        } : {},
        ...os === "macos" && notarize ? { "osxNotarize": { "keychainProfile": process.env.APPLE_NOTARY_PROFILE! } } : {}
    };
};
