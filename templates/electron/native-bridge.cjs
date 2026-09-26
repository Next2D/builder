const { spawn } = require("node:child_process");
const { StringDecoder } = require("node:string_decoder");
const MAX_BYTES = 256 * 1024;

class NativeBridge {
    constructor(executable, methods, onEvent, options = {}) {
        this.methods = new Set(methods);
        this.pending = new Map();
        this.sequence = 0;
        this.failure = null;
        this.timeout = options.timeout ?? 15000;
        this.onEvent = onEvent;
        this.child = spawn(executable, options.args ?? [], {
            cwd: options.cwd, shell: false, windowsHide: true, stdio: ["pipe", "pipe", "pipe"]
        });
        const decoder = new StringDecoder("utf8");
        let buffer = "";
        this.child.stdout.on("data", (chunk) => {
            if (this.failure) return;
            buffer += decoder.write(chunk);
            let end;
            while ((end = buffer.indexOf("\n")) >= 0) {
                const line = buffer.slice(0, end);
                buffer = buffer.slice(end + 1);
                if (Buffer.byteLength(line) > MAX_BYTES) return this.fail("Native message too large");
                try {
                    const message = JSON.parse(line);
                    if (!message || typeof message !== "object") throw new Error("Invalid message");
                    if (typeof message.event === "string" && message.event.length <= 64 && message.event !== "bridge.closed") {
                        this.onEvent({ event: message.event, data: message.data ?? null });
                    } else if (Number.isSafeInteger(message.id)) {
                        const pending = this.pending.get(message.id);
                        if (!pending) continue;
                        clearTimeout(pending.timer);
                        this.pending.delete(message.id);
                        if (typeof message.error === "string") pending.reject(new Error(message.error));
                        else pending.resolve(message.result ?? null);
                    } else throw new Error("Invalid message");
                } catch {
                    return this.fail("Invalid native protocol");
                }
            }
            if (Buffer.byteLength(buffer) > MAX_BYTES) this.fail("Native message too large");
        });
        // Drain diagnostics without sending credentials or SDK logs to the renderer.
        this.child.stderr.resume();
        this.child.stdin.on("error", () => this.fail("Native input closed"));
        this.child.once("error", () => this.fail("Native process could not start"));
        this.child.once("exit", () => this.fail("Native process exited"));
    }

    async request(method, params) {
        if (this.failure) throw new Error(this.failure);
        if (!this.methods.has(method)) throw new Error("Native method not allowed");
        if (this.pending.size >= 128 || this.child.stdin.writableLength > MAX_BYTES) throw new Error("Native bridge busy");
        const id = ++this.sequence;
        const line = JSON.stringify({ id, method, params: params ?? null }) + "\n";
        if (Buffer.byteLength(line) > MAX_BYTES) throw new Error("Native request too large");
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this.pending.delete(id);
                reject(new Error("Native request timed out"));
            }, this.timeout);
            this.pending.set(id, { resolve, reject, timer });
            this.child.stdin.write(line);
        });
    }

    fail(reason, graceful = false) {
        if (this.failure) return;
        this.failure = reason;
        for (const pending of this.pending.values()) {
            clearTimeout(pending.timer);
            pending.reject(new Error(reason));
        }
        this.pending.clear();
        if (graceful) {
            this.child.stdin.end();
            const timer = setTimeout(() => this.child.kill("SIGKILL"), 4000);
            timer.unref();
            this.child.once("exit", () => clearTimeout(timer));
        } else this.child.kill("SIGKILL");
        this.onEvent({ event: "bridge.closed", data: { reason } });
    }

    close() { this.fail("Native bridge closed", true); }
}

const isTrustedSender = (event, window) => {
    if (!window || window.isDestroyed() || event.sender !== window.webContents
        || event.senderFrame !== window.webContents.mainFrame) return false;
    try {
        const url = new URL(event.senderFrame.url);
        return url.protocol === "next2d:" && url.host === "game" && !url.username && !url.password;
    } catch { return false; }
};

module.exports = { NativeBridge, isTrustedSender };
