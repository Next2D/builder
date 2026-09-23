import { before, test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { getCapacitorCommand } from "../dist/native.js";

// Allow a cold npm cache to download SDKs before the short per-command timeouts.
before(async () => { await getCapacitorCommand(); }, { timeout: 180000 });

const fixture = (t, platform) => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "next2d native "));
    t.after(() => fs.rmSync(root, { recursive: true, force: true }));
    fs.writeFileSync(path.join(root, "package.json"), JSON.stringify({ name: "test-game", version: "1.0.0" }));
    fs.writeFileSync(path.join(root, "capacitor.config.json"), JSON.stringify({
        appId: "app.example.game", appName: "Test Game", webDir: "obsolete",
        [platform]: { path: `native/${platform}` }
    }));
    const web = path.join(root, "custom output", platform, "prd");
    fs.mkdirSync(web, { recursive: true });
    fs.writeFileSync(path.join(web, "index.html"), "<html><head></head><body>Game content</body></html>");
    return root;
};

const run = (root, platform, action = "syncNative") => spawnSync(process.execPath, ["--input-type=module", "-e", `
    import { ctx } from ${JSON.stringify(new URL("../dist/context.js", import.meta.url).href)};
    import { ${action} } from ${JSON.stringify(new URL("../dist/native.js", import.meta.url).href)};
    Object.assign(ctx, { platform: ${JSON.stringify(platform)}, platformDir: ${JSON.stringify(platform)}, environment: "prd", outDir: "custom output" });
    await ${action}();
`], {
    cwd: root, encoding: "utf8", timeout: 30000,
    // Test add/sync only. Capacitor's optional Gradle discovery must not start a native build.
    env: { ...process.env, JAVA_HOME: path.join(root, "no-jdk") }
});

const succeeded = (result) => assert.equal(result.status, 0, result.stdout + result.stderr);

test("Android add/sync uses builder SDKs with no game Capacitor installation and keeps plugins/hooks", (t) => {
    const root = fixture(t, "android");
    const plugin = path.join(root, "node_modules/test-plugin");
    fs.mkdirSync(path.join(plugin, "android/src/main"), { recursive: true });
    fs.writeFileSync(path.join(plugin, "package.json"), JSON.stringify({
        name: "test-plugin", version: "1.0.0", capacitor: { android: { src: "android" } }
    }));
    fs.writeFileSync(path.join(plugin, "android/build.gradle"), "// Test plugin");
    const packageFile = path.join(root, "package.json");
    const pkg = JSON.parse(fs.readFileSync(packageFile, "utf8"));
    pkg.dependencies = { "test-plugin": "1.0.0" };
    pkg.scripts = { "capacitor:sync:before": "node hook.cjs" };
    fs.writeFileSync(packageFile, JSON.stringify(pkg));
    fs.writeFileSync(path.join(root, "hook.cjs"), 'require("node:fs").writeFileSync("hook-cwd.txt", process.cwd());');
    const originalPackage = fs.readFileSync(packageFile, "utf8");
    succeeded(run(root, "android"));
    assert.equal(fs.realpathSync(fs.readFileSync(path.join(root, "hook-cwd.txt"), "utf8")), fs.realpathSync(root));
    const native = path.join(root, "native/android");
    const settings = fs.readFileSync(path.join(native, "capacitor.settings.gradle"), "utf8");
    assert.match(settings, /include ':capacitor-android'/);
    assert.match(settings, /include ':test-plugin'/);
    const sdkPath = settings.match(/project\(':capacitor-android'\)\.projectDir = new File\('([^']+)'\)/)[1];
    assert.ok(fs.existsSync(path.resolve(native, sdkPath, "build.gradle")));
    assert.match(fs.readFileSync(path.join(native, "app/src/main/assets/public/index.html"), "utf8"), /Game content/);
    assert.equal(JSON.parse(fs.readFileSync(path.join(root, "capacitor.config.json"))).webDir, "custom output/android/prd/");
    fs.writeFileSync(path.join(native, "custom-native.txt"), "keep");
    succeeded(run(root, "android"));
    assert.equal(fs.readFileSync(path.join(native, "custom-native.txt"), "utf8"), "keep");
    assert.equal(fs.readFileSync(packageFile, "utf8"), originalPackage);
    assert.equal(fs.existsSync(path.join(root, "node_modules/@capacitor")), false);
});

test("iOS SPM add/sync uses builder SDKs without touching game dependencies", { skip: process.platform !== "darwin" }, (t) => {
    const root = fixture(t, "ios");
    succeeded(run(root, "ios"));
    const native = path.join(root, "native/ios/App");
    assert.match(fs.readFileSync(path.join(native, "CapApp-SPM/Package.swift"), "utf8"), /capacitor-swift-pm/);
    assert.match(fs.readFileSync(path.join(native, "App/public/index.html"), "utf8"), /Game content/);
    fs.writeFileSync(path.join(native, "custom-native.txt"), "keep");
    succeeded(run(root, "ios"));
    assert.equal(fs.readFileSync(path.join(native, "custom-native.txt"), "utf8"), "keep");
    assert.equal(fs.existsSync(path.join(root, "node_modules")), false);
});

test("native CLI failures stop build/open and return a nonzero exit", (t) => {
    const root = fixture(t, "android");
    fs.rmSync(path.join(root, "custom output"), { recursive: true });
    for (const action of ["buildNative", "openNative", "runNative"]) {
        const result = run(root, "android", action);
        assert.notEqual(result.status, 0);
        assert.match(result.stderr, /Capacitor (?:sync|run|add) android failed/);
    }
});

test("builder CLI resolution is independent of the game and supplies NODE_PATH", async (t) => {
    const root = fixture(t, "android");
    const { cli, env } = await getCapacitorCommand();
    assert.ok(fs.existsSync(cli));
    const result = spawnSync(process.execPath, ["-e", `
        for (const name of ['core', 'ios', 'android']) {
            console.log(require.resolve('@capacitor/' + name + '/package.json', { paths: [process.cwd()] }));
        }
    `], { cwd: root, env, encoding: "utf8" });
    succeeded(result);
    assert.equal(result.stdout.trim().split("\n").length, 3);
});
