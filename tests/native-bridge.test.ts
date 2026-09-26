import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { createRequire } from "node:module";
import { spawnSync } from "node:child_process";
import { readElectronConfig, createElectronPackagerOptions } from "../dist/electron-config.js";
import { withElectronHost } from "../dist/electron-host.js";
const require = createRequire(import.meta.url);
const { NativeBridge, isTrustedSender } = require("../templates/electron/native-bridge.cjs");

test("SDK-independent macOS example compiles and implements the native protocol", { skip: process.platform !== "darwin" }, async (t) => {
    if (spawnSync("xcrun", ["--find", "swiftc"]).status !== 0) {
        t.skip("Xcode Command Line Tools are required for the Swift example"); return;
    }
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), "next2d-native-example-"));
    t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
    const executable = path.join(directory, "native-helper");
    const compiled = spawnSync("xcrun", ["swiftc", "examples/native-bridge/macos/main.swift", "-o", executable],
        { encoding: "utf8", timeout: 60000 });
    assert.equal(compiled.status, 0, compiled.stderr);
    const events: Array<{ event: string }> = [];
    const bridge = new NativeBridge(executable, ["system.info", "unknown.method"], (event: { event: string }) => events.push(event));
    t.after(() => bridge.close());
    const info = await bridge.request("system.info", {});
    assert.equal(info.platform, "macos");
    assert.ok(info.logicalProcessors > 0);
    assert.ok(info.uptimeSeconds >= 0);
    assert.equal(events[0].event, "system.ready");
    await assert.rejects(bridge.request("unknown.method", null), /Unsupported method/);
    await assert.rejects(bridge.request("system.info", { unexpected: true }), /empty parameters/);
    assert.equal((await bridge.request("system.info", null)).platform, "macos");
    const exited = new Promise<number | null>((resolve) => bridge.child.once("exit", resolve));
    bridge.close();
    assert.equal(await exited, 0);
});

test("native bundle stays outside web assets and ASAR; requires matching architecture", async (t) => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "next2d-native-test-"));
    t.after(() => fs.rmSync(root, { recursive: true, force: true }));
    fs.writeFileSync(path.join(root, "package.json"), JSON.stringify({ name: "native-test", version: "1.0.0" }));
    fs.mkdirSync(path.join(root, "web"));
    fs.writeFileSync(path.join(root, "web/index.html"), "game");
    fs.mkdirSync(path.join(root, "bundle"));
    fs.writeFileSync(path.join(root, "bundle/sidecar"), "fixture");
    fs.writeFileSync(path.join(root, "electron.config.json"), JSON.stringify({ nativeBridge: {
        methods: ["system.info"], targets: { "macos-arm64": { directory: "bundle", executable: "sidecar" } }
    } }));
    const config = readElectronConfig(root);
    await withElectronHost(root, path.join(root, "web"), config, async (dir) => {
        assert.ok(fs.existsSync(path.join(dir, "native/sidecar")));
        assert.equal(fs.existsSync(path.join(dir, "resources/native")), false);
        const runtime = JSON.parse(fs.readFileSync(path.join(dir, "runtime-config.json"), "utf8"));
        assert.deepEqual(runtime.nativeBridge, { methods: ["system.info"], executable: "sidecar" });
        const options = createElectronPackagerOptions(root, config, "macos", path.join(dir, "resources"));
        assert.ok(options.extraResource.includes(path.join(dir, "native")));
        assert.ok(options.ignore.some((rule) => rule.test("/native/sidecar")));
    }, "macos-arm64");
    await assert.rejects(withElectronHost(root, path.join(root, "web"), config, async () => {}, "macos-universal"), /requires a macos-universal/);
    config.nativeBridge!.backgroundThrottling = true;
    await withElectronHost(root, path.join(root, "web"), config, async (dir) => {
        const runtime = JSON.parse(fs.readFileSync(path.join(dir, "runtime-config.json"), "utf8"));
        assert.equal(runtime.nativeBridge.backgroundThrottling, true);
    }, "macos-arm64");
    for (const nativeBridge of [null, { methods: ["system.info"], targets: {}, backgroundThrottling: "false" }, { methods: ["../exec"], targets: {} }, {
        methods: ["connect"], targets: { "macos-arm64": { directory: "bundle", executable: "../sidecar" } }
    }]) {
        fs.writeFileSync(path.join(root, "electron.config.json"), JSON.stringify({ nativeBridge }));
        assert.throws(() => readElectronConfig(root), /Invalid nativeBridge/);
    }
});

const script = `
require('node:readline').createInterface({input:process.stdin}).on('line', line => {
 const m=JSON.parse(line);
 if(m.method==='hang')return;
 if(m.method==='crash')process.exit(1);
 if(m.method==='bad'){process.stdout.write('invalid\\n');return;}
 if(m.method==='large'){process.stdout.write('x'.repeat(262145));return;}
 process.stdout.write(JSON.stringify({event:'connected',data:true})+'\\n');
 process.stdout.write(JSON.stringify({id:m.id,result:m.params})+'\\n');
});`;

test("native child round trips, events, allowlist, size limit and timeout", async (t) => {
    const events = [];
    const bridge = new NativeBridge(process.execPath, ["echo", "hang"], (event) => events.push(event), {
        args: ["-e", script], timeout: 500
    });
    t.after(() => bridge.close());
    assert.deepEqual(await bridge.request("echo", { text: "新宿" }), { text: "新宿" });
    assert.equal(events[0].event, "connected");
    await assert.rejects(bridge.request("exec", {}), /not allowed/);
    await assert.rejects(bridge.request("echo", "x".repeat(262144)), /too large/);
    await assert.rejects(bridge.request("hang", null), /timed out/);
    assert.equal(bridge.pending.size, 0);
    bridge.close();
    await assert.rejects(bridge.request("echo", null), /closed/);
});

test("native crash and invalid output reject pending requests", async () => {
    for (const method of ["crash", "bad", "large"]) {
        const bridge = new NativeBridge(process.execPath, [method], () => {}, { args: ["-e", script] });
        try {
            await assert.rejects(bridge.request(method, null), /exited|protocol|too large/);
            assert.equal(bridge.pending.size, 0);
        } finally { bridge.close(); }
    }
    const bridge = new NativeBridge(path.join(os.tmpdir(), "missing-next2d-executable"), ["echo"], () => {});
    await assert.rejects(bridge.request("echo", null), /start|closed/);
    bridge.close();
});

test("native IPC rejects foreign windows, child frames and remote origins", () => {
    const frame = { url: "next2d://game/index.html" };
    const sender = { mainFrame: frame };
    const window = { isDestroyed: () => false, webContents: sender };
    assert.equal(isTrustedSender({ sender, senderFrame: frame }, window), true);
    assert.equal(isTrustedSender({ sender: {}, senderFrame: frame }, window), false);
    assert.equal(isTrustedSender({ sender, senderFrame: { ...frame } }, window), false);
    frame.url = "https://game/index.html";
    assert.equal(isTrustedSender({ sender, senderFrame: frame }, window), false);
});
