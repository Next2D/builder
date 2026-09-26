const { contextBridge, ipcRenderer } = require("electron");

contextBridge.exposeInMainWorld("next2dNative", {
    request: (method, params) => ipcRenderer.invoke("next2d:native:request", method, params),
    onEvent: (listener) => {
        if (typeof listener !== "function") throw new TypeError("Expected event listener");
        const handler = (_event, message) => listener(message);
        ipcRenderer.on("next2d:native:event", handler);
        return () => ipcRenderer.removeListener("next2d:native:event", handler);
    }
});
