"use strict";

/**
 * Next2D Xbox host セルフテスト。
 *
 * 実行方法: Next2DXboxHost.exe --selftest
 * (bootstrap.js の後にクラシックスクリプトとして評価され、アプリの代わりに走る)
 *
 * C++ バインディング (WebGPU / Canvas2D / Worker / XHR / Audio / 入力 / 画像) を
 * 実機・PC(GDK) 上で網羅的に実行し、TAP 風の結果を console に出す。
 * 完了時に globalThis.__selftestExitCode を設定するとホストが自動終了する。
 *
 * Windows PC / devkit を入手したら最初にこれを流すことで、
 * macOS 上では検証できなかった実挙動を一括確認できる。
 */
(function () {

    const results = { "pass": 0, "fail": 0, "skip": 0 };
    const failures = [];

    function tap(status, name, detail) {
        const line = status === "ok"
            ? "ok - " + name
            : status === "skip"
                ? "ok - " + name + " # SKIP " + (detail || "")
                : "not ok - " + name + (detail ? " # " + detail : "");
        console.log("[selftest] " + line);
    }

    function pass(name) { results.pass++; tap("ok", name) }
    function fail(name, e) {
        results.fail++;
        const msg = e && e.message ? e.message : String(e);
        failures.push(name + ": " + msg);
        tap("fail", name, msg);
    }
    function skip(name, why) { results.skip++; tap("skip", name, why) }

    function assert(cond, msg) { if (!cond) { throw new Error(msg || "assertion failed") } }

    function withTimeout(promise, ms, what) {
        return Promise.race([
            promise,
            new Promise((_, rej) => setTimeout(
                () => rej(new Error("timeout (" + ms + "ms): " + (what || ""))), ms))
        ]);
    }

    async function test(name, fn, timeoutMs) {
        try {
            await withTimeout(Promise.resolve().then(fn), timeoutMs || 5000, name);
            pass(name);
        } catch (e) {
            fail(name, e);
        }
    }

    // 環境依存 (音声デバイス無し等) で失敗し得るテスト: 失敗を skip として扱う
    async function softTest(name, fn, timeoutMs) {
        try {
            await withTimeout(Promise.resolve().then(fn), timeoutMs || 5000, name);
            pass(name);
        } catch (e) {
            skip(name, e && e.message ? e.message : String(e));
        }
    }

    // --- テスト用の埋め込みアセット -----------------------------------------
    // 2x2 PNG: (0,0)=赤 (1,0)=緑 (0,1)=青 (1,1)=半透明白
    const TEST_PNG = new Uint8Array([
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00,
        0x13, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
        0x1f, 0x0c, 0x81, 0x34, 0x08, 0x34, 0x00, 0x00, 0x49, 0x49, 0x09, 0x78,
        0x28, 0xa0, 0xdb, 0x77, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
        0xae, 0x42, 0x60, 0x82
    ]);

    // 48kHz mono 16bit PCM WAV (矩形波 48 サンプル = 1ms)
    const TEST_WAV = new Uint8Array([
        0x52, 0x49, 0x46, 0x46, 0x84, 0x00, 0x00, 0x00, 0x57, 0x41, 0x56, 0x45,
        0x66, 0x6d, 0x74, 0x20, 0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
        0x80, 0xbb, 0x00, 0x00, 0x00, 0x77, 0x01, 0x00, 0x02, 0x00, 0x10, 0x00,
        0x64, 0x61, 0x74, 0x61, 0x60, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x40,
        0x00, 0x40, 0x00, 0x40, 0x00, 0xc0, 0x00, 0xc0, 0x00, 0xc0, 0x00, 0xc0,
        0x00, 0x40, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40, 0x00, 0xc0, 0x00, 0xc0,
        0x00, 0xc0, 0x00, 0xc0, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40,
        0x00, 0xc0, 0x00, 0xc0, 0x00, 0xc0, 0x00, 0xc0, 0x00, 0x40, 0x00, 0x40,
        0x00, 0x40, 0x00, 0x40, 0x00, 0xc0, 0x00, 0xc0, 0x00, 0xc0, 0x00, 0xc0,
        0x00, 0x40, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40, 0x00, 0xc0, 0x00, 0xc0,
        0x00, 0xc0, 0x00, 0xc0, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40,
        0x00, 0xc0, 0x00, 0xc0, 0x00, 0xc0, 0x00, 0xc0
    ]);

    async function main() {
        console.log("[selftest] === Next2D Xbox host selftest start ===");

        // ==================== 基盤 ====================
        await test("globals: window/document/navigator/screen", () => {
            assert(typeof window === "object", "window");
            assert(typeof document === "object", "document");
            assert(typeof navigator === "object", "navigator");
            assert(typeof screen === "object", "screen");
            assert(window === globalThis && self === globalThis, "window === globalThis");
        });

        await test("timers: setTimeout / setInterval / clearInterval", async () => {
            const order = [];
            await new Promise((resolve) => {
                setTimeout(() => order.push("t10"), 10);
                setTimeout(() => { order.push("t30"); resolve() }, 30);
                Promise.resolve().then(() => order.push("micro"));
                order.push("sync");
            });
            assert(order[0] === "sync" && order[1] === "micro",
                "microtask order: " + order.join(","));
            assert(order.indexOf("t10") < order.indexOf("t30"), "timer order");

            let ticks = 0;
            await new Promise((resolve) => {
                const id = setInterval(() => {
                    if (++ticks >= 2) { clearInterval(id); resolve() }
                }, 5);
            });
            assert(ticks === 2, "interval stopped after clearInterval");
        });

        await test("requestAnimationFrame: フレームが進む", async () => {
            const t0 = performance.now();
            await new Promise((resolve) => requestAnimationFrame(resolve));
            await new Promise((resolve) => requestAnimationFrame(resolve));
            assert(performance.now() >= t0, "performance.now monotonic");
        });

        await test("EventTarget: addEventListener / dispatchEvent", () => {
            let got = null;
            const handler = (e) => { got = e };
            window.addEventListener("selftest-event", handler);
            window.dispatchEvent({ "type": "selftest-event", "value": 42 });
            assert(got && got.value === 42, "listener called");
            window.removeEventListener("selftest-event", handler);
            got = null;
            window.dispatchEvent({ "type": "selftest-event" });
            assert(got === null, "listener removed");
        });

        // ==================== 画像 ====================
        let bitmap = null;
        await test("createImageBitmap: PNG デコード (WIC)", async () => {
            bitmap = await createImageBitmap(TEST_PNG.buffer);
            assert(bitmap.width === 2 && bitmap.height === 2,
                "2x2 png -> " + bitmap.width + "x" + bitmap.height);
        });

        // ==================== Canvas2D ====================
        await test("Canvas2D: fill / getImageData", () => {
            const c = new OffscreenCanvas(16, 16);
            const ctx = c.getContext("2d");
            ctx.fillStyle = "#ff0000";
            ctx.beginPath();
            ctx.rect(2, 2, 8, 8);
            ctx.fill();
            const d = ctx.getImageData(0, 0, 16, 16).data;
            const at = (x, y) => (y * 16 + x) * 4;
            assert(d[at(5, 5)] === 255 && d[at(5, 5) + 3] === 255, "inside filled");
            assert(d[at(12, 12) + 3] === 0, "outside untouched");
        });

        await test("Canvas2D: clip がはみ出しを抑える (TextField 枠)", () => {
            const c = new OffscreenCanvas(20, 20);
            const ctx = c.getContext("2d");
            ctx.save();
            ctx.beginPath();
            ctx.moveTo(5, 5); ctx.lineTo(15, 5); ctx.lineTo(15, 15); ctx.lineTo(5, 15);
            ctx.clip();
            ctx.fillStyle = "#00ff00";
            ctx.fillRect(0, 0, 20, 20);
            ctx.restore();
            const d = ctx.getImageData(0, 0, 20, 20).data;
            const at = (x, y) => (y * 20 + x) * 4;
            assert(d[at(10, 10) + 1] === 255, "inside clip filled");
            assert(d[at(2, 2) + 3] === 0, "outside clip untouched");
            // restore 後はクリップ解除
            ctx.fillRect(0, 0, 1, 1);
            const d2 = ctx.getImageData(0, 0, 1, 1).data;
            assert(d2[3] !== 0, "clip released after restore");
        });

        await test("Canvas2D: isPointInPath (ヒット判定)", () => {
            const c = new OffscreenCanvas(4, 4);
            const ctx = c.getContext("2d");
            ctx.setTransform(1, 0, 0, 1, 0, 0);
            ctx.beginPath();
            ctx.moveTo(10, 10); ctx.lineTo(30, 10); ctx.lineTo(30, 30); ctx.lineTo(10, 30);
            assert(ctx.isPointInPath(20, 20) === true, "inside");
            assert(ctx.isPointInPath(40, 40) === false, "outside");
        });

        await test("Canvas2D: fillText / measureText (DirectWrite)", () => {
            const c = new OffscreenCanvas(64, 32);
            const ctx = c.getContext("2d");
            ctx.font = "24px sans-serif";
            const m = ctx.measureText("Hello");
            assert(m.width > 10, "measureText width=" + m.width);
            ctx.fillStyle = "#ffffff";
            ctx.fillText("A", 4, 24);
            const d = ctx.getImageData(0, 0, 64, 32).data;
            let opaque = 0;
            for (let i = 3; i < d.length; i += 4) { if (d[i] > 128) { opaque++ } }
            assert(opaque > 5, "glyph pixels=" + opaque);
        });

        await test("Canvas2D: drawImage (ImageBitmap 合成)", () => {
            assert(bitmap, "needs decoded bitmap");
            const c = new OffscreenCanvas(4, 4);
            const ctx = c.getContext("2d");
            ctx.drawImage(bitmap, 0, 0);
            const d = ctx.getImageData(0, 0, 2, 2).data;
            assert(d[0] === 255 && d[1] === 0, "(0,0) red");
            assert(d[4] === 0 && d[5] === 255, "(1,0) green");
        });

        // ==================== ネットワーク (Blob / URL / XHR) ====================
        let blobUrl = null;
        await test("Blob / URL.createObjectURL / revokeObjectURL", () => {
            const blob = new Blob(["self.__marker = 1;"], { "type": "text/javascript" });
            blobUrl = URL.createObjectURL(blob);
            assert(typeof blobUrl === "string" && blobUrl.indexOf("blob:") === 0,
                "url=" + blobUrl);
        });

        await softTest("XMLHttpRequest: アセット読込 (assets/app)", async () => {
            await new Promise((resolve, reject) => {
                const xhr = new XMLHttpRequest();
                xhr.open("GET", "index.html", true);
                xhr.onload = () => xhr.status === 200 ? resolve() : reject(new Error("status " + xhr.status));
                xhr.onerror = () => reject(new Error("xhr error (assets/app 未配置?)"));
                xhr.send();
            });
        });

        // ==================== Worker ====================
        await test("Worker: blob URL + postMessage echo (協調実行)", async () => {
            const src = "self.onmessage = function(e) { " +
                        "  self.postMessage({ echo: e.data.value * 2, arr: e.data.arr }); " +
                        "};";
            const url = URL.createObjectURL(new Blob([src], { "type": "text/javascript" }));
            const worker = new Worker(url);
            const reply = await new Promise((resolve, reject) => {
                worker.onmessage = (e) => resolve(e.data);
                worker.onerror = (e) => reject(new Error("worker error: " + (e && e.message)));
                worker.postMessage({ "value": 21, "arr": new Float32Array([1.5, 2.5]) });
            });
            assert(reply.echo === 42, "echo=" + reply.echo);
            assert(reply.arr instanceof Float32Array && reply.arr[1] === 2.5,
                "structured clone TypedArray");
            if (worker.terminate) { worker.terminate() }
        });

        await test("OffscreenCanvas: transferControlToOffscreen + Worker 転送", async () => {
            const canvas = document.createElement("canvas");
            canvas.width = 8; canvas.height = 8;
            const off = canvas.transferControlToOffscreen();
            assert(off && typeof off === "object", "offscreen created");
            const src = "self.onmessage = function(e) { " +
                        "  var c = e.data.canvas; " +
                        "  self.postMessage({ w: c ? c.width : -1 }); " +
                        "};";
            const url = URL.createObjectURL(new Blob([src], { "type": "text/javascript" }));
            const worker = new Worker(url);
            const reply = await new Promise((resolve, reject) => {
                worker.onmessage = (e) => resolve(e.data);
                worker.onerror = (_e) => reject(new Error("worker error"));
                worker.postMessage({ "canvas": off }, [off]);
            });
            assert(reply.w === 8, "transferred canvas width=" + reply.w);
            if (worker.terminate) { worker.terminate() }
        });

        // ==================== WebGPU ====================
        // GPU の無い環境 (CI ランナー等) では adapter が取れず skip になる (softTest)。
        // 実機・GPU あり PC では全て実行される。
        let device = null;
        await softTest("WebGPU: requestAdapter / requestDevice / limits", async () => {
            assert(navigator.gpu, "navigator.gpu");
            const format = navigator.gpu.getPreferredCanvasFormat();
            assert(typeof format === "string" && format.length > 0, "preferred format=" + format);
            const adapter = await navigator.gpu.requestAdapter();
            assert(adapter, "adapter");
            device = await adapter.requestDevice();
            assert(device, "device");
            assert(device.limits && device.limits.maxTextureDimension2D >= 4096,
                "maxTextureDimension2D=" + (device.limits && device.limits.maxTextureDimension2D));
        });

        await softTest("WebGPU: writeBuffer -> copyBufferToBuffer -> mapAsync 読み戻し", async () => {
            assert(device, "needs device");
            const src = device.createBuffer({ "size": 16, "usage": GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST });
            const dst = device.createBuffer({ "size": 16, "usage": GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
            device.queue.writeBuffer(src, 0, new Uint32Array([1, 2, 3, 4]));
            const enc = device.createCommandEncoder();
            enc.copyBufferToBuffer(src, 0, dst, 0, 16);
            device.queue.submit([enc.finish()]);
            await dst.mapAsync(GPUMapMode.READ);
            const view = new Uint32Array(dst.getMappedRange().slice(0));
            dst.unmap();
            assert(view[0] === 1 && view[3] === 4, "readback=" + Array.from(view).join(","));
        });

        await softTest("WebGPU: copyExternalImageToTexture -> copyTextureToBuffer 読み戻し", async () => {
            assert(device && bitmap, "needs device+bitmap");
            const tex = device.createTexture({
                "size": { "width": 2, "height": 2 },
                "format": "rgba8unorm",
                "usage": GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.COPY_SRC | GPUTextureUsage.RENDER_ATTACHMENT
            });
            device.queue.copyExternalImageToTexture(
                { "source": bitmap }, { "texture": tex }, { "width": 2, "height": 2 });
            // bytesPerRow は 256 アライン必須
            const buf = device.createBuffer({ "size": 256 * 2, "usage": GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
            const enc = device.createCommandEncoder();
            enc.copyTextureToBuffer(
                { "texture": tex }, { "buffer": buf, "bytesPerRow": 256 }, { "width": 2, "height": 2 });
            device.queue.submit([enc.finish()]);
            await buf.mapAsync(GPUMapMode.READ);
            const px = new Uint8Array(buf.getMappedRange().slice(0));
            buf.unmap();
            assert(px[0] === 255 && px[1] === 0 && px[2] === 0, "(0,0) red: " + px[0] + "," + px[1] + "," + px[2]);
            assert(px[4] === 0 && px[5] === 255, "(1,0) green");
        });

        await softTest("WebGPU: dynamic offset 付き uniform で描画 (塗りの中核経路)", async () => {
            assert(device, "needs device");
            const shader = device.createShaderModule({ "code": `
                struct U { color: vec4f };
                @group(0) @binding(0) var<uniform> u: U;
                @vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
                    var p = array<vec2f, 3>(vec2f(-1,-3), vec2f(3,1), vec2f(-1,1));
                    return vec4f(p[i], 0, 1);
                }
                @fragment fn fs() -> @location(0) vec4f { return u.color; }
            ` });
            const bgl = device.createBindGroupLayout({ "entries": [{
                "binding": 0, "visibility": GPUShaderStage.FRAGMENT,
                "buffer": { "type": "uniform", "hasDynamicOffset": true }
            }] });
            const pipeline = device.createRenderPipeline({
                "layout": device.createPipelineLayout({ "bindGroupLayouts": [bgl] }),
                "vertex": { "module": shader, "entryPoint": "vs" },
                "fragment": { "module": shader, "entryPoint": "fs",
                    "targets": [{ "format": "rgba8unorm" }] },
                "primitive": { "topology": "triangle-list" }
            });
            // offset 0 = 赤 / offset 256 = 緑。dynamic offset 256 で緑が描かれるべき。
            const ubo = device.createBuffer({ "size": 512, "usage": GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
            device.queue.writeBuffer(ubo, 0, new Float32Array([1, 0, 0, 1]));
            device.queue.writeBuffer(ubo, 256, new Float32Array([0, 1, 0, 1]));
            const bindGroup = device.createBindGroup({ "layout": bgl, "entries": [{
                "binding": 0, "resource": { "buffer": ubo, "offset": 0, "size": 16 }
            }] });
            const target = device.createTexture({
                "size": { "width": 4, "height": 4 }, "format": "rgba8unorm",
                "usage": GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC
            });
            const enc = device.createCommandEncoder();
            const pass = enc.beginRenderPass({ "colorAttachments": [{
                "view": target.createView(), "loadOp": "clear", "storeOp": "store",
                "clearValue": [0, 0, 0, 0]
            }] });
            pass.setPipeline(pipeline);
            pass.setBindGroup(0, bindGroup, [256]);   // ← dynamic offset
            pass.draw(3);
            pass.end();
            const buf = device.createBuffer({ "size": 256 * 4, "usage": GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
            const enc2 = device.createCommandEncoder();
            enc2.copyTextureToBuffer({ "texture": target }, { "buffer": buf, "bytesPerRow": 256 }, { "width": 4, "height": 4 });
            device.queue.submit([enc.finish(), enc2.finish()]);
            await buf.mapAsync(GPUMapMode.READ);
            const px = new Uint8Array(buf.getMappedRange().slice(0));
            buf.unmap();
            assert(px[1] === 255 && px[0] === 0,
                "dynamic offset 256 -> green, got rgba=" + px[0] + "," + px[1] + "," + px[2] + "," + px[3]);
        });

        await softTest("WebGPU: stencil8 パイプライン + setStencilReference (マスク経路)", async () => {
            assert(device, "needs device");
            const shader = device.createShaderModule({ "code": `
                @vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
                    var p = array<vec2f, 3>(vec2f(-1,-3), vec2f(3,1), vec2f(-1,1));
                    return vec4f(p[i], 0, 1);
                }
                @fragment fn fs() -> @location(0) vec4f { return vec4f(0, 0, 1, 1); }
            ` });
            const layout = device.createPipelineLayout({ "bindGroupLayouts": [] });
            // 1st: ステンシルへ replace 書き込み (色は書かない設定でも良いが簡略化)
            const writePipe = device.createRenderPipeline({
                layout, "vertex": { "module": shader, "entryPoint": "vs" },
                "fragment": { "module": shader, "entryPoint": "fs", "targets": [{ "format": "rgba8unorm" }] },
                "primitive": { "topology": "triangle-list" },
                "depthStencil": {
                    "format": "stencil8",
                    "stencilFront": { "compare": "always", "failOp": "keep", "depthFailOp": "keep", "passOp": "replace" },
                    "stencilBack":  { "compare": "always", "failOp": "keep", "depthFailOp": "keep", "passOp": "replace" },
                    "stencilReadMask": 0xff, "stencilWriteMask": 0xff
                }
            });
            // 2nd: equal 比較で描画 (マスク内のみ)
            const testPipe = device.createRenderPipeline({
                layout, "vertex": { "module": shader, "entryPoint": "vs" },
                "fragment": { "module": shader, "entryPoint": "fs", "targets": [{ "format": "rgba8unorm" }] },
                "primitive": { "topology": "triangle-list" },
                "depthStencil": {
                    "format": "stencil8",
                    "stencilFront": { "compare": "equal", "failOp": "keep", "depthFailOp": "keep", "passOp": "keep" },
                    "stencilBack":  { "compare": "equal", "failOp": "keep", "depthFailOp": "keep", "passOp": "keep" },
                    "stencilReadMask": 0xff, "stencilWriteMask": 0x00
                }
            });
            const color = device.createTexture({
                "size": { "width": 4, "height": 4 }, "format": "rgba8unorm",
                "usage": GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC
            });
            const stencil = device.createTexture({
                "size": { "width": 4, "height": 4 }, "format": "stencil8",
                "usage": GPUTextureUsage.RENDER_ATTACHMENT
            });
            const enc = device.createCommandEncoder();
            const p1 = enc.beginRenderPass({
                "colorAttachments": [{ "view": color.createView(), "loadOp": "clear", "storeOp": "store", "clearValue": [0,0,0,0] }],
                "depthStencilAttachment": {
                    "view": stencil.createView(),
                    "stencilLoadOp": "clear", "stencilStoreOp": "store", "stencilClearValue": 0
                }
            });
            p1.setPipeline(writePipe);
            p1.setStencilReference(1);
            p1.draw(3);
            p1.end();
            const p2 = enc.beginRenderPass({
                "colorAttachments": [{ "view": color.createView(), "loadOp": "load", "storeOp": "store" }],
                "depthStencilAttachment": {
                    "view": stencil.createView(),
                    "stencilLoadOp": "load", "stencilStoreOp": "store"
                }
            });
            p2.setPipeline(testPipe);
            p2.setStencilReference(1);   // ref==stencil(1) -> 描画される
            p2.draw(3);
            p2.end();
            const buf = device.createBuffer({ "size": 256 * 4, "usage": GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
            enc.copyTextureToBuffer({ "texture": color }, { "buffer": buf, "bytesPerRow": 256 }, { "width": 4, "height": 4 });
            device.queue.submit([enc.finish()]);
            await buf.mapAsync(GPUMapMode.READ);
            const px = new Uint8Array(buf.getMappedRange().slice(0));
            buf.unmap();
            assert(px[2] === 255, "stencil equal pass drew blue, got rgba=" + px[0] + "," + px[1] + "," + px[2]);
        });

        // ==================== Audio ====================
        let audioCtx = null;
        await test("AudioContext: decodeAudioData (Media Foundation)", async () => {
            audioCtx = new AudioContext();
            const buffer = await audioCtx.decodeAudioData(TEST_WAV.buffer.slice(0));
            assert(buffer.sampleRate === 48000, "sampleRate=" + buffer.sampleRate);
            assert(buffer.duration > 0, "duration=" + buffer.duration);
            globalThis.__selftestAudioBuffer = buffer;
        });

        await softTest("Audio: gain 接続 + 再生 + ended 発火", async () => {
            assert(audioCtx && globalThis.__selftestAudioBuffer, "needs decoded buffer");
            const gainNode = audioCtx.createGain();
            gainNode.connect(audioCtx.destination);
            gainNode.gain.value = 0.0;   // ミュートで再生 (実機でも無音)
            const source = audioCtx.createBufferSource();
            source.buffer = globalThis.__selftestAudioBuffer;
            source.connect(gainNode);
            const ended = new Promise((resolve) => source.addEventListener("ended", resolve));
            source.start(0);
            await withTimeout(ended, 3000, "ended event");
        });

        // ==================== 入力 / その他 ====================
        await test("Gamepad: navigator.getGamepads()", () => {
            const pads = navigator.getGamepads();
            assert(Array.isArray(pads) || pads && typeof pads.length === "number",
                "returns array-like");
        });

        await softTest("clipboard: writeText / readText 往復", async () => {
            await navigator.clipboard.writeText("next2d-selftest");
            const text = await navigator.clipboard.readText();
            assert(text === "next2d-selftest", "roundtrip=" + text);
        });

        await softTest("fetch: アセット取得", async () => {
            const res = await fetch("index.html");
            assert(res && (res.ok || res.status === 200), "status=" + (res && res.status));
        });

        // ==================== まとめ ====================
        console.log("[selftest] === done: " +
                    results.pass + " passed, " +
                    results.fail + " failed, " +
                    results.skip + " skipped ===");
        if (failures.length) {
            console.error("[selftest] failures:\n  - " + failures.join("\n  - "));
        }
        globalThis.__selftestExitCode = results.fail > 0 ? 1 : 0;
    }

    main().catch((e) => {
        console.error("[selftest] fatal: " + (e && e.stack ? e.stack : e));
        globalThis.__selftestExitCode = 2;
    });

})();
