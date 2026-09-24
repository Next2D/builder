import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { readElectronConfig } from "../dist/electron-config.js";
import { readSteamConfig, recordSteamPackage, writeSharedSteamDepots } from "../dist/steam.js";
import { parseArgv } from "../dist/cli.js";

const fixture = (t, depots = { windows: 2409461, macos: "2409461", linux: 2409461 }) => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "next2d shared depot "));
    t.after(() => fs.rmSync(root, { recursive: true, force: true }));
    fs.writeFileSync(path.join(root, "package.json"), JSON.stringify({ name: "test-game", version: "1.0.0" }));
    fs.writeFileSync(path.join(root, "electron.config.json"), JSON.stringify({
        appName: "My Game", executableName: "my-game", steam: { appId: 2409460, depots }
    }));
    return { root, steamRoot: path.join(root, "custom output/steam"), config: readElectronConfig(root) };
};

const addPackage = (f, os, arch = os === "macos" ? "universal" : "x64", version = "1.0.0") => {
    const native = { windows: "win32", macos: "darwin", linux: "linux" }[os];
    const content = path.join(f.steamRoot, os, "build/prd", `My Game-${native}-${arch}`);
    fs.mkdirSync(content, { recursive: true });
    const exe = os === "macos" ? "My Game.app" : os === "windows" ? "my-game.exe" : "my-game";
    if (os === "macos") fs.mkdirSync(path.join(content, exe));
    else fs.writeFileSync(path.join(content, exe), "binary", { mode: 0o755 });
    fs.writeFileSync(path.join(content, "resources.pak"), os);
    recordSteamPackage(content, `steam:${os}`, arch, f.config, version);
    return content;
};

const scriptDir = (f) => path.join(f.steamRoot, "shared/prd/depot-2409461");

test("one OS with a shared Depot ID exports metadata but no partial upload VDF", (t) => {
    const f = fixture(t);
    assert.deepEqual(readSteamConfig(f.config, "steam:macos").sharedPlatforms, ["windows", "macos", "linux"]);
    const content = addPackage(f, "macos");
    assert.equal(fs.existsSync(`${content}-steampipe/app_build.vdf`), false);
    const launch = JSON.parse(fs.readFileSync(`${content}-steampipe/launch.json`, "utf8"));
    assert.equal(launch.executable, "macos/My Game.app");
    assert.equal(launch.localExecutable, "My Game.app");
    const pending = writeSharedSteamDepots(f.steamRoot, "prd", f.config, "1.0.0");
    assert.equal(pending.length, 1);
    assert.equal(fs.existsSync(path.join(scriptDir(f), "app_build.vdf")), false);
    const status = JSON.parse(fs.readFileSync(path.join(scriptDir(f), "launch.json"), "utf8"));
    assert.equal(status.ready, false);
    assert.equal(status.missing.length, 2);
});

test("shared manifest maps every OS into separate directories and uses latest architecture", (t) => {
    const f = fixture(t);
    for (const os of ["windows", "macos", "linux"]) addPackage(f, os);
    const latest = addPackage(f, "macos", "arm64");
    assert.deepEqual(writeSharedSteamDepots(f.steamRoot, "prd", f.config, "1.0.0"), []);
    const dir = scriptDir(f);
    const depot = fs.readFileSync(path.join(dir, "depot_build.vdf"), "utf8");
    assert.equal((depot.match(/"FileMapping"/g) || []).length, 3);
    for (const os of ["windows", "macos", "linux"]) {
        assert.ok(depot.includes(`"DepotPath" "${os}/"`));
        assert.ok(depot.includes(`"LocalPath" "${os}/build/prd/My Game-`));
    }
    assert.ok(depot.includes(path.relative(f.steamRoot, latest).replaceAll("\\", "/") + "/*"));
    assert.doesNotMatch(depot, /darwin-universal/);
    assert.doesNotMatch(depot, /steampipe|steam-package\.json/);
    const app = fs.readFileSync(path.join(dir, "app_build.vdf"), "utf8");
    assert.match(app, /"ContentRoot" "\.\.\/\.\.\/\.\."/);
    assert.equal((app.match(/"2409461" "depot_build.vdf"/g) || []).length, 1);
    assert.doesNotMatch(app, /SetLive/);
    assert.match(fs.readFileSync(path.join(dir, "app_preview.vdf"), "utf8"), /"Preview" "1"/);
    const status = JSON.parse(fs.readFileSync(path.join(dir, "launch.json"), "utf8"));
    assert.equal(status.ready, true);
    assert.deepEqual(status.launches.map(x => x.executable), ["windows/my-game.exe", "macos/My Game.app", "linux/my-game"]);
});

test("missing or outdated packages invalidate previously ready manifests", (t) => {
    const f = fixture(t);
    for (const os of ["windows", "macos", "linux"]) addPackage(f, os);
    writeSharedSteamDepots(f.steamRoot, "prd", f.config, "1.0.0");
    assert.ok(fs.existsSync(path.join(scriptDir(f), "app_build.vdf")));
    assert.equal(writeSharedSteamDepots(f.steamRoot, "prd", f.config, "2.0.0").length, 1);
    assert.equal(fs.existsSync(path.join(scriptDir(f), "app_build.vdf")), false);
    writeSharedSteamDepots(f.steamRoot, "prd", f.config, "1.0.0");
    fs.rmSync(path.join(f.steamRoot, "windows/build/prd/steam-package.json"));
    assert.equal(writeSharedSteamDepots(f.steamRoot, "prd", f.config, "1.0.0").length, 1);
    assert.equal(fs.existsSync(path.join(scriptDir(f), "depot_build.vdf")), false);
});

test("mixed shared and separate depots remain independent; changing config removes obsolete VDFs", (t) => {
    const f = fixture(t, { windows: 2409461, macos: 2409461, linux: 2409462 });
    const windows = addPackage(f, "windows");
    addPackage(f, "macos");
    const linux = addPackage(f, "linux");
    fs.writeFileSync(`${windows}-steampipe/app_build.vdf`, "obsolete standalone upload");
    writeSharedSteamDepots(f.steamRoot, "prd", f.config, "1.0.0");
    assert.equal(fs.existsSync(`${windows}-steampipe/app_build.vdf`), false);
    assert.ok(fs.existsSync(`${linux}-steampipe/app_build.vdf`));
    const depot = fs.readFileSync(path.join(scriptDir(f), "depot_build.vdf"), "utf8");
    assert.doesNotMatch(depot, /linux/);
    f.config.steam.depots.macos = "2409463";
    writeSharedSteamDepots(f.steamRoot, "prd", f.config, "1.0.0");
    assert.equal(fs.existsSync(path.join(scriptDir(f), "app_build.vdf")), false);
});

test("manifest CLI requires a Steam target and cannot rebuild or preview", () => {
    const args = ["--platform", "steam:macos", "--env", "prd", "--steam-manifest"];
    assert.equal(parseArgv(args).steamManifest, true);
    assert.equal(parseArgv(args).hasHelp, false);
    for (const flag of ["--preview", "--build", "--open"]) assert.equal(parseArgv([...args, flag]).hasHelp, true);
    assert.equal(parseArgv([...args, "--arch", "arm64"]).hasHelp, true);
    assert.equal(parseArgv(["--platform", "web", "--env", "prd", "--steam-manifest"]).hasHelp, true);
});

test("legacy standalone VDFs are removed even without a package pointer", (t) => {
    const f = fixture(t);
    const scripts = path.join(f.steamRoot, "windows/build/prd/My Game-win32-x64-steampipe");
    fs.mkdirSync(scripts, { recursive: true });
    fs.writeFileSync(path.join(scripts, "app_build.vdf"), "old upload");
    writeSharedSteamDepots(f.steamRoot, "prd", f.config, "1.0.0");
    assert.equal(fs.existsSync(path.join(scripts, "app_build.vdf")), false);
});

test("manifest CLI assembles relocated CI packages without a web build, and fails on incomplete input", async (t) => {
    const { spawnSync } = await import("node:child_process");
    const { fileURLToPath } = await import("node:url");
    const f = fixture(t);
    for (const os of ["windows", "macos", "linux"]) addPackage(f, os);
    const destination = path.join(f.root, "collected/steam");
    fs.mkdirSync(path.dirname(destination));
    fs.renameSync(f.steamRoot, destination);
    const vite = path.join(f.root, "node_modules/vite");
    fs.mkdirSync(vite, { recursive: true });
    fs.writeFileSync(path.join(vite, "package.json"), JSON.stringify({ name: "vite", type: "module", main: "index.js" }));
    fs.writeFileSync(path.join(vite, "index.js"), 'export const loadConfigFromFile = async () => ({ config: { build: { outDir: "collected" } } });');
    const run = () => spawnSync(process.execPath, [fileURLToPath(new URL("../dist/index.js", import.meta.url)),
        "--platform", "steam:macos", "--env", "prd", "--steam-manifest"], {
        cwd: f.root, encoding: "utf8", timeout: 10000, env: { ...process.env, npm_config_offline: "true" }
    });
    let result = run();
    assert.equal(result.status, 0, result.stderr);
    assert.doesNotMatch(result.stdout, /Packaging Electron|vite .*building/);
    const script = path.join(destination, "shared/prd/depot-2409461/app_build.vdf");
    assert.ok(fs.existsSync(script));
    assert.doesNotMatch(fs.readFileSync(script, "utf8"), /custom output/);
    fs.rmSync(path.join(destination, "linux/build/prd/steam-package.json"));
    result = run();
    assert.equal(result.status, 1);
    assert.match(result.stderr, /Shared Steam depots are incomplete/);
    assert.equal(fs.existsSync(script), false);
});
