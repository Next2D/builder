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
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "next2d mini "));
    t.after(() => fs.rmSync(root, { recursive: true, force: true }));
    fs.writeFileSync(path.join(root, "package.json"), JSON.stringify({ name: "test-game", version: "1.0.0" }));
    if (electron !== undefined) fs.writeFileSync(path.join(root, "electron.config.json"), JSON.stringify(electron));
    return root;
};

const loadWithElectron = (t, file, electron) => {
    const load = Module._load;
    Module._load = function (request, ...rest) {
        return request === "electron" ? electron : load.call(this, request, ...rest);
    };
    t.after(() => { Module._load = load; });
    delete require.cache[file];
    require(file);
};

/** A fake Electron whose window keeps its state (bounds, fullscreen, flags) like the real one. */
const fakeElectron = (options = {}) => {
    const handlers = new Map();
    const calls = [];
    const windows = [];
    let ready;
    // A title bar of 28px: the outer size is the content size plus the frame.
    const FRAME = options.frame ?? 28;
    const display = { workArea: { x: 0, y: 25, width: 1440, height: 875 }, workAreaSize: { width: 1440, height: 875 }, scaleFactor: 2 };
    class BrowserWindow {
        constructor(windowOptions) {
            this.options = windowOptions;
            this.fullScreen = Boolean(windowOptions.fullscreen);
            this.maximized = false;
            this.flags = { resizable: true, maximizable: true, fullScreenable: true, alwaysOnTop: false };
            this.bounds = { x: 100, y: 80, width: windowOptions.width, height: windowOptions.height + FRAME };
            this.once_ = {};
            this.webContents = {
                executeJavaScript: async () => "null", setWindowOpenHandler() {},
                on: (name, cb) => calls.push(["webContents.on", name, cb]),
                removeAllListeners() {}, send: (...args) => calls.push(["send", ...args]), mainFrame: {}
            };
            windows.push(this);
        }
        isDestroyed() { return false; }
        isFullScreen() { return this.fullScreen; }
        setFullScreen(value) {
            calls.push(["setFullScreen", value]);
            this.fullScreen = value;
            // Like macOS: the change is reported a little later (after the animation).
            if (options.reportFullScreen !== false) setImmediate(() => this.once_[value ? "enter-full-screen" : "leave-full-screen"]?.());
        }
        isMaximized() { return this.maximized; }
        maximize() { calls.push(["maximize"]); this.maximized = true; }
        unmaximize() { calls.push(["unmaximize"]); this.maximized = false; }
        getBounds() { return { ...this.bounds }; }
        getNormalBounds() { return { ...this.bounds }; }
        setBounds(bounds) { calls.push(["setBounds", bounds]); this.bounds = { ...bounds }; }
        // In fullscreen the content fills the display; the normal bounds are kept for later.
        getContentSize() { return this.fullScreen ? [1440, 900] : [this.bounds.width, this.bounds.height - FRAME]; }
        setContentSize(width, height) {
            calls.push(["setContentSize", width, height, this.flags.resizable]);
            this.bounds = { ...this.bounds, width, height: height + FRAME };
        }
        getPosition() { return [this.bounds.x, this.bounds.y]; }
        setPosition(x, y) { calls.push(["setPosition", x, y]); this.bounds = { ...this.bounds, x, y }; }
        setResizable(value) { this.flags.resizable = value; }
        setMaximizable(value) { this.flags.maximizable = value; }
        setFullScreenable(value) { this.flags.fullScreenable = value; }
        setAlwaysOnTop(value, level) { calls.push(["setAlwaysOnTop", value, level]); this.flags.alwaysOnTop = value; }
        setVisibleOnAllWorkspaces(...args) { calls.push(["setVisibleOnAllWorkspaces", ...args]); }
        setIgnoreMouseEvents(...args) { calls.push(["setIgnoreMouseEvents", ...args]); }
        focus() { calls.push(["focus"]); }
        show() {}
        showInactive() {}
        once(name, cb) { this.once_[name] = cb; }
        async loadURL() {}
    }
    const electron = {
        app: {
            isPackaged: false, commandLine: { getSwitchValue: () => "", hasSwitch: () => false }, setName() {}, setPath() {},
            requestSingleInstanceLock: () => true, dock: { hide() {}, show() {} },
            getPath: () => os.tmpdir(), on() {}, quit: () => calls.push(["quit"]), exit: (code) => calls.push(["exit", code]),
            whenReady: () => new Promise((resolve) => { ready = resolve; })
        },
        BrowserWindow,
        Menu: { setApplicationMenu() {}, buildFromTemplate: () => ({}) },
        powerMonitor: { on() {} },
        net: {}, protocol: { registerSchemesAsPrivileged() {}, handle() {} },
        screen: { getPrimaryDisplay: () => display, getDisplayMatching: () => display, on() {}, removeListener() {} },
        session: { defaultSession: { setPermissionRequestHandler() {}, setPermissionCheckHandler() {} } },
        ipcMain: { handle: (name, cb) => handlers.set(name, cb) }
    };
    return { electron, handlers, calls, windows, display, start: async () => { ready(); await new Promise((r) => setImmediate(r)); } };
};

const startHost = async (t, config, options = {}) => {
    const root = fixture(t, config);
    fs.writeFileSync(path.join(root, "index.html"), "hello");
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "next2d mini host "));
    t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
    await withElectronHost(root, root, readElectronConfig(root), async (host) => fs.cpSync(host, dir, { recursive: true }));
    const fake = fakeElectron(options);
    loadWithElectron(t, path.join(dir, "index.js"), fake.electron);
    await fake.start();
    const window = fake.windows[0];
    const trusted = { sender: window.webContents, senderFrame: Object.assign(window.webContents.mainFrame, { url: "next2d://game/index.html" }) };
    return { ...fake, window, trusted, runtime: JSON.parse(fs.readFileSync(path.join(dir, "runtime-config.json"), "utf8")) };
};

test("mini window defaults and validation", (t) => {
    assert.equal(readElectronConfig(fixture(t)).window.mini, undefined);
    assert.deepEqual(readElectronConfig(fixture(t, { window: { mini: {} } })).window.mini,
        { width: 720, height: 180, anchor: "bottom-right", margin: 16, alwaysOnTop: true });
    assert.deepEqual(readElectronConfig(fixture(t, { window: { mini: { width: 480, anchor: "top-left", margin: 0, alwaysOnTop: false } } })).window.mini,
        { width: 480, height: 180, anchor: "top-left", margin: 0, alwaysOnTop: false });
    for (const [mini, message] of [
        [null, /window.mini must be an object/], [[], /must be an object/], [true, /must be an object/],
        [{ width: 0 }, /width\/height must be integers/], [{ height: 1.5 }, /width\/height must be integers/], [{ width: "720" }, /width\/height/],
        [{ anchor: "bottom" }, /anchor must be one of "bottom-right", "bottom-left", "top-right", "top-left"/],
        [{ margin: -1 }, /margin must be an integer/], [{ margin: 1001 }, /margin/],
        [{ alwaysOnTop: "yes" }, /alwaysOnTop must be a boolean/]
    ]) {
        assert.throws(() => readElectronConfig(fixture(t, { window: { mini } })), message);
    }
    assert.throws(() => readElectronConfig(fixture(t, { window: { mini: {}, overlay: { enabled: true } } })),
        /window.mini cannot be combined with window.overlay.enabled/);
    // A disabled overlay may stay in the file next to the mini window.
    assert.equal(readElectronConfig(fixture(t, { window: { mini: {}, overlay: { enabled: false } } })).window.mini.width, 720);
});

test("without window.mini nothing is exposed and the window behaves as before", async (t) => {
    const { window, handlers } = await startHost(t, { window: { width: 1920, height: 1080, fullscreen: true } });
    assert.equal(window.options.webPreferences.preload, undefined);
    assert.equal(handlers.size, 0);
});

test("the renderer gets the window mode API only for window.mini", async (t) => {
    const { window, handlers, runtime } = await startHost(t, { window: { width: 1280, height: 720, mini: {} } });
    assert.deepEqual(runtime.window.mini, { width: 720, height: 180, anchor: "bottom-right", margin: 16, alwaysOnTop: true });
    assert.deepEqual(window.options.webPreferences.additionalArguments, ["--next2d-window-mode"]);
    assert.equal(window.options.webPreferences.sandbox, true);
    assert.equal(window.options.webPreferences.contextIsolation, true);
    assert.equal(window.options.focusable, undefined);
    assert.deepEqual([...handlers.keys()].sort(),
        ["next2d:window:get-bounds", "next2d:window:get-mode", "next2d:window:quit", "next2d:window:set-mode"]);
    const untrusted = { sender: {}, senderFrame: { url: "https://example.com/" } };
    for (const name of handlers.keys()) assert.throws(() => handlers.get(name)(untrusted, "mini"), /access denied/);
});

test("full → mini → full keeps the window and puts it back where it was", async (t) => {
    const { window, handlers, trusted, calls } = await startHost(t, { window: { width: 1280, height: 720, fullscreen: true, mini: {} } });
    const setMode = handlers.get("next2d:window:set-mode");
    for (const invalid of [undefined, "small", 1, {}]) assert.throws(() => setMode(trusted, invalid), /mode must be "full" or "mini"/);
    assert.deepEqual(handlers.get("next2d:window:get-mode")(trusted), { mode: "full", width: 1440, height: 900 });
    assert.equal(window.isFullScreen(), true);
    const before = window.getNormalBounds();

    assert.deepEqual(await setMode(trusted, "mini"), { mode: "mini", width: 720, height: 180 });
    assert.equal(window.isFullScreen(), false);
    // Resized while still resizable, then locked.
    assert.deepEqual(calls.find(([name]) => name === "setContentSize"), ["setContentSize", 720, 180, true]);
    assert.deepEqual(window.flags, { resizable: false, maximizable: false, fullScreenable: false, alwaysOnTop: true });
    // Bottom-right of the work area, 16px in: x = 1440 - 720 - 16, y = 25 + 875 - (180 + 28) - 16.
    assert.deepEqual(window.getPosition(), [704, 676]);
    assert.deepEqual(calls.find(([name]) => name === "setAlwaysOnTop"), ["setAlwaysOnTop", true, "floating"]);
    assert.equal(calls.some(([name]) => name === "setVisibleOnAllWorkspaces"), process.platform === "darwin");
    assert.deepEqual(calls.filter(([name, channel]) => name === "send" && channel === "next2d:window:mode").at(-1),
        ["send", "next2d:window:mode", { mode: "mini", width: 720, height: 180 }]);
    // Asking for the current mode again changes nothing and sends nothing.
    const sent = calls.length;
    assert.deepEqual(await setMode(trusted, "mini"), { mode: "mini", width: 720, height: 180 });
    assert.equal(calls.length, sent);

    // Resolved once fullscreen is entered again, with the fullscreen size.
    assert.deepEqual(await setMode(trusted, "full"), { mode: "full", width: 1440, height: 900 });
    assert.deepEqual(window.getBounds(), before);
    assert.equal(window.isFullScreen(), true);
    assert.deepEqual(window.flags, { resizable: true, maximizable: true, fullScreenable: true, alwaysOnTop: false });
    assert.ok(calls.some(([name]) => name === "focus"));
});

test("the small window comes back where the player moved it", async (t) => {
    const { window, handlers, trusted } = await startHost(t, { window: { mini: { anchor: "top-left", margin: 8 } } });
    const setMode = handlers.get("next2d:window:set-mode");
    await setMode(trusted, "mini");
    assert.deepEqual(window.getPosition(), [8, 33]);
    window.setPosition(300, 400);
    await setMode(trusted, "full");
    await setMode(trusted, "mini");
    assert.deepEqual(window.getPosition(), [300, 400]);
});

test("a maximized window is restored maximized; alwaysOnTop false never raises it", async (t) => {
    const { window, handlers, trusted, calls } = await startHost(t, { window: { mini: { alwaysOnTop: false } } });
    window.maximize();
    const setMode = handlers.get("next2d:window:set-mode");
    await setMode(trusted, "mini");
    assert.equal(window.isMaximized(), false);
    assert.equal(calls.some(([name]) => name === "setAlwaysOnTop"), false);
    await setMode(trusted, "full");
    assert.equal(window.isMaximized(), true);
});

test("fullscreen changes do not wait forever when the OS never reports them", async (t) => {
    t.mock.timers.enable({ apis: ["setTimeout"] });
    const { window, handlers, trusted } = await startHost(t, { window: { fullscreen: true, mini: {} } }, { reportFullScreen: false });
    const setMode = handlers.get("next2d:window:set-mode");
    const toMini = setMode(trusted, "mini");
    await new Promise((resolve) => setImmediate(resolve));
    t.mock.timers.tick(1500);
    assert.deepEqual(await toMini, { mode: "mini", width: 720, height: 180 });
    assert.deepEqual(window.getContentSize(), [720, 180]);
    const toFull = setMode(trusted, "full");
    await new Promise((resolve) => setImmediate(resolve));
    t.mock.timers.tick(1500);
    assert.deepEqual(await toFull, { mode: "full", width: 1440, height: 900 });
});

test("changes are applied one after another", async (t) => {
    const { handlers, trusted, calls } = await startHost(t, { window: { fullscreen: true, mini: {} } });
    const setMode = handlers.get("next2d:window:set-mode");
    const results = await Promise.all([setMode(trusted, "mini"), setMode(trusted, "full"), setMode(trusted, "mini")]);
    assert.deepEqual(results.map(({ mode }) => mode), ["mini", "full", "mini"]);
    assert.deepEqual(calls.filter(([name, channel]) => name === "send" && channel === "next2d:window:mode").map(([, , info]) => info.mode),
        ["mini", "full", "mini"]);
});

test("F11 / Alt+Enter / Escape toggle fullscreen only in the full window", async (t) => {
    const { window, handlers, trusted, calls } = await startHost(t, { window: { mini: {} } });
    const keys = calls.find(([name, event]) => name === "webContents.on" && event === "before-input-event")[2];
    let prevented = 0;
    const press = (input) => keys({ preventDefault: () => { prevented++; } }, { type: "keyDown", ...input });
    press({ key: "F11" });
    assert.equal(window.isFullScreen(), true);
    assert.equal(prevented, 1);
    press({ key: "F11" });
    await handlers.get("next2d:window:set-mode")(trusted, "mini");
    prevented = 0;
    for (const input of [{ key: "F11" }, { key: "Enter", alt: true }, { key: "Escape" }]) press(input);
    assert.equal(prevented, 0);
    assert.equal(window.isFullScreen(), false);
});

test("get-bounds and quit work for the mini window", async (t) => {
    const { handlers, trusted, calls } = await startHost(t, { window: { mini: {} } });
    assert.deepEqual(handlers.get("next2d:window:get-bounds")(trusted), { x: 100, y: 80, width: 1280, height: 748, scaleFactor: 2 });
    handlers.get("next2d:window:quit")(trusted);
    await new Promise((resolve) => setImmediate(resolve));
    assert.deepEqual(calls.at(-1), ["quit"]);
});

test("preload exposes the mode API and checks its arguments", async (t) => {
    const exposed = {};
    const invoked = [];
    const listeners = new Map();
    const argv = process.argv;
    process.argv = [...argv, "--next2d-window-mode"];
    try {
        loadWithElectron(t, path.join(templates, "preload.cjs"), {
            contextBridge: { exposeInMainWorld: (key, api) => { exposed[key] = api; } },
            ipcRenderer: {
                invoke: async (...args) => { invoked.push(args); return { mode: "mini", width: 720, height: 180 }; },
                on: (name, cb) => listeners.set(name, cb), removeListener: (name) => listeners.delete(name)
            }
        });
    } finally {
        process.argv = argv;
    }
    assert.deepEqual(Object.keys(exposed), ["next2dWindow"]);
    const api = exposed.next2dWindow;
    assert.deepEqual(Object.keys(api).sort(), ["getBounds", "getMode", "onModeChange", "quit", "setMode"]);
    await assert.rejects(api.setMode("small"), TypeError);
    assert.deepEqual(await api.setMode("mini"), { mode: "mini", width: 720, height: 180 });
    await api.getMode();
    await api.getBounds();
    assert.equal(await api.quit(), undefined);
    assert.deepEqual(invoked, [["next2d:window:set-mode", "mini"], ["next2d:window:get-mode"], ["next2d:window:get-bounds"], ["next2d:window:quit"]]);
    assert.throws(() => api.onModeChange("x"), TypeError);
    const seen = [];
    const off = api.onModeChange((info) => seen.push(info));
    listeners.get("next2d:window:mode")({}, { mode: "full", width: 1280, height: 720 });
    assert.deepEqual(seen, [{ mode: "full", width: 1280, height: 720 }]);
    off();
    assert.equal(listeners.has("next2d:window:mode"), false);
});
