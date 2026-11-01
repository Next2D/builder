import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import Module, { createRequire } from "node:module";
import { readElectronConfig } from "../dist/electron-config.js";
import { withElectronHost } from "../dist/electron-host.js";
const require = createRequire(import.meta.url);
const templates = path.resolve(import.meta.dirname, "../templates/electron");

const fixture = (t, electron) => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "next2d overlay "));
    t.after(() => fs.rmSync(root, { recursive: true, force: true }));
    fs.writeFileSync(path.join(root, "package.json"), JSON.stringify({ name: "test-game", version: "1.0.0" }));
    if (electron !== undefined) fs.writeFileSync(path.join(root, "electron.config.json"), JSON.stringify(electron));
    return root;
};

/** Loads a template file with a fake "electron" module; returns what it touched. */
const loadWithElectron = (t, file, electron) => {
    const load = Module._load;
    Module._load = function (request, ...rest) {
        return request === "electron" ? electron : load.call(this, request, ...rest);
    };
    t.after(() => { Module._load = load; });
    delete require.cache[file];
    require(file);
};

const fakeElectron = (options = {}) => {
    const listeners = new Map();
    const handlers = new Map();
    const calls = [];
    const powerListeners = new Map();
    const appListeners = new Map();
    const windows = [];
    let ready;
    const display = { workArea: { x: 0, y: 25, width: 1440, height: 875 }, workAreaSize: { width: 1440, height: 875 }, scaleFactor: 2 };
    class BrowserWindow {
        constructor(options) {
            this.options = options;
            this.bounds = { x: options.x ?? 0, y: options.y ?? 0, width: options.width, height: options.height };
            this.webContents = { executeJavaScript: async (script) => { calls.push(["executeJavaScript", script.slice(0, 20)]); return "{\"moves\":1}"; }, reloadIgnoringCache: () => calls.push(["reloadIgnoringCache"]), setWindowOpenHandler() {}, on: (name, cb) => calls.push(["webContents.on", name, cb]), send: (...args) => calls.push(["send", ...args]), mainFrame: {} };
            windows.push(this);
        }
        setAlwaysOnTop(...args) { calls.push(["setAlwaysOnTop", ...args]); }
        setVisibleOnAllWorkspaces(...args) { calls.push(["setVisibleOnAllWorkspaces", ...args]); }
        setIgnoreMouseEvents(...args) { calls.push(["setIgnoreMouseEvents", ...args]); }
        setBounds(bounds) { this.bounds = bounds; }
        getBounds() { return this.bounds; }
        setFullScreen(value) { calls.push(["setFullScreen", value]); }
        isFullScreen() { return false; }
        isDestroyed() { return false; }
        isVisible() { return true; }
        isAlwaysOnTop() { return true; }
        moveTop() { calls.push(["moveTop"]); }
        showInactive() { calls.push(["showInactive"]); }
        once(name, cb) { this.onceListeners = { ...this.onceListeners, [name]: cb }; }
        async loadURL() {}
    }
    const electron = {
        app: {
            isPackaged: false, commandLine: { getSwitchValue: () => "", hasSwitch: () => false }, setName() {}, setPath() {},
            requestSingleInstanceLock: () => { calls.push(["requestSingleInstanceLock"]); return options.lock !== false; },
            dock: { hide: () => calls.push(["dock.hide"]), show: () => calls.push(["dock.show"]) },
            getPath: () => os.tmpdir(), on: (name, cb) => appListeners.set(name, cb), quit: () => calls.push(["quit"]), exit: (code) => calls.push(["exit", code]),
            whenReady: () => new Promise((resolve) => { ready = resolve; })
        },
        BrowserWindow,
        Menu: { setApplicationMenu() {}, buildFromTemplate: () => ({}) },
        powerMonitor: { on: (name, cb) => powerListeners.set(name, cb) },
        net: {}, protocol: { registerSchemesAsPrivileged() {}, handle() {} },
        screen: {
            getPrimaryDisplay: () => display, getDisplayMatching: () => display,
            on: (name, cb) => listeners.set(name, cb), removeListener: (name) => listeners.delete(name)
        },
        session: { defaultSession: { setPermissionRequestHandler() {}, setPermissionCheckHandler: (cb) => calls.push(["permissionCheck", cb]) } },
        ipcMain: { handle: (name, cb) => handlers.set(name, cb) }
    };
    return { electron, listeners, powerListeners, appListeners, handlers, calls, windows, display, start: async () => { ready(); await new Promise((r) => setImmediate(r)); } };
};

const startHost = async (t, config, options = {}) => {
    const root = fixture(t, config);
    fs.writeFileSync(path.join(root, "index.html"), "hello");
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "next2d overlay host "));
    t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
    await withElectronHost(root, root, readElectronConfig(root), async (host) => fs.cpSync(host, dir, { recursive: true }));
    const fake = fakeElectron(options);
    loadWithElectron(t, path.join(dir, "index.js"), fake.electron);
    await fake.start();
    return { ...fake, runtime: JSON.parse(fs.readFileSync(path.join(dir, "runtime-config.json"), "utf8")) };
};

test("overlay defaults and validation", (t) => {
    assert.equal(readElectronConfig(fixture(t)).window.overlay, undefined);
    assert.deepEqual(readElectronConfig(fixture(t, { window: { fullscreen: true, overlay: {} } })).window,
        { width: 1280, height: 720, fullscreen: true, overlay: { enabled: false, anchor: "bottom", clickThrough: false } });
    const enabled = readElectronConfig(fixture(t, { window: { fullscreen: true, overlay: { enabled: true, clickThrough: true } } })).window;
    assert.deepEqual(enabled, { width: 1280, height: 720, fullscreen: false, overlay: { enabled: true, anchor: "bottom", clickThrough: true } });
    for (const [overlay, message] of [
        [null, /window.overlay must be an object/], [true, /must be an object/], [[], /must be an object/],
        [{ enabled: "yes" }, /enabled\/clickThrough must be booleans/], [{ clickThrough: 1 }, /must be booleans/],
        [{ anchor: "top" }, /anchor must be "bottom"/]
    ]) {
        assert.throws(() => readElectronConfig(fixture(t, { window: { overlay } })), message);
    }
});

test("existing window behaviour is unchanged without an enabled overlay", async (t) => {
    for (const config of [{}, { window: { overlay: { enabled: false, clickThrough: true } } }]) {
        const { windows, calls, handlers, listeners, powerListeners } = await startHost(t, config);
        const { options } = windows[0];
        assert.equal(options.transparent, undefined);
        assert.equal(options.frame, undefined);
        assert.equal(options.backgroundColor, "#000000");
        assert.equal(options.webPreferences.preload, undefined);
        assert.equal(options.webPreferences.sandbox, true);
        assert.equal(calls.some(([name]) => /^set(AlwaysOnTop|IgnoreMouseEvents)$/.test(name)), false);
        assert.equal(handlers.size, 0);
        assert.equal(listeners.size, 0);
        assert.equal(powerListeners.size, 0);
        const check = calls.find(([name]) => name === "permissionCheck")[1];
        assert.equal(check(null, "fullscreen"), true);
    }
});

test("overlay window is transparent, anchored, always on top and click-through", async (t) => {
    const { windows, calls, handlers, listeners, display, runtime } = await startHost(t, {
        window: { width: 1920, height: 420, overlay: { enabled: true, anchor: "bottom", clickThrough: true } }
    });
    assert.deepEqual(runtime.window.overlay, { enabled: true, anchor: "bottom", clickThrough: true });
    const [window] = windows;
    const { options } = window;
    assert.deepEqual({ ...options, webPreferences: undefined, title: undefined, icon: undefined }, {
        ...options, webPreferences: undefined, title: undefined, icon: undefined,
        x: 0, y: 480, width: 1440, height: 420, fullscreen: false, fullscreenable: false,
        transparent: true, frame: false, resizable: false, hasShadow: false, skipTaskbar: false, backgroundColor: "#00000000"
    });
    assert.equal(options.webPreferences.sandbox, true);
    assert.equal(options.webPreferences.contextIsolation, true);
    assert.equal(options.webPreferences.nodeIntegration, false);
    assert.deepEqual(options.webPreferences.additionalArguments, ["--next2d-overlay"]);
    assert.ok(options.webPreferences.preload.endsWith("preload.cjs"));
    assert.deepEqual(calls.find(([name]) => name === "setAlwaysOnTop"), ["setAlwaysOnTop", true, "floating"]);
    assert.deepEqual(calls.find(([name]) => name === "setIgnoreMouseEvents"), ["setIgnoreMouseEvents", true, { forward: true }]);
    assert.equal(calls.some(([name]) => name === "setVisibleOnAllWorkspaces"), process.platform === "darwin");
    assert.equal(calls.find(([name]) => name === "permissionCheck")[1](null, "fullscreen"), false);

    const keys = calls.find(([name, event]) => name === "webContents.on" && event === "before-input-event")[2];
    let prevented = false;
    keys({ preventDefault: () => { prevented = true; } }, { type: "keyDown", key: "F11" });
    keys({ preventDefault: () => { prevented = true; } }, { type: "keyDown", key: "Enter", alt: true });
    assert.equal(prevented, false);
    assert.equal(calls.some(([name]) => name === "setFullScreen"), false);

    display.workArea = { x: 1440, y: 0, width: 1024, height: 728 };
    for (const name of ["display-metrics-changed", "display-added", "display-removed"]) assert.ok(listeners.has(name));
    listeners.get("display-metrics-changed")();
    assert.deepEqual(window.bounds, { x: 1440, y: 308, width: 1024, height: 420 });

    const trusted = { sender: window.webContents, senderFrame: Object.assign(window.webContents.mainFrame, { url: "next2d://game/index.html" }) };
    const untrusted = { sender: {}, senderFrame: { url: "https://example.com/" } };
    for (const name of ["next2d:window:set-ignore-mouse-events", "next2d:window:get-bounds", "next2d:window:quit"]) {
        assert.throws(() => handlers.get(name)(untrusted, true), /access denied/);
    }
    const setIgnore = handlers.get("next2d:window:set-ignore-mouse-events");
    for (const invalid of [undefined, 1, "false", {}]) assert.throws(() => setIgnore(trusted, invalid), /boolean/);
    setIgnore(trusted, false);
    assert.deepEqual(calls.at(-1), ["setIgnoreMouseEvents", false, undefined]);
    assert.deepEqual(handlers.get("next2d:window:get-bounds")(trusted), { x: 1440, y: 308, width: 1024, height: 420, scaleFactor: 2 });
    handlers.get("next2d:window:quit")(trusted);
    await new Promise((resolve) => setImmediate(resolve));
    assert.deepEqual(calls.at(-1), ["quit"]);
});

test("preload exposes each renderer API only for its configured feature", (t) => {
    for (const [flags, expected] of [
        [[], []], [["--next2d-native-bridge"], ["next2dNative"]], [["--next2d-overlay"], ["next2dWindow"]],
        [["--next2d-native-bridge", "--next2d-overlay"], ["next2dNative", "next2dWindow"]]
    ]) {
        const exposed = {};
        const invoked = [];
        const argv = process.argv;
        process.argv = [...argv, ...flags];
        try {
            loadWithElectron(t, path.join(templates, "preload.cjs"), {
                contextBridge: { exposeInMainWorld: (key, api) => { exposed[key] = api; } },
                ipcRenderer: { invoke: async (...args) => { invoked.push(args); return { x: 1 }; }, on() {}, removeListener() {} }
            });
        } finally {
            process.argv = argv;
        }
        assert.deepEqual(Object.keys(exposed), expected);
        if (exposed.next2dWindow) {
            t.test(`next2dWindow ${flags.join(" ")}`, async () => {
                await assert.rejects(exposed.next2dWindow.setIgnoreMouseEvents("true"), TypeError);
                assert.equal(await exposed.next2dWindow.setIgnoreMouseEvents(true), undefined);
                assert.deepEqual(await exposed.next2dWindow.getBounds(), { x: 1 });
                assert.equal(await exposed.next2dWindow.quit(), undefined);
                assert.equal(typeof exposed.next2dWindow.onResume(() => {}), "function");
                assert.throws(() => exposed.next2dWindow.onResume("x"), TypeError);
                assert.deepEqual(invoked, [["next2d:window:set-ignore-mouse-events", true], ["next2d:window:get-bounds"], ["next2d:window:quit"]]);
            });
        }
    }
});

test("overlay is applied again and the page is told after sleep or unlock", async (t) => {
    t.mock.timers.enable({ apis: ["setTimeout"] });
    const { powerListeners, handlers, calls, windows } = await startHost(t, { window: { overlay: { enabled: true, clickThrough: true } } });
    assert.deepEqual([...powerListeners.keys()].sort(), ["lock-screen", "resume", "suspend", "unlock-screen"]);
    // The page last asked for "capture clicks" (the pointer is over the pet), then the PC slept.
    const trusted = { sender: windows[0].webContents, senderFrame: Object.assign(windows[0].webContents.mainFrame, { url: "next2d://game/index.html" }) };
    handlers.get("next2d:window:set-ignore-mouse-events")(trusted, false);
    calls.length = 0;
    powerListeners.get("unlock-screen")();
    t.mock.timers.tick(1000);
    assert.deepEqual(calls.filter(([name]) => /^(setAlwaysOnTop|setIgnoreMouseEvents|send)$/.test(name)), [
        ["setAlwaysOnTop", true, "floating"],
        ["setIgnoreMouseEvents", false],
        ["setIgnoreMouseEvents", false, undefined],
        ["send", "next2d:window:resume"]
    ]);
});

test("a desktop overlay runs once: a second launch quits before any window or Dock icon", async (t) => {
    const denied = await startHost(t, { window: { overlay: { enabled: true, clickThrough: true } } }, { lock: false });
    assert.equal(denied.windows.length, 0);
    assert.deepEqual(denied.calls.filter(([name]) => /^(requestSingleInstanceLock|exit|dock\.hide)$/.test(name)),
        process.platform === "darwin"
            ? [["requestSingleInstanceLock"], ["dock.hide"], ["exit", 0]]
            : [["requestSingleInstanceLock"], ["exit", 0]]);

    const first = await startHost(t, { window: { overlay: { enabled: true, clickThrough: true } } });
    assert.equal(first.windows.length, 1);
    assert.equal(first.calls.some(([name]) => name === "exit"), false);
    // The overlay is an agent app: no instance shows a Dock icon.
    assert.equal(first.calls.some(([name]) => name === "dock.show"), false);
    assert.equal(denied.calls.some(([name]) => name === "dock.show"), false);
    first.calls.length = 0;
    first.appListeners.get("second-instance")();
    assert.deepEqual(first.calls.filter(([name]) => name === "setAlwaysOnTop"), [["setAlwaysOnTop", true, "floating"]]);
});

test("a game without the overlay is not limited to one instance", async (t) => {
    const { calls, appListeners } = await startHost(t, {});
    assert.equal(calls.some(([name]) => name === "requestSingleInstanceLock"), false);
    assert.equal(appListeners.has("second-instance"), false);
});

test("on Windows the overlay is put back on top every few seconds and when the displays change", async (t) => {
    t.mock.timers.enable({ apis: ["setInterval", "setTimeout"] });
    const platform = Object.getOwnPropertyDescriptor(process, "platform");
    Object.defineProperty(process, "platform", { value: "win32" });
    t.after(() => Object.defineProperty(process, "platform", platform));
    const { calls, listeners, handlers, windows } = await startHost(t, { window: { overlay: { enabled: true, clickThrough: true } } });
    calls.length = 0;
    t.mock.timers.tick(3000);
    assert.deepEqual(calls.filter(([name]) => /^(setAlwaysOnTop|moveTop)$/.test(name)), [["setAlwaysOnTop", true, "floating"], ["moveTop"]]);
    // Click-through is on: the mouse-forwarding hook is installed again (off, then on).
    assert.deepEqual(calls.filter(([name]) => name === "setIgnoreMouseEvents"), [["setIgnoreMouseEvents", false], ["setIgnoreMouseEvents", true, { forward: true }]]);
    // While the page captures the mouse (pointer over the pet) nothing is toggled under it.
    handlers.get("next2d:window:set-ignore-mouse-events")({ sender: windows[0].webContents, senderFrame: Object.assign(windows[0].webContents.mainFrame, { url: "next2d://game/index.html" }) }, false);
    calls.length = 0;
    t.mock.timers.tick(3000);
    assert.equal(calls.some(([name]) => name === "setIgnoreMouseEvents"), false);
    calls.length = 0;
    listeners.get("display-added")();
    assert.deepEqual(calls.filter(([name]) => /^(setAlwaysOnTop|moveTop)$/.test(name)), [["setAlwaysOnTop", true, "floating"], ["moveTop"]]);
});

test("on other platforms there is no periodic raise", async (t) => {
    t.mock.timers.enable({ apis: ["setInterval", "setTimeout"] });
    if (process.platform === "win32") return;
    const { calls } = await startHost(t, { window: { overlay: { enabled: true, clickThrough: true } } });
    calls.length = 0;
    t.mock.timers.tick(10000);
    assert.equal(calls.some(([name]) => name === "moveTop"), false);
});

test("after sleep the renderer is loaded again (the GPU may have been reset); unlocking the screen does not", async (t) => {
    t.mock.timers.enable({ apis: ["setTimeout"] });
    const diagnostics = path.join(os.tmpdir(), "diagnostics.log");
    fs.rmSync(diagnostics, { force: true });
    t.after(() => fs.rmSync(diagnostics, { force: true }));
    const { powerListeners, calls } = await startHost(t, { window: { overlay: { enabled: true, clickThrough: true } } });
    assert.deepEqual([...powerListeners.keys()].sort(), ["lock-screen", "resume", "suspend", "unlock-screen"]);
    calls.length = 0;
    powerListeners.get("unlock-screen")();
    t.mock.timers.tick(1000);
    assert.equal(calls.some(([name]) => name === "reloadIgnoringCache"), false);
    powerListeners.get("suspend")();
    powerListeners.get("resume")();
    t.mock.timers.tick(1000);
    assert.equal(calls.filter(([name]) => name === "reloadIgnoringCache").length, 1);
    // Click-through starts again from the configured state after the page was reloaded.
    assert.deepEqual(calls.filter(([name]) => name === "setIgnoreMouseEvents").at(-1), ["setIgnoreMouseEvents", true, { forward: true }]);
    const log = fs.readFileSync(diagnostics, "utf8");
    for (const word of ["unlock-screen", "suspend", "resume", "reload renderer (resume)"]) assert.match(log, new RegExp(word.replace(/[()]/g, "\\$&")));
});

test("the page's mouse event counts are written to the diagnostics log when they change", async (t) => {
    t.mock.timers.enable({ apis: ["setInterval", "setTimeout"] });
    const diagnostics = path.join(os.tmpdir(), "diagnostics.log");
    fs.rmSync(diagnostics, { force: true });
    t.after(() => fs.rmSync(diagnostics, { force: true }));
    const { calls } = await startHost(t, { window: { overlay: { enabled: true, clickThrough: true } } });
    const loaded = calls.find(([name, event]) => name === "webContents.on" && event === "did-finish-load")[2];
    loaded();
    assert.ok(calls.some(([name, script]) => name === "executeJavaScript" && /__n2dDiag|\(\(\) =>/.test(script)));
    t.mock.timers.tick(5000);
    await new Promise((resolve) => setImmediate(resolve));
    t.mock.timers.tick(5000);
    await new Promise((resolve) => setImmediate(resolve));
    const log = fs.readFileSync(diagnostics, "utf8");
    assert.match(log, /renderer loaded/);
    assert.equal((log.match(/renderer \{"moves":1\} ignore=true onTop=true/g) ?? []).length, 1);
});

test("game errors and denied window requests are written to the diagnostics log", async (t) => {
    const diagnostics = path.join(os.tmpdir(), "diagnostics.log");
    fs.rmSync(diagnostics, { force: true });
    t.after(() => fs.rmSync(diagnostics, { force: true }));
    const { calls, handlers } = await startHost(t, { window: { overlay: { enabled: true, clickThrough: true } } });
    const consoleMessage = calls.find(([name, event]) => name === "webContents.on" && event === "console-message")[2];
    consoleMessage({ level: "error", message: "Error: boom" });
    consoleMessage({ level: "info", message: "ignored" });
    consoleMessage({}, 3, "legacy error");
    assert.throws(() => handlers.get("next2d:window:set-ignore-mouse-events")({ sender: {}, senderFrame: { url: "https://example.com/" } }, true), /access denied/);
    const log = fs.readFileSync(diagnostics, "utf8");
    assert.match(log, /console error: Error: boom/);
    assert.doesNotMatch(log, /ignored/);
    assert.match(log, /console 3: legacy error/);
    assert.match(log, /window API access denied \(frame url: https:\/\/example.com\/\)/);
});
