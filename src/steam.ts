import fs from "node:fs";
import path from "node:path";
import { validateSteamId } from "./electron-config.js";
import type { IElectronConfig, ElectronOS } from "./electron-config.js";

export interface ISteamConfig {
    appId: string;
    depotId: string | null;
    sharedPlatforms?: ElectronOS[];
}

const platforms: ElectronOS[] = ["windows", "macos", "linux"];

export const readSteamConfig = (config: IElectronConfig, platform: string): ISteamConfig | null => {
    if (!config.steam?.appId) {
        return null;
    }
    const os = platform.replace(/^steam:/, "") as ElectronOS;
    const depot = config.steam.depots[os];
    const depotId = depot ? validateSteamId(depot) : null;
    const sharedPlatforms = depotId ? platforms.filter((key) => String(config.steam?.depots[key]) === depotId) : [];
    return { "appId": config.steam.appId, depotId, ...sharedPlatforms.length > 1 ? { sharedPlatforms } : {} };
};

export const quoteSteamValue = (value: string): string => {
    if (/["\r\n\0]/.test(value)) {
        throw new Error("Unsupported quote or control character in SteamPipe path.");
    }
    return `"${value.replace(/\\/g, "/")}"`;
};
const quote = quoteSteamValue;

/** Discover the executable from the output, including customized executable names. */
export const findSteamLaunch = (content: string, platform: string, executableName?: string): string => {
    const entries = fs.readdirSync(content, { "withFileTypes": true });
    if (platform === "steam:macos") {
        const apps = entries.filter((entry) => entry.isDirectory() && entry.name.endsWith(".app"));
        if (apps.length !== 1) {
            throw new Error("Expected exactly one macOS .app in the packaged folder.");
        }
        return apps[0].name;
    }
    if (executableName) {
        const executable = `${executableName}${platform === "steam:windows" ? ".exe" : ""}`;
        if (!fs.statSync(path.join(content, executable), { "throwIfNoEntry": false })?.isFile()) {
            throw new Error(`Packaged Steam executable is missing: ${executable}`);
        }
        return executable;
    }
    const candidates = entries.filter((entry) => {
        if (!entry.isFile()) {
            return false;
        }
        if (platform === "steam:windows") {
            return entry.name.endsWith(".exe") && entry.name !== "chrome_crashpad_handler.exe";
        }
        // Electron's other Linux binaries have extensions or known helper names.
        return !entry.name.includes(".") && !["chrome-sandbox", "chrome_crashpad_handler", "LICENSE", "version"].includes(entry.name)
            && (fs.statSync(path.join(content, entry.name)).mode & 0o111) !== 0;
    });
    if (candidates.length !== 1) {
        throw new Error(`Cannot determine the Steam executable in ${content}`);
    }
    return candidates[0].name;
};

/** Keep scripts outside the content directory so Steam never installs build metadata. */
export const writeSteamMetadata = (content: string, platform: string, arch: string, config: ISteamConfig | null, executableName?: string): void => {
    const localExecutable = findSteamLaunch(content, platform, executableName);
    const depotPath = config?.sharedPlatforms ? platform.replace("steam:", "") : "";
    const executable = depotPath ? `${depotPath}/${localExecutable}` : localExecutable;
    const scripts = `${content}-steampipe`;
    fs.mkdirSync(scripts, { "recursive": true });
    const metadata = {
        platform, arch, executable,
        localExecutable, depotPath,
        "contentRoot": `../${path.basename(content)}`,
        "workingDirectory": "",
        "arguments": "",
        "appId": config?.appId || null,
        "depotId": config?.depotId || null
    };
    fs.writeFileSync(path.join(scripts, "launch.json"), `${JSON.stringify(metadata, null, 2)}\n`);
    // Remove scripts from an earlier configured run if IDs are no longer configured.
    for (const name of ["app_build.vdf", "app_preview.vdf", "depot_build.vdf"]) {
        fs.rmSync(path.join(scripts, name), { "force": true });
    }
    if (!config?.depotId) {
        console.log("Steam package ready. Set steam.appId and steam.depots in electron.config.json to generate SteamPipe VDFs.");
        return;
    }
    // Upload a shared depot only through the combined manifest, never one OS at a time.
    if (config.sharedPlatforms) {
        return;
    }
    fs.writeFileSync(path.join(scripts, "depot_build.vdf"), `"DepotBuild"\n{\n    "DepotID" "${config.depotId}"\n    "FileMapping"\n    {\n        "LocalPath" "*"\n        "DepotPath" "."\n        "Recursive" "1"\n    }\n    "FileExclusion" "steam_appid.txt"\n    "FileExclusion" "*.pdb"\n    "FileExclusion" ".DS_Store"\n}\n`);
    for (const preview of [false, true]) {
        fs.writeFileSync(path.join(scripts, preview ? "app_preview.vdf" : "app_build.vdf"),
            `"AppBuild"\n{\n    "AppID" "${config.appId}"\n    "Desc" ${quote(`${platform} ${arch}`)}\n    "ContentRoot" ${quote(metadata.contentRoot)}\n    "BuildOutput" "cache"\n    "Preview" "${preview ? "1" : "0"}"\n    "Depots"\n    {\n        "${config.depotId}" "depot_build.vdf"\n    }\n}\n`);
    }
};

/** A portable pointer to the most recently exported architecture for this OS/environment. */
export const recordSteamPackage = (
    content: string, platform: string, arch: string, config: IElectronConfig, version: string
): void => {
    const steam = readSteamConfig(config, platform);
    writeSteamMetadata(content, platform, arch, steam, config.executableName);
    const record = {
        platform, arch, version,
        "appId": steam?.appId ?? null, "depotId": steam?.depotId ?? null,
        "appName": config.appName, "executableName": config.executableName,
        "contentRoot": path.basename(content)
    };
    fs.writeFileSync(path.join(path.dirname(content), "steam-package.json"), `${JSON.stringify(record, null, 2)}\n`);
};

/** Validate that an exported package still matches the current project and depot configuration. */
export const readSteamPackage = (steamRoot: string, environment: string, config: IElectronConfig, version: string, os: ElectronOS) => {
    const output = path.join(steamRoot, os, "build", environment);
    const record = JSON.parse(fs.readFileSync(path.join(output, "steam-package.json"), "utf8"));
    if (typeof record.contentRoot !== "string" || path.basename(record.contentRoot) !== record.contentRoot
        || /[\\/*?"\r\n\0]/.test(record.contentRoot) || record.contentRoot === "." || record.contentRoot === "..") {
        throw new Error("Invalid package path");
    }
    const content = path.join(output, record.contentRoot);
    if (record.platform !== `steam:${os}` || record.appId !== config.steam?.appId
        || record.depotId !== validateSteamId(config.steam?.depots[os])
        || record.version !== version || record.appName !== config.appName || record.executableName !== config.executableName
        || !(os === "macos" ? ["x64", "arm64", "universal"] : ["x64", "arm64"]).includes(record.arch)) {
        throw new Error("Package metadata no longer matches the project configuration");
    }
    const relativeRealPath = path.relative(fs.realpathSync(steamRoot), fs.realpathSync(content));
    if (path.isAbsolute(relativeRealPath) || relativeRealPath === ".." || relativeRealPath.startsWith(`..${path.sep}`)) {
        throw new Error("Package directory must be inside the Steam output root");
    }
    return { content, "arch": record.arch as string, "localExecutable": findSteamLaunch(content, `steam:${os}`, config.executableName) };
};

/** Combine existing packages with multiple FileMappings, preserving app symlinks and permissions. */
export const writeSharedSteamDepots = (
    steamRoot: string, environment: string, config: IElectronConfig, version: string
): string[] => {
    const pending: string[] = [];
    const groups = new Map<string, ElectronOS[]>();
    for (const os of platforms) {
        const id = config.steam?.depots[os];
        if (id) {
            const key = validateSteamId(id);
            groups.set(key, [...groups.get(key) ?? [], os]);
        }
    }
    const sharedRoot = path.join(steamRoot, "shared", environment);
    // Remove obsolete generated VDFs after a depot's membership or IDs change.
    if (fs.existsSync(sharedRoot)) {
        for (const entry of fs.readdirSync(sharedRoot, { "withFileTypes": true })) {
            if (entry.isDirectory() && /^depot-[0-9]+$/.test(entry.name)) {
                for (const name of ["app_build.vdf", "app_preview.vdf", "depot_build.vdf", "launch.json"]) {
                    fs.rmSync(path.join(sharedRoot, entry.name, name), { "force": true });
                }
            }
        }
    }
    for (const [depotId, members] of groups) {
        if (members.length < 2) {
            continue;
        }
        const scripts = path.join(sharedRoot, `depot-${depotId}`);
        fs.mkdirSync(scripts, { "recursive": true });
        const mappings: string[] = [];
        const launches: { platform: string; arch: string; executable: string; workingDirectory: string; arguments: string }[] = [];
        const missing: string[] = [];
        for (const os of members) {
            const output = path.join(steamRoot, os, "build", environment);
            // Also invalidate legacy exports created before steam-package.json existed.
            if (fs.existsSync(output)) {
                for (const entry of fs.readdirSync(output, { "withFileTypes": true })) {
                    if (entry.isDirectory() && entry.name.startsWith(`${config.appName}-`) && entry.name.endsWith("-steampipe")) {
                        for (const name of ["app_build.vdf", "app_preview.vdf", "depot_build.vdf"]) {
                            fs.rmSync(path.join(output, entry.name, name), { "force": true });
                        }
                    }
                }
            }
            try {
                const { content, arch, localExecutable } = readSteamPackage(steamRoot, environment, config, version, os);
                const relative = path.relative(steamRoot, content).replace(/\\/g, "/");
                mappings.push(`    "FileMapping"\n    {\n        "LocalPath" ${quote(`${relative}/*`)}\n        "DepotPath" "${os}/"\n        "Recursive" "1"\n    }`);
                launches.push({ "platform": `steam:${os}`, arch, "executable": `${os}/${localExecutable}`, "workingDirectory": "", "arguments": "" });
                writeSteamMetadata(content, `steam:${os}`, arch, readSteamConfig(config, `steam:${os}`), config.executableName);
            } catch (error) {
                missing.push(`${os}: ${error instanceof Error ? error.message : String(error)}`);
            }
        }
        fs.writeFileSync(path.join(scripts, "launch.json"), `${JSON.stringify({
            "appId": config.steam?.appId, depotId, "ready": !missing.length,
            "requiredPlatforms": members, missing, "launches": launches
        }, null, 2)}\n`);
        if (missing.length) {
            pending.push(`Depot ${depotId}: ${missing.join("; ")}`);
            console.log(`Shared Steam depot ${depotId}: waiting for all OS packages. See ${path.join(scripts, "launch.json")}`);
            continue;
        }
        fs.writeFileSync(path.join(scripts, "depot_build.vdf"),
            `"DepotBuild"\n{\n    "DepotID" "${depotId}"\n${mappings.join("\n")}\n    "FileExclusion" "steam_appid.txt"\n    "FileExclusion" "*.pdb"\n    "FileExclusion" ".DS_Store"\n}\n`);
        const contentRoot = path.relative(scripts, steamRoot);
        for (const preview of [false, true]) {
            fs.writeFileSync(path.join(scripts, preview ? "app_preview.vdf" : "app_build.vdf"),
                `"AppBuild"\n{\n    "AppID" "${config.steam?.appId}"\n    "Desc" ${quote(`Shared depot ${depotId}: ${members.join(", ")} ${version}`)}\n    "ContentRoot" ${quote(contentRoot)}\n    "BuildOutput" "cache"\n    "Preview" "${preview ? "1" : "0"}"\n    "Depots"\n    {\n        "${depotId}" "depot_build.vdf"\n    }\n}\n`);
        }
        console.log(`Shared Steam depot ready: ${scripts}`);
    }
    return pending;
};
