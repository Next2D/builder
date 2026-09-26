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
let mainWindow;
const createWindow = async () => {
    const { width, height } = screen.getPrimaryDisplay().workAreaSize;
    mainWindow = new BrowserWindow({
        width: Math.min(config.window.width, width),
        height: Math.min(config.window.height, height),
        title: config.appName,
        fullscreen: config.window.fullscreen,
        ...(config.icon ? { icon: path.join(assetRoot, config.icon) } : {}),
        useContentSize: true,
        backgroundColor: "#000000",
        autoHideMenuBar: true,
        show: false,
        webPreferences: {
            nodeIntegration: false, contextIsolation: true, sandbox: true,
            ...(config.nativeBridge ? { preload: path.join(__dirname, "preload.cjs"),
                backgroundThrottling: config.nativeBridge.backgroundThrottling ?? false } : {})
        }
    });
    mainWindow.webContents.setWindowOpenHandler(() => ({ action: "deny" }));
    mainWindow.webContents.on("will-navigate", (event, url) => {
        if (!resolveAssetPath(assetRoot, url)) {
            event.preventDefault();
        }
    });
    mainWindow.webContents.on("before-input-event", (event, input) => {
        if (input.type !== "keyDown") {
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
        callback(permission === "fullscreen");
    });
    session.defaultSession.setPermissionCheckHandler((_contents, permission) => permission === "fullscreen");
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
    await createWindow();
}).catch((error) => {
    console.error(error);
    app.exit(1);
});

// Closing the game must stop Steam's "Running" status, including on macOS.
app.on("window-all-closed", () => app.quit());
