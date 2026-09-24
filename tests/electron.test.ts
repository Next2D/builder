import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { createRequire } from "node:module";
import { resolveElectronTarget } from "../dist/electron.js";
import { withElectronHost, getElectronVersion } from "../dist/electron-host.js";
import { readElectronConfig, createElectronPackagerOptions } from "../dist/electron-config.js";
import { readSteamConfig, writeSteamMetadata } from "../dist/steam.js";
const require = createRequire(import.meta.url);
const { resolveAssetPath } = require("../templates/electron/local-assets.cjs");

const fixture = (t) => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "next2d electron "));
    t.after(() => fs.rmSync(root, { recursive: true, force: true }));
    fs.writeFileSync(path.join(root, "package.json"), JSON.stringify({ name: "test-game", version: "1.2.3" }));
    return root;
};

test("Steam targets use darwin, universal macOS and x64 Windows/Linux", () => {
    assert.deepEqual(resolveElectronTarget("steam:macos"), { platform: "darwin", arch: "universal" });
    assert.deepEqual(resolveElectronTarget("steam:windows"), { platform: "win32", arch: "x64" });
    assert.deepEqual(resolveElectronTarget("steam:linux"), { platform: "linux", arch: "x64" });
    assert.throws(() => resolveElectronTarget("steam:linux", "universal"));
    assert.throws(() => resolveElectronTarget("steam:windows", "all"));
    assert.throws(() => resolveElectronTarget("mas"));
    const foreign = process.platform === "win32" ? "linux" : "windows";
    assert.throws(() => resolveElectronTarget(foreign, "", true));
});

test("temporary host uses current metadata and assets without creating a game electron directory", async (t) => {
    const root = fixture(t);
    const source = path.join(root, "web");
    fs.mkdirSync(source);
    fs.writeFileSync(path.join(source, "index.html"), "hello");
    const config = readElectronConfig(root);
    let generated;
    const result = await withElectronHost(root, source, config, async (dir) => {
        generated = dir;
        const pkg = JSON.parse(fs.readFileSync(path.join(dir, "package.json"), "utf8"));
        assert.equal(pkg.version, "1.2.3");
        assert.equal(pkg.name, "test-game");
        assert.equal(pkg.dependencies, undefined);
        assert.equal(pkg.devDependencies, undefined);
        assert.equal(fs.readFileSync(path.join(dir, "resources/index.html"), "utf8"), "hello");
        const runtime = JSON.parse(fs.readFileSync(path.join(dir, "runtime-config.json"), "utf8"));
        assert.equal(runtime.appId, config.appId);
        assert.ok(fs.existsSync(path.join(dir, "resources", runtime.icon)));
        assert.match(fs.readFileSync(path.join(dir, "index.js"), "utf8"), /sandbox: true/);
        assert.equal(fs.existsSync(path.join(dir, "node_modules")), false);
        const options = createElectronPackagerOptions(root, config, "windows", path.join(dir, "resources"));
        assert.deepEqual(options.extraResource, [path.join(dir, "resources")]);
        return "packaged";
    });
    assert.equal(result, "packaged");
    assert.equal(fs.existsSync(generated), false);
    assert.equal(fs.existsSync(path.join(root, "electron")), false);
    assert.match(getElectronVersion(), /^\d+\.\d+\.\d+$/);
});

test("temporary host is removed on failure and separate builds never reuse host files", async (t) => {
    const root = fixture(t);
    const config = readElectronConfig(root);
    await assert.rejects(withElectronHost(root, root, config, async () => assert.fail("must not be called")), /entry point is missing/);
    fs.writeFileSync(path.join(root, "index.html"), "hello");
    const generated = [];
    await assert.rejects(withElectronHost(root, root, config, async (first) => {
        generated.push(first);
        return withElectronHost(root, root, config, async (second) => {
            generated.push(second);
            assert.notEqual(first, second);
            assert.ok(fs.existsSync(first));
            throw new Error("packaging failed");
        });
    }), /packaging failed/);
    for (const dir of generated) {
        assert.equal(fs.existsSync(dir), false);
    }
    assert.deepEqual(fs.readdirSync(root).sort(), ["index.html", "package.json"]);
});

test("JSON metadata drives packaging; missing Depot IDs still allow export", (t) => {
    const root = fixture(t);
    fs.writeFileSync(path.join(root, "electron.config.json"), JSON.stringify({
        appId: "app.test.game", appName: "My Game", executableName: "my-game",
        steam: { appId: 2409460, depots: { windows: null } }
    }));
    const config = readElectronConfig(root);
    const options = createElectronPackagerOptions(root, config, "windows", path.join(root, "resources"));
    assert.equal(options.name, "My Game");
    assert.equal(options.appBundleId, "app.test.game");
    assert.equal(options.executableName, "my-game");
    assert.deepEqual(readSteamConfig(config, "steam:windows"), { appId: "2409460", depotId: null });
});

test("Electron host and Windows description prefer config and fall back to package metadata", async (t) => {
    const root = fixture(t);
    const web = path.join(root, "web");
    fs.mkdirSync(web);
    fs.writeFileSync(path.join(web, "index.html"), "game");
    for (const [override, fallback, expected] of [
        ["Desktop description", "Package description", "Desktop description"],
        [undefined, "Package description", "Package description"],
        [null, "Package description", "Package description"],
        ["", "Package description", ""],
        [undefined, undefined, ""]
    ]) {
        fs.writeFileSync(path.join(root, "package.json"), JSON.stringify({ name: "test-game", version: "1.2.3", description: fallback }));
        const source = JSON.stringify({ description: override });
        fs.writeFileSync(path.join(root, "electron.config.json"), source);
        const config = readElectronConfig(root);
        await withElectronHost(root, web, config, async (dir) => {
            assert.equal(JSON.parse(fs.readFileSync(path.join(dir, "package.json"))).description, expected);
            assert.equal(createElectronPackagerOptions(root, config, "windows", web).win32metadata.FileDescription, expected);
        });
        assert.equal(fs.readFileSync(path.join(root, "electron.config.json"), "utf8"), source);
    }
    fs.writeFileSync(path.join(root, "electron.config.json"), '{"description":123}');
    assert.throws(() => readElectronConfig(root), /description must be a string/);
});

test("invalid IDs, invalid icons and unsafe names fail", (t) => {
    const root = fixture(t);
    for (const input of [
        { steam: { appId: 0 } },
        { steam: { appId: null, depots: { windows: 123 } } },
        { icons: { windows: "missing.ico" } },
        { appName: "../escape" },
        { architectures: { linux: "universal" } },
        { macos: { notarize: true, sign: false } }
    ]) {
        fs.writeFileSync(path.join(root, "electron.config.json"), JSON.stringify(input));
        assert.throws(() => readElectronConfig(root));
    }
});

test("unassigned Steam IDs in templates allow local packaging", (t) => {
    const root = fixture(t);
    for (const appId of [null, undefined]) {
        fs.writeFileSync(path.join(root, "electron.config.json"), JSON.stringify({
            steam: { appId, depots: { windows: null, macos: null, linux: null } }
        }));
        const config = readElectronConfig(root);
        assert.equal(readSteamConfig(config, "steam:windows"), null);
        assert.equal(createElectronPackagerOptions(root, config, "windows", path.join(root, "resources")).name, "test-game");
    }
});

test("SteamPipe maps package contents to install root and excludes development AppID", (t) => {
    const root = fixture(t);
    for (const [platform, arch, executable] of [
        ["windows", "x64", "test-game.exe"], ["linux", "x64", "test-game"], ["macos", "universal", "My Game.app"]
    ]) {
        const content = path.join(root, platform);
        fs.mkdirSync(content);
        if (platform === "macos") {
            fs.mkdirSync(path.join(content, executable));
        } else {
            fs.writeFileSync(path.join(content, executable), "binary", { mode: 0o755 });
        }
        const scripts = `${content}-steampipe`;
        // Match the builder's configured executableName; Windows cannot preserve POSIX execute bits.
        writeSteamMetadata(content, `steam:${platform}`, arch, { appId: "2409460", depotId: "1234" }, "test-game");
        const launch = JSON.parse(fs.readFileSync(path.join(scripts, "launch.json"), "utf8"));
        assert.equal(launch.executable, executable);
        assert.equal(path.resolve(scripts, launch.contentRoot), content);
        const depot = fs.readFileSync(path.join(scripts, "depot_build.vdf"), "utf8");
        assert.match(depot, /"Recursive" "1"/);
        assert.match(depot, /"FileExclusion" "steam_appid.txt"/);
        const app = fs.readFileSync(path.join(scripts, "app_build.vdf"), "utf8");
        assert.match(app, /"Preview" "0"/);
        assert.doesNotMatch(app, /SetLive/);
        assert.match(fs.readFileSync(path.join(scripts, "app_preview.vdf"), "utf8"), /"Preview" "1"/);
        writeSteamMetadata(content, `steam:${platform}`, arch, { appId: "2409460", depotId: null }, "test-game");
        assert.equal(fs.existsSync(path.join(scripts, "app_build.vdf")), false);
    }
});

test("Linux launch metadata uses the configured name even without POSIX execute permissions", (t) => {
    const root = fixture(t);
    const content = path.join(root, "linux");
    fs.mkdirSync(content);
    // Reproduce permissions seen on Windows without requiring a Windows test runner.
    fs.writeFileSync(path.join(content, "custom-game"), "binary", { mode: 0o644 });
    fs.writeFileSync(path.join(content, "resources.pak"), "resources");
    writeSteamMetadata(content, "steam:linux", "x64", { appId: "2409460", depotId: "1234" }, "custom-game");
    const launch = JSON.parse(fs.readFileSync(`${content}-steampipe/launch.json`, "utf8"));
    assert.equal(launch.executable, "custom-game");
    assert.equal(path.resolve(`${content}-steampipe`, launch.contentRoot), content);
    assert.throws(() => writeSteamMetadata(content, "steam:linux", "x64", null, "missing-game"), /Packaged Steam executable is missing/);
});

test("local protocol rejects foreign origins, encoded traversal and malformed paths", () => {
    const root = path.resolve(os.tmpdir(), "game/resources");
    assert.equal(resolveAssetPath(root, "next2d://game/assets/test%20file.js"), path.join(root, "assets/test file.js"));
    assert.equal(resolveAssetPath(root, "next2d://game/"), path.join(root, "index.html"));
    for (const url of [
        "https://game/index.html", "next2d://other/index.html", "next2d://user@game/index.html",
        "next2d://game/%2e%2e%2fsecret", "next2d://game/%5c..%5csecret", "next2d://game/%00", "next2d://game/%ZZ"
    ]) {
        assert.equal(resolveAssetPath(root, url), null, url);
    }
});

test("release mode requires signing and notarization credentials", (t) => {
    const root = fixture(t);
    const previous = { ...process.env };
    t.after(() => { process.env = previous; });
    process.env.NEXT2D_STEAM_RELEASE = "1";
    delete process.env.APPLE_SIGNING_IDENTITY;
    delete process.env.APPLE_NOTARY_PROFILE;
    const config = readElectronConfig(root);
    assert.throws(() => createElectronPackagerOptions(root, config, "macos", path.join(root, "resources")), /APPLE_SIGNING_IDENTITY/);
    process.env.APPLE_SIGNING_IDENTITY = "test identity";
    assert.throws(() => createElectronPackagerOptions(root, config, "macos", path.join(root, "resources")), /APPLE_NOTARY_PROFILE/);
    process.env.APPLE_NOTARY_PROFILE = "test profile";
    const options = createElectronPackagerOptions(root, config, "macos", path.join(root, "resources"));
    assert.ok(options.osxSign);
    assert.equal(options.osxSign.continueOnError, false);
    assert.deepEqual(options.osxNotarize, { keychainProfile: "test profile" });
});

test("configured macOS signing must fail the build with or without notarization", (t) => {
    const root = fixture(t);
    const previous = { ...process.env };
    t.after(() => { process.env = previous; });
    delete process.env.NEXT2D_STEAM_RELEASE;
    process.env.APPLE_SIGNING_IDENTITY = "test identity";
    process.env.APPLE_NOTARY_PROFILE = "test profile";
    for (const notarize of [false, true]) {
        fs.writeFileSync(path.join(root, "electron.config.json"), JSON.stringify({ macos: { sign: true, notarize } }));
        const config = readElectronConfig(root);
        const options = createElectronPackagerOptions(root, config, "macos", path.join(root, "resources"));
        assert.equal(options.osxSign?.continueOnError, false);
        assert.equal(Boolean(options.osxNotarize), notarize);
        // Signing settings must not affect other target operating systems.
        for (const os of ["windows", "linux"] as const) {
            const other = createElectronPackagerOptions(root, config, os, path.join(root, "resources"));
            assert.equal(other.osxSign, undefined);
            assert.equal(other.osxNotarize, undefined);
        }
    }
});
