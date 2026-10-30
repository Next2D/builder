const { contextBridge, ipcRenderer } = require("electron");
// Set by the main process through webPreferences.additionalArguments.
const flags = new Set(process.argv);

if (flags.has("--next2d-native-bridge")) {
    contextBridge.exposeInMainWorld("next2dNative", {
        request: (method, params) => ipcRenderer.invoke("next2d:native:request", method, params),
        onEvent: (listener) => {
            if (typeof listener !== "function") throw new TypeError("Expected event listener");
            const handler = (_event, message) => listener(message);
            ipcRenderer.on("next2d:native:event", handler);
            return () => ipcRenderer.removeListener("next2d:native:event", handler);
        }
    });
}

if (flags.has("--next2d-overlay")) {
    contextBridge.exposeInMainWorld("next2dWindow", {
        setIgnoreMouseEvents: async (ignore) => {
            if (typeof ignore !== "boolean") throw new TypeError("Expected boolean");
            await ipcRenderer.invoke("next2d:window:set-ignore-mouse-events", ignore);
        },
        getBounds: () => ipcRenderer.invoke("next2d:window:get-bounds"),
        quit: async () => { await ipcRenderer.invoke("next2d:window:quit"); }
    });
}
