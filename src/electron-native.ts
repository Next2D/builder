import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";

export interface INativeBridgeConfig {
    methods: string[];
    targets: Record<string, { directory: string; executable: string }>;
    backgroundThrottling?: boolean;
    prepare?: string;
}

export const validateNativeBridge = (value: INativeBridgeConfig | undefined): void => {
    if (value === undefined) {
        return;
    }
    if (value && value.prepare !== undefined && (typeof value.prepare !== "string" || !value.prepare.trim()
        || path.isAbsolute(value.prepare) || value.prepare.includes("\0") || !/\.[cm]?js$/.test(value.prepare))) {
        throw new Error("nativeBridge.prepare must be a project-relative Node.js script.");
    }
    if (!value || !Array.isArray(value.methods) || !value.methods.length
        || value.methods.some((method) => typeof method !== "string" || !/^[a-zA-Z][a-zA-Z0-9_.]{0,63}$/.test(method))
        || !value.targets || typeof value.targets !== "object" || Array.isArray(value.targets)
        || value.backgroundThrottling !== undefined && typeof value.backgroundThrottling !== "boolean") {
        throw new Error("Invalid nativeBridge methods/targets.");
    }
    for (const [target, entry] of Object.entries(value.targets)) {
        if (!/^(windows|linux)-(x64|arm64)$|^macos-(x64|arm64|universal)$/.test(target)
            || !entry || typeof entry.directory !== "string" || !entry.directory.trim()
            || typeof entry.executable !== "string" || !/^[a-zA-Z0-9_-][a-zA-Z0-9_.-]*$/.test(entry.executable)) {
            throw new Error(`Invalid nativeBridge target: ${target}`);
        }
    }
};

export const stageNativeBridge = (root: string, host: string, config: INativeBridgeConfig, target: string) => {
    validateNativeBridge(config);
    const entry = config.targets[target];
    if (!entry) {
        throw new Error(`nativeBridge requires a ${target} target (universal builds require universal native binaries).`);
    }
    const source = path.resolve(root, entry.directory);
    const check = (file: string): void => {
        const stat = fs.lstatSync(file);
        if (stat.isSymbolicLink() || !stat.isFile() && !stat.isDirectory()) {
            throw new Error(`Native bridge bundles must contain only regular files/directories: ${file}`);
        }
        if (stat.isDirectory()) {
            for (const child of fs.readdirSync(file)) {
                check(path.join(file, child));
            }
        }
    };
    const destination = path.join(host, "native");
    if (config.prepare) {
        const script = fs.realpathSync(path.resolve(root, config.prepare));
        const relative = path.relative(fs.realpathSync(root), script);
        if (relative === ".." || relative.startsWith(`..${path.sep}`) || path.isAbsolute(relative)) {
            throw new Error("nativeBridge.prepare must stay inside the project.");
        }
        fs.mkdirSync(destination, { "mode": 0o700 });
        const result = spawnSync(process.execPath, [script, "--target", target, "--source", source, "--destination", destination],
            { "cwd": root, "stdio": "inherit", "shell": false, "timeout": 120000 });
        if (result.error) {
            throw result.error;
        }
        if (result.status !== 0) {
            throw new Error(`Native preparation failed (${result.status}).`);
        }
    } else {
        check(source);
        fs.cpSync(source, destination, { "recursive": true });
    }
    check(destination);
    if (!fs.statSync(path.join(destination, entry.executable), { "throwIfNoEntry": false })?.isFile()) {
        throw new Error(`Native bridge executable is missing: ${entry.executable}`);
    }
    if (!target.startsWith("windows-")) {
        fs.chmodSync(path.join(destination, entry.executable), 0o755);
    }
    return { "executable": entry.executable, "methods": config.methods,
        ...config.backgroundThrottling !== undefined ? { "backgroundThrottling": config.backgroundThrottling } : {} };
};
