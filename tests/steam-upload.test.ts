import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { readElectronConfig } from "../dist/electron-config.js";
import { recordSteamPackage } from "../dist/steam.js";
import { prepareSteamUpload, validateSteamBranch } from "../dist/steam-upload.js";
import { parseArgv } from "../dist/cli.js";

const fixture = (t, depots = { windows: 2409461, macos: 2409461, linux: 2409461 }) => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "next2d upload "));
    t.after(() => fs.rmSync(root, { recursive: true, force: true }));
    fs.writeFileSync(path.join(root, "package.json"), JSON.stringify({ name: "test-game", version: "1.0.0" }));
    fs.writeFileSync(path.join(root, "electron.config.json"), JSON.stringify({
        appName: "Test Game", executableName: "my-game", steam: { appId: 2409460, branch: "internal", depots }
    }));
    const config = readElectronConfig(root);
    const steamRoot = path.join(root, "custom output/steam");
    const contents = {};
    for (const platform of Object.keys(depots)) {
        if (!depots[platform]) continue;
        const content = path.join(steamRoot, platform, "build/prd", `Test Game-${platform}`);
        fs.mkdirSync(content, { recursive: true });
        if (platform === "macos") fs.mkdirSync(path.join(content, "Test Game.app"));
        else fs.writeFileSync(path.join(content, platform === "windows" ? "my-game.exe" : "my-game"), "binary", { mode: 0o755 });
        recordSteamPackage(content, `steam:${platform}`, "x64", config, "1.0.0");
        contents[platform] = content;
    }
    return { root, steamRoot, environment: "prd", contents };
};

const cli = (f, args = [], env = {}) => spawnSync(process.execPath, [
    fileURLToPath(new URL("../dist/index.js", import.meta.url)),
    "--steam-upload", "--env", "prd", "--steam-root", f.steamRoot, ...args
], {
    cwd: f.root, encoding: "utf8", timeout: 10000,
    env: { ...process.env, STEAM_USERNAME: "", STEAMCMD: path.join(f.root, "missing-steamcmd"), ...env }
});

test("Steam upload CLI is platform independent and rejects mixed actions, missing values and unknown flags", () => {
    const args = ["--steam-upload", "--env", "prd"];
    assert.equal(parseArgv(args).hasHelp, false);
    assert.equal(parseArgv([...args, "--dry-run"]).dryRun, true);
    assert.equal(parseArgv([...args, "--steam-branch", "internal"]).steamBranch, "internal");
    assert.equal(parseArgv([...args, "--steam-comment", "内部テスト用"]).steamComment, "内部テスト用");
    for (const extra of [["--platform", "steam:macos"], ["--steam-manifest"], ["--preview"], ["--build"], ["--open"],
        ["--arch", "x64"], ["--steam-branch"], ["--steam-root"], ["--steam-comment"],
        ["--steam-comment", ""], ["--steam-comment", "--dry-run"], ["--dryrun"]]) {
        assert.equal(parseArgv([...args, ...extra]).hasHelp, true, extra.join(" "));
    }
    assert.equal(parseArgv(["--steam-upload"]).hasHelp, true);
    assert.equal(parseArgv(["--platform", "web", "--env", "prd", "--dry-run"]).hasHelp, true);
    assert.equal(parseArgv(["--platform", "web", "--env", "prd", "--steam-comment", "test"]).hasHelp, true);
});

test("upload combines a shared depot once, sets internal, and preserves ordinary build manifests", (t) => {
    const f = fixture(t);
    const plan = prepareSteamUpload(f);
    const manifest = fs.readFileSync(plan.manifest, "utf8");
    assert.match(manifest, /"Desc" "Test Game 1\.0\.0 \(prd, internal\)"/);
    assert.equal(plan.comment, "Test Game 1.0.0 (prd, internal)");
    assert.match(manifest, /"SetLive" "internal"/);
    assert.match(manifest, /"Preview" "0"/);
    assert.equal((manifest.match(/"2409461" "depot_2409461.vdf"/g) || []).length, 1);
    assert.equal(plan.launches.length, 3);
    const depot = fs.readFileSync(path.join(path.dirname(plan.manifest), "depot_2409461.vdf"), "utf8");
    for (const os of ["windows", "macos", "linux"]) assert.match(depot, new RegExp(`"DepotPath" "${os}/"`));
    assert.doesNotMatch(depot, /steam-package\.json|uploads\//);
    assert.match(depot, /"FileExclusion" "steam_appid.txt"/);
    const preview = prepareSteamUpload({ ...f, dryRun: true });
    assert.notEqual(preview.manifest, plan.manifest);
    assert.doesNotMatch(fs.readFileSync(preview.manifest, "utf8"), /SetLive/);
    assert.match(fs.readFileSync(preview.manifest, "utf8"), /"Preview" "1"/);
    assert.equal(fs.readFileSync(plan.manifest, "utf8"), manifest);
});

test("separate and shared depots are uploaded in the same app build with correct install paths", (t) => {
    const f = fixture(t, { windows: 2409461, macos: 2409461, linux: 2409462 });
    const plan = prepareSteamUpload({ ...f, branch: "qa" });
    const manifest = fs.readFileSync(plan.manifest, "utf8");
    for (const depot of [2409461, 2409462]) assert.ok(manifest.includes(`"${depot}" "depot_${depot}.vdf"`));
    assert.match(manifest, /"SetLive" "qa"/);
    const linux = fs.readFileSync(path.join(path.dirname(plan.manifest), "depot_2409462.vdf"), "utf8");
    assert.match(linux, /"DepotPath" "\."/);
    assert.equal(plan.launches.find(x => x.platform === "steam:linux").executable, "my-game");
    assert.doesNotMatch(fs.readFileSync(`${f.contents.linux}-steampipe/app_build.vdf`, "utf8"), /SetLive/);
});

test("missing OS, stale version/IDs, invalid arch and escaped package paths block upload before SteamCMD", (t) => {
    const f = fixture(t);
    const file = path.join(path.dirname(f.contents.windows), "steam-package.json");
    const original = JSON.parse(fs.readFileSync(file, "utf8"));
    for (const patch of [{ version: "0.9.0" }, { appId: "480" }, { depotId: "2409462" }, { arch: "ia32" },
        { contentRoot: "../secret" }, { contentRoot: "..\\secret" }]) {
        fs.writeFileSync(file, JSON.stringify({ ...original, ...patch }));
        const result = cli(f, [], { STEAM_USERNAME: "test_builder" });
        assert.notEqual(result.status, 0);
        assert.match(result.stderr, /windows package is missing or outdated/);
        assert.doesNotMatch(result.stderr, /Cannot start SteamCMD/);
    }
    fs.rmSync(file);
    assert.throws(() => prepareSteamUpload(f), /windows package is missing or outdated/);
    assert.equal(fs.existsSync(path.join(f.steamRoot, "uploads")), false);
});

test("unsafe/default branches, invalid env and absent IDs are rejected", (t) => {
    for (const branch of ["default", "Default", "public", "none", "", "test beta", "x\"\nSetLive", "../internal", null]) {
        assert.throws(() => validateSteamBranch(branch));
    }
    const f = fixture(t, {});
    assert.throws(() => prepareSteamUpload(f), /at least one Steam Depot/);
    assert.throws(() => prepareSteamUpload({ ...f, environment: "../prd" }), /--env/);
    const config = JSON.parse(fs.readFileSync(path.join(f.root, "electron.config.json"), "utf8"));
    config.steam.appId = null;
    fs.writeFileSync(path.join(f.root, "electron.config.json"), JSON.stringify(config));
    assert.throws(() => prepareSteamUpload(f), /steam.appId/);
});

test("dry-run CLI needs no SteamCMD, credentials, Vite or tool downloads", (t) => {
    const f = fixture(t);
    const result = cli(f, ["--dry-run"], { npm_config_cache: path.join(f.root, "empty-cache"), npm_config_offline: "true" });
    assert.equal(result.status, 0, result.stdout + result.stderr);
    assert.match(result.stdout, /SteamCMD was not started/);
    assert.match(result.stdout, /beta branch internal/);
    assert.equal(fs.existsSync(path.join(f.root, "empty-cache")), false);
    assert.equal(fs.existsSync(path.join(f.root, "node_modules")), false);
});

test("dry-run CLI preserves a custom build comment in its manifest, plan and output", (t) => {
    const f = fixture(t);
    const comment = "内部テスト：プレイヤー's 入力を修正 (revision abc123)";
    const result = cli(f, ["--dry-run", "--steam-comment", comment]);
    assert.equal(result.status, 0, result.stdout + result.stderr);
    assert.ok(result.stdout.includes(`Comment: ${comment}`));
    const manifest = result.stdout.match(/^Manifest: (.+)$/m)[1].trim();
    assert.ok(fs.readFileSync(manifest, "utf8").includes(`"Desc" "${comment}"`));
    const plan = JSON.parse(fs.readFileSync(path.join(path.dirname(manifest), "upload-plan.json"), "utf8"));
    assert.equal(plan.comment, comment);
});

test("invalid comments fail before writing manifests or starting SteamCMD", (t) => {
    const f = fixture(t);
    for (const comment of ["", "   ", "bad\"comment", "bad\\comment", "bad\ncomment", "bad\rcomment", "bad\tcomment", "bad\0comment", null]) {
        assert.throws(() => prepareSteamUpload({ ...f, comment }), /--steam-comment/);
    }
    const result = cli(f, ["--steam-comment", "   "], { STEAM_USERNAME: "test_builder" });
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /--steam-comment/);
    assert.doesNotMatch(result.stderr, /Cannot start SteamCMD/);
    assert.equal(fs.existsSync(path.join(f.steamRoot, "uploads")), false);
});

test("upload requires a cached-login account and propagates missing executable errors", (t) => {
    const f = fixture(t);
    assert.match(cli(f).stderr, /Set STEAM_USERNAME/);
    assert.match(cli(f, [], { STEAM_USERNAME: "user +quit" }).stderr, /Set STEAM_USERNAME/);
    assert.match(cli(f, [], { STEAM_USERNAME: "test_builder" }).stderr, /Cannot start SteamCMD/);
});

test("SteamCMD receives literal paths, runs once, saves BuildID, and propagates exit/output failures", { skip: process.platform === "win32" }, (t) => {
    const f = fixture(t);
    const command = path.join(f.root, "fake steamcmd");
    const calls = path.join(f.root, "calls.json");
    fs.writeFileSync(command, `#!/usr/bin/env node
const fs = require('node:fs');
fs.writeFileSync(process.env.TEST_CALLS, JSON.stringify(process.argv.slice(2)));
console.log(process.env.TEST_OUTPUT);
process.exit(Number(process.env.TEST_CODE || 0));
`, { mode: 0o755 });
    const env = { STEAM_USERNAME: "test_builder", STEAMCMD: command, TEST_CALLS: calls,
        TEST_OUTPUT: "Success! App '2409460' fully built (BuildID 123456)." };
    const result = cli(f, [], env);
    assert.equal(result.status, 0, result.stdout + result.stderr);
    const args = JSON.parse(fs.readFileSync(calls, "utf8"));
    assert.deepEqual(args.slice(0, 7), ["+@ShutdownOnFailedCommand", "1", "+@NoPromptForPassword", "1", "+login", "test_builder", "+run_app_build"]);
    assert.equal(args.length, 9);
    assert.equal(args[8], "+quit");
    assert.ok(fs.existsSync(args[7]));
    assert.equal(JSON.parse(fs.readFileSync(path.join(path.dirname(args[7]), "upload-result.json"), "utf8")).buildId, "123456");
    // Actual SteamCMD output includes a timestamp and uses a different completion message.
    const currentOutput = "[2026-09-24 08:02:22]: Successfully finished AppID 2409460 build (BuildID 25493101).\r\nUnloading Steam API...OK";
    const comment = "内部テスト用ビルド";
    const current = cli(f, ["--steam-comment", comment], { ...env, TEST_OUTPUT: currentOutput });
    assert.equal(current.status, 0, current.stdout + current.stderr);
    const currentArgs = JSON.parse(fs.readFileSync(calls, "utf8"));
    assert.ok(fs.readFileSync(currentArgs[7], "utf8").includes(`"Desc" "${comment}"`));
    const currentRecord = JSON.parse(fs.readFileSync(path.join(path.dirname(currentArgs[7]), "upload-result.json"), "utf8"));
    assert.equal(currentRecord.buildId, "25493101");
    assert.equal(currentRecord.requestedBranch, "internal");
    for (const failure of [{ TEST_CODE: "7" }, { TEST_OUTPUT: "ERROR! Failed to build App." },
        { TEST_OUTPUT: "Logged in but no build happened" }, { TEST_OUTPUT: env.TEST_OUTPUT + "\nFailed to set build live" },
        { TEST_OUTPUT: currentOutput, TEST_CODE: "7" },
        { TEST_OUTPUT: currentOutput.replace("AppID 2409460", "AppID 480") },
        { TEST_OUTPUT: currentOutput.replace("BuildID 25493101", "BuildID unknown") },
        { TEST_OUTPUT: currentOutput + "\nERROR! Failed to set build live" },
        { TEST_OUTPUT: "ERROR! Failed to build depot.\n" + currentOutput }]) {
        const failed = cli(f, [], { ...env, ...failure });
        assert.notEqual(failed.status, 0, failed.stdout + failed.stderr);
        const failedArgs = JSON.parse(fs.readFileSync(calls, "utf8"));
        assert.equal(fs.existsSync(path.join(path.dirname(failedArgs[7]), "upload-result.json")), false);
    }
});
