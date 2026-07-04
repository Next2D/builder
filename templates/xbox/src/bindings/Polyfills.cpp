// 全コンテキスト (メイン + 各 worker) に必要な JS ポリフィルを評価する。
//
// bootstrap.js はメインコンテキストでのみ実行されるため、worker コンテキストには
// TextEncoder/TextDecoder 等が存在せず、Vite がバンドルした worker スクリプトが
// 評価時に ReferenceError で落ちる。ここで C++ から全コンテキストに注入する。
// (bootstrap.js 側は typeof ガード付きなので二重定義にはならない)
#include "Bindings.h"

#include "v8/V8Util.h"

namespace next2d {

namespace {

const char kPolyfillsJs[] = R"JS(
(function (global) {
    "use strict";

    if (typeof global.queueMicrotask !== "function") {
        global.queueMicrotask = function (callback) {
            Promise.resolve().then(callback);
        };
    }

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
                if (!input) { return ""; }
                const bytes = input instanceof Uint8Array
                    ? input
                    : new Uint8Array(input.buffer || input);
                let out = "";
                let i = 0;
                while (i < bytes.length) {
                    const c = bytes[i++];
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
})(globalThis);
)JS";

} // namespace

void InstallPolyfills(v8::Isolate* isolate, v8::Local<v8::Context> context)
{
    v8::Context::Scope cs(context);
    v8::TryCatch tc(isolate);
    v8::Local<v8::String> src = v8util::Str(isolate, kPolyfillsJs);
    v8::Local<v8::Script> script;
    if (v8::Script::Compile(context, src).ToLocal(&script)) {
        (void) script->Run(context);
    }
}

} // namespace next2d
