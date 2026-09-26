import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { withElectronHost } from "../dist/electron-host.js";
import { readElectronConfig } from "../dist/electron-config.js";

test("project native preparation stages files, keeps runtime clean and cleans up failures", async (t) => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "native preparation "));
    t.after(() => fs.rmSync(root, { recursive: true, force: true }));
    fs.writeFileSync(path.join(root, "package.json"), JSON.stringify({ name: "fixture", version: "1.0.0" }));
    fs.mkdirSync(path.join(root, "web")); fs.writeFileSync(path.join(root, "web/index.html"), "test");
    fs.writeFileSync(path.join(root, "prepare.cjs"), `const fs = require('node:fs'), path = require('node:path');
const dest = process.argv[process.argv.indexOf('--destination') + 1];
fs.writeFileSync(path.join(dest, 'helper'), 'prepared');
fs.writeFileSync(path.join(dest, 'config.json'), '{}');`);
    fs.writeFileSync(path.join(root, "electron.config.json"), JSON.stringify({ nativeBridge: {
        prepare: "prepare.cjs", methods: ["system.info"], targets: { "linux-x64": { directory: "not-yet-built", executable: "helper" } }
    } }));
    const config = readElectronConfig(root);
    let host = "";
    await withElectronHost(root, path.join(root, "web"), config, async (directory) => {
        host = directory;
        assert.equal(fs.readFileSync(path.join(directory, "native/helper"), "utf8"), "prepared");
        const runtime = JSON.parse(fs.readFileSync(path.join(directory, "runtime-config.json"), "utf8"));
        assert.equal(runtime.nativeBridge.prepare, undefined);
        assert.equal(fs.existsSync(path.join(directory, "resources/native")), false);
    }, "linux-x64");
    assert.equal(fs.existsSync(host), false);
    fs.writeFileSync(path.join(root, "prepare.cjs"), "process.exit(7)");
    await assert.rejects(withElectronHost(root, path.join(root, "web"), config, async () => assert.fail("must not package"), "linux-x64"), /preparation failed/);
    config.nativeBridge.prepare = "../outside.cjs";
    await assert.rejects(withElectronHost(root, path.join(root, "web"), config, async () => {}, "linux-x64"));
});
