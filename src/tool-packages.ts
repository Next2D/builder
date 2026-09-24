import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawn } from "node:child_process";

// Keep native SDKs together and pin tools independently of the game's dependencies.
const packages = {
    "electron": { "@electron/packager": "20.3.0" },
    "capacitor": {
        "@capacitor/cli": "8.5.2", "@capacitor/core": "8.5.2",
        "@capacitor/ios": "8.5.2", "@capacitor/android": "8.5.2"
    }
};
type Tool = keyof typeof packages;
const pending = new Map<Tool, Promise<Record<string, string>>>();

/** Launch npm's npx script with Node, avoiding cmd.exe quoting on Windows. */
const resolveNpxCli = (): string => {
    const candidates = [
        ...process.env.npm_execpath ? [path.join(path.dirname(process.env.npm_execpath), "npx-cli.js")] : [],
        path.join(path.dirname(process.execPath), "node_modules/npm/bin/npx-cli.js"),
        ...(process.env.PATH ?? "").split(path.delimiter).flatMap((dir) => [
            path.join(dir, "npx"), path.join(dir, "node_modules/npm/bin/npx-cli.js")
        ])
    ];
    for (const file of candidates) {
        if (fs.statSync(file, { "throwIfNoEntry": false })?.isFile()) {
            const resolved = fs.realpathSync(file);
            if (path.basename(resolved) === "npx-cli.js") {
                return resolved;
            }
        }
    }
    throw new Error("npm/npx is required to download build tools. Install Node.js with npm and add it to PATH.");
};

const acquire = async (tool: Tool): Promise<Record<string, string>> => {
    const cli = resolveNpxCli();
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "next2d-tools-"));
    const result = path.join(dir, "packages.json");
    const specs = Object.entries(packages[tool]).map(([name, version]) => `--package=${name}@${version}`);
    try {
        console.log(`Preparing ${tool} tools via npx (${specs.map((spec) => spec.slice(10)).join(", ")})`);
        await new Promise<void>((resolve, reject) => {
            const child = spawn(process.execPath, [cli, "--yes", ...specs, "--call",
                "node --input-type=module -e \"import(process.env.NEXT2D_TOOL_RESOLVER)\""
            ], {
                // A neutral cwd prevents project dependencies and npm hooks influencing tool acquisition.
                "cwd": dir, "stdio": "inherit",
                "env": {
                    ...process.env,
                    "NEXT2D_TOOL_RESOLVER": new URL("./resolve-tool-packages.js", import.meta.url).href,
                    "NEXT2D_TOOL_PACKAGES": JSON.stringify(packages[tool]),
                    "NEXT2D_TOOL_RESULT": result
                }
            });
            child.once("error", reject);
            child.once("close", (code, signal) => code === 0 ? resolve()
                : reject(new Error(`npx could not prepare ${tool} tools (${signal || code}). Check npm connectivity/cache and retry.`)));
        });
        return JSON.parse(fs.readFileSync(result, "utf8"));
    } finally {
        fs.rmSync(dir, { "recursive": true, "force": true });
    }
};

/** Download only on first use; npm retains the packages after the temporary request is removed. */
export const resolveToolPackages = (tool: Tool): Promise<Record<string, string>> => {
    let request = pending.get(tool);
    if (!request) {
        request = acquire(tool).catch((error) => {
            pending.delete(tool);
            throw error;
        });
        pending.set(tool, request);
    }
    return request;
};
