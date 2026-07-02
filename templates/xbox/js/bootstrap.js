"use strict";

/**
 * Next2D Xbox host ブートストラップ。
 *
 * C++ ホスト(V8Runtime)が起動時にクラシックスクリプトとして最初に評価する。
 * ここでは C++ バインディングが用意していないブラウザ相当 API を JS 層で補完し、
 * Next2D アプリ本体(ESM)が正しく動く前提を整える。
 *
 * すでに C++ 側で用意済み:
 *   console / setTimeout / setInterval / requestAnimationFrame / performance.now /
 *   fetch / Image / createImageBitmap / AudioContext / navigator.gpu /
 *   navigator.getGamepads / window / document / screen / devicePixelRatio
 */
(function (global) {

    // --- queueMicrotask (V8 が未提供の場合の保険) ---------------------------
    if (typeof global.queueMicrotask !== "function") {
        global.queueMicrotask = function (callback) {
            Promise.resolve().then(callback);
        };
    }

    // --- TextEncoder / TextDecoder (UTF-8) ---------------------------------
    // V8 単体には Web の Text* が含まれないため最小実装を提供する。
    if (typeof global.TextEncoder === "undefined") {
        global.TextEncoder = class TextEncoder {
            get encoding() { return "utf-8"; }
            encode(str) {
                str = String(str);
                const out = [];
                for (let i = 0; i < str.length; i++) {
                    let code = str.charCodeAt(i);
                    if (code < 0x80) {
                        out.push(code);
                    } else if (code < 0x800) {
                        out.push(0xc0 | (code >> 6), 0x80 | (code & 0x3f));
                    } else if (code >= 0xd800 && code <= 0xdbff) {
                        // サロゲートペア
                        const hi = code;
                        const lo = str.charCodeAt(++i);
                        code = 0x10000 + ((hi - 0xd800) << 10) + (lo - 0xdc00);
                        out.push(
                            0xf0 | (code >> 18),
                            0x80 | ((code >> 12) & 0x3f),
                            0x80 | ((code >> 6) & 0x3f),
                            0x80 | (code & 0x3f)
                        );
                    } else {
                        out.push(
                            0xe0 | (code >> 12),
                            0x80 | ((code >> 6) & 0x3f),
                            0x80 | (code & 0x3f)
                        );
                    }
                }
                return new Uint8Array(out);
            }
        };
    }

    if (typeof global.TextDecoder === "undefined") {
        global.TextDecoder = class TextDecoder {
            constructor(label) { this._label = label || "utf-8"; }
            get encoding() { return "utf-8"; }
            decode(input) {
                if (!input) return "";
                const bytes = input instanceof Uint8Array
                    ? input
                    : new Uint8Array(input.buffer || input);
                let out = "";
                let i = 0;
                while (i < bytes.length) {
                    let c = bytes[i++];
                    if (c < 0x80) {
                        out += String.fromCharCode(c);
                    } else if (c < 0xe0) {
                        out += String.fromCharCode(((c & 0x1f) << 6) | (bytes[i++] & 0x3f));
                    } else if (c < 0xf0) {
                        out += String.fromCharCode(
                            ((c & 0x0f) << 12) | ((bytes[i++] & 0x3f) << 6) | (bytes[i++] & 0x3f)
                        );
                    } else {
                        let code = ((c & 0x07) << 18) | ((bytes[i++] & 0x3f) << 12) |
                                   ((bytes[i++] & 0x3f) << 6) | (bytes[i++] & 0x3f);
                        code -= 0x10000;
                        out += String.fromCharCode(0xd800 + (code >> 10), 0xdc00 + (code & 0x3ff));
                    }
                }
                return out;
            }
        };
    }

    // --- URL の最小補完 -----------------------------------------------------
    // ローカル実行では基本パスのみ扱えれば十分。標準 URL があればそれを使う。
    if (typeof global.URL === "undefined") {
        global.URL = class URL {
            constructor(url, base) {
                this.href = base ? (String(base).replace(/\/?$/, "/") + url) : String(url);
                this.pathname = this.href.replace(/^[a-z]+:\/\/[^/]*/i, "");
                this.search = "";
                this.hash = "";
            }
            toString() { return this.href; }
        };
        global.URL.createObjectURL = function () { return ""; };
        global.URL.revokeObjectURL = function () {};
    }

    // --- location (Next2D が参照する場合の最小値) ---------------------------
    if (typeof global.location === "undefined") {
        global.location = {
            href: "app://local/",
            origin: "app://local",
            protocol: "app:",
            host: "local",
            hostname: "local",
            pathname: "/",
            search: "",
            hash: "",
            reload() {}
        };
    }

    // --- 既定キャンバス -----------------------------------------------------
    // Next2D は通常 document.body に canvas を追加するが、C++ 側で用意済みの
    // メインキャンバスを既定として参照できるようにしておく。
    if (global.document && global.document.__mainCanvas && !global.document.body.__hasCanvas) {
        global.document.body.__hasCanvas = true;
    }

    // --- 未処理例外/Promise 拒否の可視化 -----------------------------------
    global.addEventListener("error", function (e) {
        console.error("[unhandled error]", e && e.message ? e.message : e);
    });
    global.addEventListener("unhandledrejection", function (e) {
        console.error("[unhandled rejection]", e && e.reason ? e.reason : e);
    });

    console.log("[bootstrap] Next2D Xbox host environment ready.");

})(globalThis);
