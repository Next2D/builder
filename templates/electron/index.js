const { app, BrowserWindow, Menu, net, protocol, screen, session, ipcMain } = require("electron");
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
            fullscreenable: false, skipTaskbar: false, backgroundColor: "#00000000"
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
        const events = ["display-metrics-changed", "display-added", "display-removed"];
        for (const name of events) screen.on(name, placeOverlay);
        mainWindow.once("closed", () => {
            for (const name of events) screen.removeListener(name, placeOverlay);
        });
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
    mainWindow.once("ready-to-show", () => mainWindow.show());
    await mainWindow.loadURL(startURL);
};

app.whenReady().then(async () => {
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
            if (!isTrustedSender(event, mainWindow)) throw new Error("Window API access denied");
        };
        ipcMain.handle("next2d:window:set-ignore-mouse-events", (event, ignore) => {
            trusted(event);
            if (typeof ignore !== "boolean") throw new TypeError("ignore must be a boolean");
            mainWindow.setIgnoreMouseEvents(ignore, ignore ? { forward: true } : undefined);
        });
        ipcMain.handle("next2d:window:get-bounds", (event) => {
            trusted(event);
            const bounds = mainWindow.getBounds();
            return { ...bounds, scaleFactor: screen.getDisplayMatching(bounds).scaleFactor };
        });
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

// Closing the game must stop Steam's "Running" status, including on macOS.
app.on("window-all-closed", () => app.quit());
