import { test } from "node:test";
import assert from "node:assert/strict";
import crypto from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { createRequire } from "node:module";
import { createIntegrityManifest } from "../dist/integrity.js";
import { withElectronHost } from "../dist/electron-host.js";
import { readElectronConfig } from "../dist/electron-config.js";
const require = createRequire(import.meta.url);
const { verifyAsset } = require("../templates/electron/local-assets.cjs");

const fixture = (t) => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "next2d integrity "));
    t.after(() => fs.rmSync(root, { recursive: true, force: true }));
    fs.mkdirSync(path.join(root, "web/assets/js"), { recursive: true });
    fs.writeFileSync(path.join(root, "web/index.html"), "<html></html>");
    fs.writeFileSync(path.join(root, "web/assets/js/app.js"), "console.log(1);");
    fs.writeFileSync(path.join(root, "package.json"), JSON.stringify({ name: "test-game", version: "1.0.0" }));
    return root;
};

test("the manifest lists every file with its SHA-256, keyed by a forward-slash relative path", (t) => {
    const root = fixture(t);
    const manifest = createIntegrityManifest(path.join(root, "web"));
    assert.equal(manifest.algorithm, "sha256");
    assert.deepEqual(Object.keys(manifest.files), ["assets/js/app.js", "index.html"]);
    assert.equal(manifest.files["assets/js/app.js"],
        crypto.createHash("sha256").update("console.log(1);").digest("hex"));
});

test("the Electron host accepts only files that match the manifest", (t) => {
    const root = fixture(t);
    const web = path.join(root, "web");
    const manifest = createIntegrityManifest(web);
    const file = path.join(web, "assets/js/app.js");
    assert.equal(verifyAsset(manifest, web, file, Buffer.from("console.log(1);")), true);
    // changed content
    assert.equal(verifyAsset(manifest, web, file, Buffer.from("console.log(2);")), false);
    // a file added after the build
    assert.equal(verifyAsset(manifest, web, path.join(web, "assets/js/extra.js"), Buffer.from("x")), false);
    // names that exist on Object.prototype are not files
    assert.equal(verifyAsset(manifest, web, path.join(web, "constructor"), Buffer.from("")), false);
});

test("the generated Electron host carries the manifest of the game files it ships", async (t) => {
    const root = fixture(t);
    const web = path.join(root, "web");
    await withElectronHost(root, web, readElectronConfig(root), async (dir) => {
        const manifest = JSON.parse(fs.readFileSync(path.join(dir, "integrity.json"), "utf8"));
        assert.deepEqual(manifest, createIntegrityManifest(path.join(dir, "resources")));
        // every shipped file is listed, including the ones the builder adds
        assert.ok(manifest.files["index.html"]);
        assert.ok(manifest.files["assets/js/app.js"]);
        assert.ok(manifest.files["desktop-icon.png"]);
        assert.match(fs.readFileSync(path.join(dir, "index.js"), "utf8"), /verifyAsset\(integrity, assetRoot, asset, data\)/);
    });
});

test("packaged Electron runtimes load only the validated ASAR and ignore switches that run other code", async () => {
    const { createElectronFuses } = await import("../dist/electron-config.js");
    for (const [platform, arch, validated, adHoc] of [
        ["win32", "x64", true, false], ["darwin", "arm64", true, true],
        ["darwin", "x64", true, false], ["linux", "x64", false, false]
    ]) {
        const fuses = createElectronFuses(platform, arch);
        assert.equal(fuses.resetAdHocDarwinSignature, adHoc);
        assert.deepEqual(fuses.options, {
            RunAsNode: false,
            EnableNodeOptionsEnvironmentVariable: false,
            EnableNodeCliInspectArguments: false,
            // Electron validates the ASAR on macOS and Windows only
            EnableEmbeddedAsarIntegrityValidation: validated,
            OnlyLoadAppFromAsar: true
        });
    }
});

test("developer tools are off unless the Electron config turns them on", async (t) => {
    const root = fixture(t);
    const web = path.join(root, "web");
    assert.equal(readElectronConfig(root).devTools, false);
    await withElectronHost(root, web, readElectronConfig(root), async (dir) => {
        assert.equal(JSON.parse(fs.readFileSync(path.join(dir, "runtime-config.json"), "utf8")).devTools, false);
        assert.match(fs.readFileSync(path.join(dir, "index.js"), "utf8"), /devTools: config\.devTools === true/);
    });
    fs.writeFileSync(path.join(root, "electron.config.json"), JSON.stringify({ devTools: true }));
    assert.equal(readElectronConfig(root).devTools, true);
    await withElectronHost(root, web, readElectronConfig(root), async (dir) => {
        assert.equal(JSON.parse(fs.readFileSync(path.join(dir, "runtime-config.json"), "utf8")).devTools, true);
    });
    fs.writeFileSync(path.join(root, "electron.config.json"), JSON.stringify({ devTools: "yes" }));
    assert.throws(() => readElectronConfig(root), /devTools must be a boolean/);
});

test("native hosts get one line per script file, and nothing for media", async () => {
    const { createIntegrityLines, renderIntegrityHeader, isCodeFile } = await import("../dist/integrity.js");
    for (const name of ["a.js", "a.MJS", "index.html", "data/config.json", "x.wasm"]) assert.equal(isCodeFile(name), true, name);
    for (const name of ["bgm.mp3", "a.png", "font.ttf", "js", "a.js.map"]) assert.equal(isCodeFile(name), false, name);
    const lines = createIntegrityLines([
        ["js/bootstrap.js", Buffer.from("boot")], ["assets/bgm.mp3", Buffer.from("sound")], ["index.html", Buffer.from("<html>")]
    ]);
    assert.deepEqual(lines, [
        `${crypto.createHash("sha256").update("boot").digest("hex")}  js/bootstrap.js`,
        `${crypto.createHash("sha256").update("<html>").digest("hex")}  index.html`
    ].sort());
    assert.throws(() => createIntegrityLines([["a\n.js", Buffer.from("")]]), /line break/);
    const header = renderIntegrityHeader([`${"0".repeat(64)}  a "quoted" \\ name.js`]);
    assert.match(header, /inline const char\* const kAssetIntegrityLines\[\] = \{/);
    assert.ok(header.includes(`    "${"0".repeat(64)}  a \\"quoted\\" \\\\ name.js",`));
    assert.ok(header.includes("    nullptr\n};"));
});
