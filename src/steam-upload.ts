import fs from "node:fs";
import path from "node:path";
import { spawn } from "node:child_process";
import { readElectronConfig, validateSteamId } from "./electron-config.js";
import { quoteSteamValue as quote, readSteamPackage } from "./steam.js";

export interface ISteamUploadOptions {
    root: string;
    steamRoot: string;
    environment: string;
    branch?: string;
    dryRun?: boolean;
}

export const validateSteamBranch = (branch: unknown): string => {
    if (typeof branch !== "string" || !/^[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}$/.test(branch)
        || ["default", "public", "none"].includes(branch.toLowerCase())) {
        throw new Error("Steam upload requires a named beta branch such as internal (letters, numbers, - or _). The default branch is not supported.");
    }
    return branch;
};

/** Recreate manifests from validated package records, never from stale or edited VDF files. */
export const prepareSteamUpload = (options: ISteamUploadOptions) => {
    const root = path.resolve(options.root);
    const steamRoot = path.resolve(root, options.steamRoot);
    if (!/^[a-zA-Z0-9][a-zA-Z0-9_-]*$/.test(options.environment)) {
        throw new Error("Steam upload --env must contain only letters, numbers, - or _.");
    }
    const config = readElectronConfig(root);
    const branch = validateSteamBranch(options.branch ?? config.steam?.branch ?? "internal");
    if (!config.steam?.appId) {
        throw new Error("Set steam.appId and steam.depots in electron.config.json before uploading.");
    }
    const version = JSON.parse(fs.readFileSync(path.join(root, "package.json"), "utf8")).version;
    if (typeof version !== "string" || !version) {
        throw new Error("Set package.json version before uploading.");
    }
    const groups = new Map<string, { os: string; content: string; arch: string; localExecutable: string }[]>();
    for (const os of ["windows", "macos", "linux"] as const) {
        const id = config.steam.depots[os];
        if (id === null || id === undefined) {
            continue;
        }
        const depot = validateSteamId(id);
        try {
            const record = readSteamPackage(steamRoot, options.environment, config, version, os);
            groups.set(depot, [...groups.get(depot) ?? [], { os, ...record }]);
        } catch (error) {
            throw new Error(`Steam upload: ${os} package is missing or outdated. Export/collect every configured OS first. ${error instanceof Error ? error.message : error}`);
        }
    }
    if (!groups.size) {
        throw new Error("Set at least one Steam Depot ID in electron.config.json before uploading.");
    }
    // Unique scripts prevent a dry-run or another invocation from overwriting an active upload.
    const uploads = path.join(steamRoot, "uploads", options.environment);
    fs.mkdirSync(uploads, { "recursive": true });
    const scripts = fs.mkdtempSync(path.join(uploads, `${branch}-`));
    const depotFiles: string[] = [];
    const launches: { platform: string; arch: string; executable: string; depotId: string }[] = [];
    for (const [depot, members] of groups) {
        const mappings = members.map(({ os, content, arch, localExecutable }) => {
            const prefix = members.length > 1 ? `${os}/` : "";
            launches.push({ "platform": `steam:${os}`, arch, "executable": `${prefix}${localExecutable}`, "depotId": depot });
            return `    "FileMapping"\n    {\n        "LocalPath" ${quote(`${path.relative(steamRoot, content)}/*`)}\n        "DepotPath" ${quote(prefix || ".")}\n        "Recursive" "1"\n    }`;
        });
        const name = `depot_${depot}.vdf`;
        fs.writeFileSync(path.join(scripts, name),
            `"DepotBuild"\n{\n    "DepotID" "${depot}"\n${mappings.join("\n")}\n    "FileExclusion" "steam_appid.txt"\n    "FileExclusion" "*.pdb"\n    "FileExclusion" ".DS_Store"\n}\n`);
        depotFiles.push(`        "${depot}" ${quote(name)}`);
    }
    const manifest = path.join(scripts, options.dryRun ? "app_preview.vdf" : "app_upload.vdf");
    // A stable cache directory allows SteamPipe to reuse chunks across uploads to a branch.
    const cache = path.join(uploads, "cache", branch);
    fs.writeFileSync(manifest,
        `"AppBuild"\n{\n    "AppID" "${config.steam.appId}"\n    "Desc" ${quote(`${config.appName} ${version} (${options.environment}, ${branch})`)}\n`
        + `    "ContentRoot" ${quote(steamRoot)}\n    "BuildOutput" ${quote(cache)}\n    "Preview" "${options.dryRun ? "1" : "0"}"\n`
        + (options.dryRun ? "" : `    "SetLive" ${quote(branch)}\n`)
        + `    "Depots"\n    {\n${depotFiles.join("\n")}\n    }\n}\n`);
    const plan = { "appId": config.steam.appId, branch, version, "environment": options.environment, "dryRun": !!options.dryRun, manifest, launches };
    fs.writeFileSync(path.join(scripts, "upload-plan.json"), `${JSON.stringify(plan, null, 2)}\n`);
    return plan;
};

/** Use SteamCMD's existing login session; passwords/Steam Guard codes never enter project config. */
export const uploadSteam = async (options: ISteamUploadOptions): Promise<void> => {
    const plan = prepareSteamUpload(options);
    console.log(`Steam App ${plan.appId} -> beta branch ${plan.branch}`);
    console.log(`Manifest: ${plan.manifest}`);
    for (const launch of plan.launches) {
        console.log(`  Depot ${launch.depotId}: ${launch.platform}/${launch.arch} -> ${launch.executable}`);
    }
    if (options.dryRun) {
        console.log("Dry run complete: SteamCMD was not started. Branch existence/password protection has not been checked on Steamworks.");
        return;
    }
    const username = process.env.STEAM_USERNAME;
    if (!username || !/^[a-zA-Z0-9_][a-zA-Z0-9_.@-]*$/.test(username)) {
        throw new Error("Set STEAM_USERNAME to your Steam build account name. Log in with SteamCMD once before uploading; see docs/steam.md.");
    }
    const command = process.env.STEAMCMD || (process.platform === "win32" ? "steamcmd.exe" : "steamcmd");
    console.log(`Uploading and requesting activation on ${plan.branch}. Configure the beta password in Steamworks to restrict access.`);
    const buildId = await new Promise<string>((resolve, reject) => {
        let output = "";
        let reportedError = false;
        const child = spawn(command, [
            "+@ShutdownOnFailedCommand", "1", "+@NoPromptForPassword", "1",
            "+login", username, "+run_app_build", plan.manifest, "+quit"
        ], { "cwd": path.resolve(options.root), "stdio": ["ignore", "pipe", "pipe"], "shell": false });
        const collect = (chunk: Buffer, stream: NodeJS.WriteStream): void => {
            stream.write(chunk);
            output = (output + chunk.toString()).slice(-1024 * 1024);
            reportedError ||= /ERROR[!:]|Failed to set[^\r\n]*live/i.test(output);
        };
        child.stdout.on("data", (chunk: Buffer) => collect(chunk, process.stdout));
        child.stderr.on("data", (chunk: Buffer) => collect(chunk, process.stderr));
        child.once("error", (error) => reject(new Error(`Cannot start SteamCMD: ${error.message}. Set STEAMCMD to the executable path; see docs/steam.md.`)));
        child.once("close", (code, signal) => {
            if (code !== 0) {
                reject(new Error(`SteamCMD failed (${signal || code}). Inspect its output and SteamPipe logs; check login, app permissions and beta branch settings.`));
                return;
            }
            const success = output.match(new RegExp(
                `(?:Success! App '${plan.appId}' fully built|Successfully finished AppID ${plan.appId} build) \\(BuildID ([0-9]+)\\)`, "i"
            ));
            if (reportedError || !success) {
                reject(new Error("SteamCMD did not report a successful app build, or reported an error. Check SteamPipe logs and the branch's active BuildID in Steamworks before retrying."));
                return;
            }
            resolve(success[1]);
        });
    });
    fs.writeFileSync(path.join(path.dirname(plan.manifest), "upload-result.json"), `${JSON.stringify({
        "appId": plan.appId, buildId, "requestedBranch": plan.branch, "completedAt": new Date().toISOString()
    }, null, 2)}\n`);
    console.log(`Uploaded BuildID ${buildId}. Verify the active ${plan.branch} build at https://partner.steamgames.com/apps/builds/${plan.appId}`);
};
