// 結合テスト: Next2D player の renderer worker (WebGL バックエンド) を、
// WebGL2 バインディング (src/bindings/webgl/WebGL2.cpp) の上でそのまま動かす。
//
// worker のバンドルを Node に読み込み、本物と同じメッセージ
// (initialize → resize → render) を送って、デフォルトフレームバッファの
// ピクセルを読み戻して検証する。render キューは player のメインスレッドが
// 生成するのと同じ Float32Array 形式を手組みする。
//
//   NEXT2D_WEBGL2_ADDON     : tests/webgl2/addon.cpp をビルドした .node
//   NEXT2D_RENDERER_BUNDLE  : player の packages/renderer を iife でバンドルした JS
"use strict";

const fs = require("node:fs");
const assert = require("node:assert/strict");

const addon = require(process.env.NEXT2D_WEBGL2_ADDON);
const bundle = fs.readFileSync(process.env.NEXT2D_RENDERER_BUNDLE, "utf8");

const WIDTH = 256;
const HEIGHT = 256;

// ---------------------------------------------------------------------------
// worker 環境の最小シム
// ---------------------------------------------------------------------------
const gl = addon.createContext(WIDTH, HEIGHT);

// 呼ばれた WebGL メソッドの集計 (未実装メソッドの検出と、使用範囲の把握用)
const called = new Map();
const missing = new Set();
// renderer が束縛したままにする状態 (読み戻しの前後で退避・復元するために追跡する)
const bound = { "packBuffer": null, "readFramebuffer": null, "drawFramebuffer": null };
const track = {
    bindBuffer (target, buffer) {
        if (target === gl.PIXEL_PACK_BUFFER) {
            bound.packBuffer = buffer;
        }
        gl.bindBuffer(target, buffer);
    },
    bindFramebuffer (target, framebuffer) {
        if (target !== gl.DRAW_FRAMEBUFFER) {
            bound.readFramebuffer = framebuffer;
        }
        if (target !== gl.READ_FRAMEBUFFER) {
            bound.drawFramebuffer = framebuffer;
        }
        gl.bindFramebuffer(target, framebuffer);
    }
};
const tracedGL = new Proxy(gl, {
    get (target, key) {
        const value = target[key];
        if (typeof key === "string") {
            if (value === undefined) {
                missing.add(key);
            } else if (typeof value === "function") {
                called.set(key, (called.get(key) || 0) + 1);
                if (key in track) {
                    return track[key];
                }
            }
        }
        return value;
    }
});

const posted = [];
let onMessage = null;
globalThis.self = globalThis;
globalThis.addEventListener = (type, listener) => {
    if (type === "message") {
        onMessage = listener;
    }
};
globalThis.postMessage = (message) => { posted.push(message); };
globalThis.requestAnimationFrame = (callback) => setTimeout(() => callback(performance.now()), 0);

// テキスト用などの 2D canvas はこのテストの対象外 (ホスト本体では Canvas2D が担当)
globalThis.OffscreenCanvas = class OffscreenCanvas {
    constructor (width, height) { this.width = width; this.height = height; }
    getContext () { return null; }
};

// メインの描画先 canvas
const mainCanvas = {
    "width": WIDTH,
    "height": HEIGHT,
    getContext (type) { return type === "webgl2" ? tracedGL : null; }
};

// navigator.gpu が無ければ renderer は WebGL バックエンドを選ぶ
assert.ok(!("gpu" in globalThis.navigator), "navigator.gpu must be absent for this test");

(0, eval)(bundle);
assert.ok(onMessage, "renderer worker did not register a message listener");

const send = (data) => onMessage({ data });
const settle = async () => {
    // CommandController は async。フェンス待ち (setTimeout 4ms) も含めて落ち着くまで待つ
    for (let i = 0; i < 20; ++i) {
        await new Promise((resolve) => setTimeout(resolve, 5));
    }
};

// ---------------------------------------------------------------------------
// render キューの組み立て (packages/renderer の読み取り順と一致させる)
// ---------------------------------------------------------------------------
const CMD = {
    "MOVE_TO": 0, "CURVE_TO": 1, "LINE_TO": 2, "CUBIC": 3, "ARC": 4,
    "FILL_STYLE": 5, "STROKE_STYLE": 6, "END_FILL": 7, "END_STROKE": 8,
    "BEGIN_PATH": 9, "GRADIENT_FILL": 10, "CLOSE_PATH": 12
};
const BLEND_NORMAL = 11;

let nextKey = 1;

const rectPath = (x0, y0, x1, y1) => [
    CMD.BEGIN_PATH,
    CMD.MOVE_TO, x0, y0,
    CMD.LINE_TO, x1, y0,
    CMD.LINE_TO, x1, y1,
    CMD.LINE_TO, x0, y1,
    CMD.LINE_TO, x0, y0
];

// ベクター Shape 1 個ぶんのキュー。commands はローカル座標 (0,0)-(w,h)、(tx,ty) に配置。
// filter: フィルターのパラメータ列 (type, 引数...)。pixels: RGBA8 のビットマップ (commands の代わり)。
const shape = ({ w, h, tx, ty, commands, alpha = 1, key = nextKey++, cached = false, filter = null, pixels = null }) =>
{
    const q = [
        1,          // visible
        0x01,       // type: shape
        1, 0, 0, 1, tx, ty,                 // matrix
        1, 1, 1, alpha, 0, 0, 0, 0,         // color transform (mul rgba, add rgba)
        tx, ty, tx + w, ty + h,             // bounds (screen)
        0, 0, w, h,                         // base bounds (local)
        0,          // isGridEnabled
        pixels ? 0 : 1,     // isDrawable
        pixels ? 1 : 0,     // renderMode: 0=vector, 1=bitmap
        key,        // uniqueKey
        1,          // cacheKey
        1, 1,       // xScale, yScale
        key,        // filter key
        cached ? 1 : 0
    ];
    if (!cached) {
        if (pixels) {
            // ビットマップは 4 バイトを 1 要素の float に詰めて運ぶ (長さはバイト数)。
            // 数値配列を経由するので、NaN になるビット列 (正規化で壊れる) は使えない。
            const packed = new Float32Array(pixels.buffer, pixels.byteOffset, pixels.byteLength / 4);
            assert.ok(packed.every((v) => !Number.isNaN(v)), "test pixels must not form NaN bit patterns");
            q.push(pixels.byteLength, ...packed);
        } else {
            q.push(commands.length, ...commands);
        }
    }
    q.push(BLEND_NORMAL);
    if (filter) {
        q.push(1, 1 /* updated */, 0, 0, w, h, filter.length, ...filter);
    } else {
        q.push(0);
    }
    return q;
};

// マスク付きコンテナ: mask (ローカル座標の矩形パス) で children を切り抜く
const maskedContainer = ({ maskBounds, maskCommands, children }) =>
{
    const q = [
        1,          // visible
        0x00,       // type: container
        BLEND_NORMAL,
        0,          // useLayer
        1,          // useMaskDisplayObject
        ...maskBounds,
        0x01,       // mask type: shape
        1, 0, 0, 1, 0, 0,   // mask matrix
        0,          // isGridEnabled
        maskCommands.length, ...maskCommands,
        children.length
    ];
    children.forEach((child, depth) => {
        q.push(depth + 1, 0 /* clipDepth */, ...child);
    });
    return q;
};

const fillRect = (x0, y0, x1, y1, r, g, b, a = 255) => [
    ...rectPath(x0, y0, x1, y1),
    CMD.FILL_STYLE, r, g, b, a,
    CMD.END_FILL
];

const render = async (backgroundColor, ...shapes) =>
{
    const data = [backgroundColor];
    for (const s of shapes) {
        data.push(...s);
    }
    const buffer = new Float32Array(data);
    posted.length = 0;
    send({ "command": "render", buffer, "length": buffer.length, "imageBitmaps": null });
    await settle();
    assert.ok(posted.some((m) => m.message === "render"), "renderer did not acknowledge the frame");
};

// canvas 座標 (左上原点) でデフォルトフレームバッファを読む
const readCanvas = () =>
{
    // renderer は PIXEL_PACK_BUFFER とフレームバッファを束縛したままにするので、
    // 読み戻しのあいだだけ外して元に戻す。
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, null);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    const pixels = new Uint8Array(WIDTH * HEIGHT * 4);
    gl.readPixels(0, 0, WIDTH, HEIGHT, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, bound.readFramebuffer);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, bound.drawFramebuffer);
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, bound.packBuffer);
    return (x, y) => {
        const o = ((HEIGHT - 1 - y) * WIDTH + x) * 4;
        return [pixels[o], pixels[o + 1], pixels[o + 2], pixels[o + 3]];
    };
};

const near = (actual, expected, tolerance, label) =>
{
    for (let i = 0; i < 4; ++i) {
        assert.ok(Math.abs(actual[i] - expected[i]) <= tolerance,
            `${label}: got [${actual}] expected [${expected}] (±${tolerance})`);
    }
};

// ---------------------------------------------------------------------------
// テスト
// ---------------------------------------------------------------------------
let passed = 0;
let failed = 0;
const test = async (name, fn) =>
{
    try {
        await fn();
        const error = gl.getError();
        assert.equal(error, gl.NO_ERROR, `GL error 0x${error.toString(16)}`);
        ++passed;
        console.log(`ok   ${name}`);
    } catch (e) {
        ++failed;
        console.log(`FAIL ${name}\n     ${e && e.stack ? e.stack.split("\n").slice(0, 6).join("\n     ") : e}`);
        while (gl.getError() !== gl.NO_ERROR) { /* drain */ }
    }
};

(async () =>
{
    await test("initialize selects the WebGL backend and boots the context", async () =>
    {
        send({ "command": "initialize", "canvas": mainCanvas, "devicePixelRatio": 1 });
        await settle();
        assert.ok(posted.some((m) => m.message === "renderFlowControl"), "no renderFlowControl message");
        send({ "command": "resize", "buffer": new Float32Array([WIDTH, HEIGHT, 1]) });
        await settle();
    });

    await test("background color only", async () =>
    {
        await render(0x336699);
        const at = readCanvas();
        near(at(5, 5), [0x33, 0x66, 0x99, 255], 1, "top-left");
        near(at(250, 250), [0x33, 0x66, 0x99, 255], 1, "bottom-right");
        assert.ok(addon.needsPresent(), "frame did not request presentation");
    });

    await test("vector fill is placed at canvas coordinates (y down)", async () =>
    {
        await render(0xffffff,
            shape({ "w": 100, "h": 50, "tx": 20, "ty": 10, "commands": fillRect(0, 0, 100, 50, 255, 0, 0) })
        );
        const at = readCanvas();
        near(at(70, 35), [255, 0, 0, 255], 1, "inside");
        near(at(70, 80), [255, 255, 255, 255], 1, "below");
        near(at(140, 35), [255, 255, 255, 255], 1, "right");
        near(at(10, 35), [255, 255, 255, 255], 1, "left");
        near(at(70, 4), [255, 255, 255, 255], 1, "above");
    });

    await test("cached node is reused on the next frame", async () =>
    {
        const key = nextKey++;
        await render(0xffffff,
            shape({ "w": 40, "h": 40, "tx": 0, "ty": 0, key, "commands": fillRect(0, 0, 40, 40, 0, 128, 0) })
        );
        await render(0xffffff,
            shape({ "w": 40, "h": 40, "tx": 200, "ty": 200, key, "cached": true })
        );
        const at = readCanvas();
        near(at(220, 220), [0, 128, 0, 255], 1, "moved");
        near(at(20, 20), [255, 255, 255, 255], 1, "old position");
    });

    await test("alpha blending of overlapping shapes", async () =>
    {
        await render(0x000000,
            shape({ "w": 100, "h": 100, "tx": 50, "ty": 50, "commands": fillRect(0, 0, 100, 100, 0, 0, 255) }),
            shape({
                "w": 100, "h": 100, "tx": 100, "ty": 100, "alpha": 0.5,
                "commands": fillRect(0, 0, 100, 100, 255, 0, 0)
            })
        );
        const at = readCanvas();
        near(at(75, 75), [0, 0, 255, 255], 1, "blue only");
        near(at(125, 125), [128, 0, 128, 255], 3, "overlap");
        near(at(175, 175), [128, 0, 0, 255], 3, "red over black");
    });

    await test("curves: circle built from quadratic segments", async () =>
    {
        // 半径 50 の円を 8 本の 2 次ベジェで近似
        const r = 50;
        const cx = 50;
        const cy = 50;
        const commands = [CMD.BEGIN_PATH, CMD.MOVE_TO, cx + r, cy];
        const steps = 8;
        for (let i = 1; i <= steps; ++i) {
            const a0 = (i - 0.5) / steps * Math.PI * 2;
            const a1 = i / steps * Math.PI * 2;
            const k = r / Math.cos(Math.PI / steps);
            commands.push(CMD.CURVE_TO,
                cx + Math.cos(a0) * k, cy + Math.sin(a0) * k,
                cx + Math.cos(a1) * r, cy + Math.sin(a1) * r);
        }
        commands.push(CMD.FILL_STYLE, 255, 255, 0, 255, CMD.END_FILL);
        await render(0x000000, shape({ "w": 100, "h": 100, "tx": 78, "ty": 78, commands }));
        const at = readCanvas();
        near(at(128, 128), [255, 255, 0, 255], 1, "center");
        near(at(128, 85), [255, 255, 0, 255], 1, "near top edge");
        near(at(82, 82), [0, 0, 0, 255], 1, "corner outside the circle");
        near(at(174, 174), [0, 0, 0, 255], 1, "opposite corner");
    });

    await test("stroke", async () =>
    {
        const commands = [
            CMD.BEGIN_PATH,
            CMD.MOVE_TO, 10, 50,
            CMD.LINE_TO, 190, 50,
            CMD.STROKE_STYLE, 10, 0, 2, 0, 0, 255, 255, 255,
            CMD.END_STROKE
        ];
        await render(0x000000, shape({ "w": 200, "h": 100, "tx": 20, "ty": 20, commands }));
        const at = readCanvas();
        near(at(120, 70), [0, 255, 255, 255], 1, "on the line");
        near(at(120, 60), [0, 0, 0, 255], 1, "above the line");
        near(at(120, 80), [0, 0, 0, 255], 1, "below the line");
    });

    await test("linear gradient fill", async () =>
    {
        // player のグラデーション行列は 1638.4 単位の正方形基準 (-819.2..819.2)
        const w = 200;
        const scale = w / 1638.4;
        const commands = [
            ...rectPath(0, 0, w, 50),
            CMD.GRADIENT_FILL,
            0,                          // type: linear
            2,                          // stops
            0, 255, 0, 0, 255,          // ratio, r, g, b, a
            1, 0, 0, 255, 255,
            scale, 0, 0, scale, w / 2, 25,  // matrix
            0,                          // spread: pad
            0,                          // interpolation: rgb
            0                           // focal
        ];
        await render(0x000000, shape({ w, "h": 50, "tx": 28, "ty": 100, commands }));
        const at = readCanvas();
        const left = at(34, 125);
        const middle = at(128, 125);
        const right = at(222, 125);
        assert.ok(left[0] > 200 && left[2] < 55, `left should be red: [${left}]`);
        assert.ok(right[2] > 200 && right[0] < 55, `right should be blue: [${right}]`);
        assert.ok(Math.abs(middle[0] - middle[2]) < 40, `middle should be mixed: [${middle}]`);
        assert.equal(middle[3], 255);
    });

    await test("resize re-creates the frame buffers", async () =>
    {
        send({ "command": "resize", "buffer": new Float32Array([128, 128, 1]) });
        await settle();
        await render(0xffffff,
            shape({ "w": 30, "h": 30, "tx": 10, "ty": 10, "commands": fillRect(0, 0, 30, 30, 255, 0, 255) })
        );
        assert.equal(mainCanvas.width, 128);
        send({ "command": "resize", "buffer": new Float32Array([WIDTH, HEIGHT, 1]) });
        await settle();
        await render(0xffffff,
            shape({ "w": 30, "h": 30, "tx": 200, "ty": 10, "commands": fillRect(0, 0, 30, 30, 255, 0, 255) })
        );
        const at = readCanvas();
        near(at(215, 25), [255, 0, 255, 255], 1, "after resize back");
        near(at(25, 25), [255, 255, 255, 255], 1, "old shape is gone");
    });

    await test("many shapes in one frame (instanced batch)", async () =>
    {
        const shapes = [];
        const key = nextKey++;
        shapes.push(shape({ "w": 8, "h": 8, "tx": 0, "ty": 0, key, "commands": fillRect(0, 0, 8, 8, 255, 128, 0) }));
        for (let i = 1; i < 400; ++i) {
            shapes.push(shape({
                "w": 8, "h": 8,
                "tx": i % 20 * 12, "ty": Math.floor(i / 20) * 12,
                key, "cached": true
            }));
        }
        await render(0x000000, ...shapes);
        const at = readCanvas();
        near(at(4, 4), [255, 128, 0, 255], 1, "first");
        near(at(19 * 12 + 4, 19 * 12 + 4), [255, 128, 0, 255], 1, "last");
        near(at(10, 4), [0, 0, 0, 255], 1, "gap");
    });

    await test("bitmap pixels (texSubImage2D into the atlas)", async () =>
    {
        // 上半分が赤、下半分が緑の 16x16
        const size = 16;
        const pixels = new Uint8Array(size * size * 4);
        for (let y = 0; y < size; ++y) {
            for (let x = 0; x < size; ++x) {
                pixels.set(y < size / 2 ? [255, 0, 0, 255] : [0, 255, 0, 255], (y * size + x) * 4);
            }
        }
        await render(0x000000, shape({ "w": size, "h": size, "tx": 100, "ty": 100, pixels }));
        const at = readCanvas();
        near(at(108, 103), [255, 0, 0, 255], 1, "top half");
        near(at(108, 112), [0, 255, 0, 255], 1, "bottom half");
        near(at(108, 120), [0, 0, 0, 255], 1, "outside");
    });

    await test("mask clips children (stencil path)", async () =>
    {
        await render(0x000000, maskedContainer({
            "maskBounds": [50, 50, 100, 100],
            "maskCommands": [...rectPath(50, 50, 100, 100), CMD.FILL_STYLE, 255, 255, 255, 255, CMD.END_FILL],
            "children": [
                shape({ "w": 200, "h": 200, "tx": 0, "ty": 0, "commands": fillRect(0, 0, 200, 200, 255, 255, 255) })
            ]
        }));
        const at = readCanvas();
        near(at(75, 75), [255, 255, 255, 255], 1, "inside the mask");
        near(at(25, 75), [0, 0, 0, 255], 1, "left of the mask");
        near(at(150, 75), [0, 0, 0, 255], 1, "right of the mask");
        near(at(75, 25), [0, 0, 0, 255], 1, "above the mask");
        near(at(75, 150), [0, 0, 0, 255], 1, "below the mask");
    });

    await test("blur filter (offscreen passes + composite)", async () =>
    {
        await render(0x000000, shape({
            "w": 60, "h": 60, "tx": 100, "ty": 100,
            "commands": fillRect(0, 0, 60, 60, 255, 255, 255),
            "filter": [1 /* BlurFilter */, 16, 16, 1]
        }));
        const at = readCanvas();
        const center = at(130, 130);
        // このキューはフィルターぶんの領域拡張をしていないので、ぼかしは元の矩形の内側に出る
        const edge = at(108, 130);
        const far = at(30, 30);
        assert.ok(center[0] > 230, `center should stay bright: [${center}]`);
        assert.ok(edge[0] > 40 && edge[0] < 215, `edge should be blurred: [${edge}]`);
        near(far, [0, 0, 0, 255], 1, "far away");
    });

    assert.equal(missing.size, 0, `renderer touched unimplemented WebGL members: ${[...missing].join(", ")}`);

    console.log(`\n${passed} passed, ${failed} failed`);
    console.log(`WebGL methods used by the renderer: ${called.size}`);
    if (process.env.NEXT2D_WEBGL2_TRACE) {
        console.log([...called.entries()].sort((a, b) => b[1] - a[1]).map(([k, v]) => `${k}:${v}`).join(" "));
    }
    process.exit(failed ? 1 : 0);
})().catch((error) => {
    console.error(error);
    process.exit(1);
});
