const { app, BrowserWindow, Menu, net, powerMonitor, protocol, screen, session, ipcMain } = require("electron");
const path = require("node:path");
const fs = require("node:fs");
const { pathToFileURL } = require("node:url");
const { resolveAssetPath, CSP } = require("./local-assets.cjs");
const config = require("./runtime-config.json");
const { NativeBridge, isTrustedSender } = require("./native-bridge.cjs");
let nativeBridge;
const closeNativeBridge = () => nativeBridge?.close();
process.on("exit", closeNativeBridge);
app.on("before-quit", closeNativeBridge);
// Keep saves in a stable user directory even if the visible game name changes.
app.setName(config.appName);
const userDataOverride = app.commandLine.getSwitchValue("user-data-dir");
app.setPath("userData", userDataOverride ? path.resolve(userDataOverride) : path.join(app.getPath("appData"), config.appId));

// A desktop overlay (a pet that stays on screen) must run once. A second launch quits at once (macOS: before
// it can show a Dock icon). The lock is tied to the userData path set above, so it needs to come after it.
const singleInstance = !app.commandLine.hasSwitch("next2d-allow-multiple") && Boolean(config.window.overlay?.enabled);
const hasLock = !singleInstance || app.requestSingleInstanceLock();
if (!hasLock) {
    if (process.platform === "darwin") app.dock?.hide();
    app.exit(0);
}
const assetRoot = app.isPackaged
    ? path.join(process.resourcesPath, "resources")
    : path.join(__dirname, "resources");

// A fixed secure origin supports fetch, module scripts, workers and localStorage.
protocol.registerSchemesAsPrivileged([{
    scheme: "next2d",
    privileges: { standard: true, secure: true, supportFetchAPI: true, corsEnabled: true, stream: true }
}]);

const startURL = "next2d://game/index.html";
const overlay = config.window.overlay?.enabled ? config.window.overlay : null;
let mainWindow;
// The last click-through state asked for, so it can be applied again after sleep/lock.
let overlayIgnore = Boolean(config.window.overlay?.enabled && config.window.overlay.clickThrough);
// Bottom anchor: flush with the primary display's work area (above the taskbar/Dock), centred.
const overlayBounds = () => {
    const { workArea } = screen.getPrimaryDisplay();
    const width = Math.min(config.window.width, workArea.width);
    const height = Math.min(config.window.height, workArea.height);
    return {
        x: workArea.x + Math.floor((workArea.width - width) / 2),
        y: workArea.y + workArea.height - height,
        width, height
    };
};
const placeOverlay = () => {
    if (mainWindow && !mainWindow.isDestroyed()) mainWindow.setBounds(overlayBounds());
};
// The overlay never takes focus, so Windows does not raise it when another topmost window (the taskbar, another
// always-on-top app) is activated, nor after the display is turned off and on. Put it back on top.
const keepOnTop = () => {
    if (!mainWindow || mainWindow.isDestroyed()) return;
    mainWindow.setAlwaysOnTop(true, "floating");
    mainWindow.moveTop();
};
// Windows forwards mouse moves to a click-through window with a system mouse hook, and Electron installs it only
// when forwarding switches on. After sleep (closing the lid) the hook can be gone while Electron still thinks it is
// installed, so the page never sees the pointer. Switching forwarding off and on again installs a fresh hook.
const refreshForwarding = () => {
    if (!overlayIgnore || !mainWindow || mainWindow.isDestroyed()) return;
    mainWindow.setIgnoreMouseEvents(false);
    mainWindow.setIgnoreMouseEvents(true, { forward: true });
};
// A small log of power/display/GPU events (userData/diagnostics.log) to find out what happened after sleep.
const diagnosticsFile = () => path.join(app.getPath("userData"), "diagnostics.log");
const diag = (message) => {
    if (!overlay) return;
    try {
        const file = diagnosticsFile();
        if (fs.existsSync(file) && fs.statSync(file).size > 200000) fs.writeFileSync(file, "");
        fs.appendFileSync(file, `${new Date().toISOString()} ${message}\n`);
    } catch { /* diagnostics must never break the game */ }
};
// After sleep (closing a laptop lid) the GPU is often reset. The renderer then keeps showing its last frame but can
// no longer draw (the web page is not told), so the pet looks alive and menus never appear. Load the page again to
// get a fresh GPU device. The game saves automatically, so progress is kept.
// Counts of mouse events the page really receives (moves are forwarded while click-through; down/up only arrive
// while the window accepts clicks). Logged when they change, to tell "clicks never reach the page" from other causes.
const DIAG_SCRIPT = `(() => {
    if (window.__n2dDiag) return;
    const d = window.__n2dDiag = { moves: 0, downs: 0, ups: 0, keys: 0, last: null };
    const mark = (name) => (event) => { d[name]++; d.last = [event.clientX, event.clientY]; };
    addEventListener("mousemove", mark("moves"), true);
    addEventListener("pointerdown", mark("downs"), true);
    addEventListener("pointerup", mark("ups"), true);
    addEventListener("keydown", () => { d.keys++; }, true);
})()`;
let lastSnapshot = "";
const snapshotRenderer = async () => {
    if (!mainWindow || mainWindow.isDestroyed()) return;
    try {
        const raw = await mainWindow.webContents.executeJavaScript("JSON.stringify(window.__n2dDiag || null)");
        if (raw !== lastSnapshot) {
            lastSnapshot = raw;
            diag(`renderer ${raw} ignore=${overlayIgnore} onTop=${mainWindow.isAlwaysOnTop()} bounds=${JSON.stringify(mainWindow.getBounds())}`);
        }
    } catch (error) {
        diag(`renderer snapshot failed: ${error?.message ?? error}`);
    }
};
const reloadRenderer = () => {
    if (!mainWindow || mainWindow.isDestroyed()) return;
    diag("reload renderer (resume)");
    overlayIgnore = Boolean(overlay?.clickThrough);
    mainWindow.setIgnoreMouseEvents(overlayIgnore, overlayIgnore ? { forward: true } : undefined);
    mainWindow.webContents.reloadIgnoringCache();
};
const createWindow = async () => {
    const { width, height } = screen.getPrimaryDisplay().workAreaSize;
    // The preload reads these flags; each API is exposed only when its feature is configured.
    const preloadFlags = [
        ...(config.nativeBridge ? ["--next2d-native-bridge"] : []),
        ...(overlay ? ["--next2d-overlay"] : [])
    ];
    mainWindow = new BrowserWindow({
        width: Math.min(config.window.width, width),
        height: Math.min(config.window.height, height),
        title: config.appName,
        fullscreen: overlay ? false : config.window.fullscreen,
        ...(config.icon ? { icon: path.join(assetRoot, config.icon) } : {}),
        useContentSize: true,
        backgroundColor: "#000000",
        autoHideMenuBar: true,
        show: false,
        ...(overlay ? {
            ...overlayBounds(), transparent: true, frame: false, resizable: false, hasShadow: false,
            fullscreenable: false, skipTaskbar: false, backgroundColor: "#00000000",
            // The overlay must never take keyboard focus or activation from the apps under it (Word etc.).
            focusable: false
        } : {}),
        webPreferences: {
            nodeIntegration: false, contextIsolation: true, sandbox: true,
            ...(preloadFlags.length ? { preload: path.join(__dirname, "preload.cjs"), additionalArguments: preloadFlags } : {}),
            ...(config.nativeBridge ? { backgroundThrottling: config.nativeBridge.backgroundThrottling ?? false } : {})
        }
    });
    if (overlay) {
        mainWindow.setAlwaysOnTop(true, "floating");
        if (process.platform === "darwin") {
            mainWindow.setVisibleOnAllWorkspaces(true, { visibleOnFullScreen: true });
        }
        if (overlay.clickThrough) {
            mainWindow.setIgnoreMouseEvents(true, { forward: true });
        }
        // Errors the game prints (a failed window request, for example) are otherwise invisible in a packaged app.
        let consoleLogged = 0;
        mainWindow.webContents.on("console-message", (...args) => {
            const first = args[0];
            const detail = first && typeof first === "object" && "message" in first ? first : { level: args[1], message: args[2] };
            if (!["error", "warning", 2, 3].includes(detail.level) || ++consoleLogged > 200) return;
            diag(`console ${detail.level}: ${String(detail.message).slice(0, 300)}`);
        });
        mainWindow.webContents.on("did-finish-load", () => {
            diag("renderer loaded");
            mainWindow.webContents.executeJavaScript(DIAG_SCRIPT).catch(() => {});
        });
        const snapshotTimer = setInterval(snapshotRenderer, 5000);
        snapshotTimer.unref?.();
        mainWindow.once("closed", () => clearInterval(snapshotTimer));
        mainWindow.webContents.on("unresponsive", () => diag("renderer unresponsive"));
        mainWindow.webContents.on("responsive", () => diag("renderer responsive"));
        const events = ["display-metrics-changed", "display-added", "display-removed"];
        const onDisplayChange = () => { diag("display changed"); placeOverlay(); keepOnTop(); };
        for (const name of events) screen.on(name, onDisplayChange);
        mainWindow.once("closed", () => {
            for (const name of events) screen.removeListener(name, onDisplayChange);
        });
        if (process.platform === "win32") {
            const timer = setInterval(() => { keepOnTop(); refreshForwarding(); }, 3000);
            timer.unref?.();
            mainWindow.once("closed", () => clearInterval(timer));
        }
    }
    mainWindow.webContents.setWindowOpenHandler(() => ({ action: "deny" }));
    mainWindow.webContents.on("will-navigate", (event, url) => {
        if (!resolveAssetPath(assetRoot, url)) {
            event.preventDefault();
        }
    });
    mainWindow.webContents.on("before-input-event", (event, input) => {
        if (input.type !== "keyDown" || overlay) {
            return;
        }
        if (input.key === "F11" || (input.alt && input.key === "Enter")) {
            mainWindow.setFullScreen(!mainWindow.isFullScreen());
            event.preventDefault();
        } else if (input.key === "Escape" && mainWindow.isFullScreen()) {
            mainWindow.setFullScreen(false);
            event.preventDefault();
        }
    });
    mainWindow.webContents.on("render-process-gone", (_event, details) => {
        console.error("Renderer exited:", details.reason);
        closeNativeBridge();
        app.exit(1);
    });
    mainWindow.once("ready-to-show", () => (overlay ? mainWindow.showInactive() : mainWindow.show()));
    await mainWindow.loadURL(startURL);
};

app.whenReady().then(async () => {
    if (!hasLock) return;
    // The overlay app is packaged as an agent app (LSUIElement): it never shows a Dock icon (a desktop pet stays out
    // of the Dock and the app switcher; the game offers its own "quit").
    Menu.setApplicationMenu(process.platform === "darwin" ? Menu.buildFromTemplate([
        { role: "appMenu" }, { role: "editMenu" }, { role: "windowMenu" }
    ]) : null);
    session.defaultSession.setPermissionRequestHandler((_contents, permission, callback) => {
        callback(!overlay && permission === "fullscreen");
    });
    session.defaultSession.setPermissionCheckHandler((_contents, permission) => !overlay && permission === "fullscreen");
    protocol.handle("next2d", async (request) => {
        let asset = resolveAssetPath(assetRoot, request.url);
        if (!asset || request.method !== "GET") {
            return new Response("Not found", { status: 404 });
        }
        // Framework SPA routes use history.pushState (e.g. /home); reload the entry.
        if (!path.extname(asset) && !fs.existsSync(asset)) {
            asset = path.join(assetRoot, "index.html");
        }
        try {
            const response = await net.fetch(pathToFileURL(asset).href);
            const headers = new Headers(response.headers);
            headers.set("Content-Security-Policy", CSP);
            return new Response(response.body, { status: response.status, headers });
        } catch {
            return new Response("Not found", { status: 404 });
        }
    });
    if (config.nativeBridge) {
        ipcMain.handle("next2d:native:request", (event, method, params) => {
            if (!isTrustedSender(event, mainWindow)) throw new Error("Native bridge access denied");
            if (!config.nativeBridge.methods.includes(method)) throw new Error("Native method not allowed");
            if (!nativeBridge) {
                const nativeRoot = path.join(app.isPackaged ? process.resourcesPath : __dirname, "native");
                nativeBridge = new NativeBridge(path.join(nativeRoot, config.nativeBridge.executable), config.nativeBridge.methods, (message) => {
                    if (mainWindow && !mainWindow.isDestroyed()) mainWindow.webContents.send("next2d:native:event", message);
                }, { cwd: nativeRoot });
            }
            return nativeBridge.request(method, params);
        });
    }
    if (overlay) {
        const trusted = (event) => {
            if (!isTrustedSender(event, mainWindow)) {
                diag(`window API access denied (frame url: ${event?.senderFrame?.url})`);
                throw new Error("Window API access denied");
            }
        };
        ipcMain.handle("next2d:window:set-ignore-mouse-events", (event, ignore) => {
            trusted(event);
            if (typeof ignore !== "boolean") throw new TypeError("ignore must be a boolean");
            overlayIgnore = ignore;
            mainWindow.setIgnoreMouseEvents(ignore, ignore ? { forward: true } : undefined);
        });
        ipcMain.handle("next2d:window:get-bounds", (event) => {
            trusted(event);
            const bounds = mainWindow.getBounds();
            return { ...bounds, scaleFactor: screen.getDisplayMatching(bounds).scaleFactor };
        });
        // After sleep or screen lock the OS can drop the mouse-forwarding hook and reset the window's
        // z-order/bounds. Apply the overlay state again and tell the page so it can recover too.
        const recoverOverlay = () => {
            if (!mainWindow || mainWindow.isDestroyed()) return;
            keepOnTop();
            placeOverlay();
            mainWindow.setIgnoreMouseEvents(false);
            mainWindow.setIgnoreMouseEvents(overlayIgnore, overlayIgnore ? { forward: true } : undefined);
            mainWindow.webContents.send("next2d:window:resume");
        };
        for (const name of ["suspend", "lock-screen"]) powerMonitor.on(name, () => diag(name));
        for (const name of ["resume", "unlock-screen"]) {
            powerMonitor.on(name, () => {
                diag(name);
                setTimeout(() => {
                    recoverOverlay();
                    // Only a resume from sleep can reset the GPU (unlocking the screen cannot).
                    if (name === "resume") reloadRenderer();
                }, 1000);
            });
        }
        app.on("child-process-gone", (_event, details) => diag(`child-process-gone ${details.type} ${details.reason}`));
        ipcMain.handle("next2d:window:quit", (event) => {
            trusted(event);
            // Reply before the window is torn down.
            setImmediate(() => app.quit());
        });
    }
    await createWindow();
}).catch((error) => {
    console.error(error);
    app.exit(1);
});

if (singleInstance && hasLock) {
    // Launching again does nothing visible; just make sure the overlay is still shown on top.
    app.on("second-instance", () => {
        if (mainWindow && !mainWindow.isDestroyed()) {
            mainWindow.setAlwaysOnTop(true, "floating");
            if (!mainWindow.isVisible()) mainWindow.showInactive();
        }
    });
}

// Closing the game must stop Steam's "Running" status, including on macOS.
app.on("window-all-closed", () => app.quit());
