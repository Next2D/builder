// 全コンテキスト (メイン + 各 worker) に必要な JS ポリフィルを評価する。
//
// bootstrap.js はメインコンテキストでのみ実行されるため、worker コンテキストには
// TextEncoder/TextDecoder 等が存在せず、Vite がバンドルした worker スクリプトが
// 評価時に ReferenceError で落ちる。ここで C++ から全コンテキストに注入する。
// (bootstrap.js 側は typeof ガード付きなので二重定義にはならない)
//
// NOTE: MSVC は 1 つの文字列リテラルが約 16KB までのため (C2026)、
//       ポリフィルは独立した IIFE のセクション配列に分割して順に評価する。
#include "Bindings.h"

#include "v8/V8Util.h"

namespace next2d {

namespace {

// section 1: base (queueMicrotask)
const char kPolyfills1[] = R"JS(
(function (global) {
    "use strict";


    if (typeof global.queueMicrotask !== "function") {
        global.queueMicrotask = function (callback) {
            Promise.resolve().then(callback);
        };
    }
})(globalThis);
)JS";

// section 2: TextEncoder / location
const char kPolyfills2[] = R"JS(
(function (global) {
    "use strict";
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

    // location (worker を含む全コンテキストで参照可能にする。
    //  player の LoadUseCase が location.origin で URL を解決する)
    if (typeof global.location === "undefined") {
        global.location = {
            "href": "app://local/",
            "origin": "app://local",
            "protocol": "app:",
            "host": "local",
            "hostname": "local",
            "pathname": "/",
            "search": "",
            "hash": "",
            reload() { /* no-op */ }
        };
    }
})(globalThis);
)JS";

// section 3: atob / btoa
const char kPolyfills3[] = R"JS(
(function (global) {
    "use strict";
    // atob / btoa (indexedDB のバイナリ値永続化にも使用)
    const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (typeof global.btoa !== "function") {
        global.btoa = function (bin) {
            bin = String(bin);
            let out = "";
            for (let i = 0; i < bin.length; i += 3) {
                const a = bin.charCodeAt(i);
                const b = i + 1 < bin.length ? bin.charCodeAt(i + 1) : NaN;
                const c = i + 2 < bin.length ? bin.charCodeAt(i + 2) : NaN;
                if (a > 255 || b > 255 || c > 255) {
                    throw new Error("btoa: character out of range");
                }
                out += B64[a >> 2];
                out += B64[((a & 3) << 4) | (isNaN(b) ? 0 : b >> 4)];
                out += isNaN(b) ? "=" : B64[((b & 15) << 2) | (isNaN(c) ? 0 : c >> 6)];
                out += isNaN(c) ? "=" : B64[c & 63];
            }
            return out;
        };
    }
    if (typeof global.atob !== "function") {
        global.atob = function (b64) {
            b64 = String(b64).replace(/[=\s]+$/, "");
            let out = "";
            let buffer = 0;
            let bits = 0;
            for (let i = 0; i < b64.length; i++) {
                const v = B64.indexOf(b64[i]);
                if (v < 0) { continue; }
                buffer = (buffer << 6) | v;
                bits += 6;
                if (bits >= 8) {
                    bits -= 8;
                    out += String.fromCharCode((buffer >> bits) & 0xff);
                }
            }
            return out;
        };
    }
})(globalThis);
)JS";

// section 4: localStorage / sessionStorage / alert / confirm
const char kPolyfills4[] = R"JS(
(function (global) {
    "use strict";
    // localStorage / sessionStorage。
    // 永続化はホストの __next2d_storage_load/save (名前付きファイル I/O) に委ねる。
    if (typeof global.localStorage === "undefined") {
        const makeStorage = function (persistent) {
            let data = {};
            if (persistent && typeof global.__next2d_storage_load === "function") {
                try {
                    const raw = global.__next2d_storage_load("localStorage");
                    if (raw) { data = JSON.parse(raw) || {}; }
                } catch (e) { data = {}; }
            }
            const persist = function () {
                if (persistent && typeof global.__next2d_storage_save === "function") {
                    try { global.__next2d_storage_save("localStorage", JSON.stringify(data)); } catch (e) { /* 保存失敗は無視 */ }
                }
            };
            return {
                getItem(k) {
                    k = String(k);
                    return Object.prototype.hasOwnProperty.call(data, k) ? data[k] : null;
                },
                setItem(k, v) { data[String(k)] = String(v); persist(); },
                removeItem(k) { delete data[String(k)]; persist(); },
                clear() { data = {}; persist(); },
                key(i) {
                    const ks = Object.keys(data);
                    return i >= 0 && i < ks.length ? ks[i] : null;
                },
                get length() { return Object.keys(data).length; }
            };
        };
        global.localStorage = makeStorage(true);
        global.sessionStorage = makeStorage(false);
    }

    // alert / confirm (ダイアログの無い環境向け。confirm は肯定を返す)
    if (typeof global.alert !== "function") {
        global.alert = function () { /* no-op */ };
    }
    if (typeof global.confirm !== "function") {
        global.confirm = function () { return true; };
    }
})(globalThis);
)JS";

// section 5: indexedDB / TextDecoder
const char kPolyfills5[] = R"JS(
(function (global) {
    "use strict";
    // indexedDB (最小実装・ファイル永続化)。
    // ゲームの典型用途 (open → objectStore → get/put/delete/getAll) をカバーする。
    // 値は JSON 化可能なもの + ArrayBuffer/TypedArray (base64 でタグ付け永続化)。
    // カーソル/インデックス/複合キーは未対応 («EXTEND»)。
    if (typeof global.indexedDB === "undefined" &&
        typeof global.__next2d_storage_load === "function") {

        const TYPED = {
            "Int8Array": Int8Array, "Uint8Array": Uint8Array,
            "Uint8ClampedArray": Uint8ClampedArray,
            "Int16Array": Int16Array, "Uint16Array": Uint16Array,
            "Int32Array": Int32Array, "Uint32Array": Uint32Array,
            "Float32Array": Float32Array, "Float64Array": Float64Array
        };

        const bytesToB64 = function (u8) {
            let bin = "";
            for (let i = 0; i < u8.length; i += 0x8000) {
                bin += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000));
            }
            return global.btoa(bin);
        };
        const b64ToBytes = function (b64) {
            const bin = global.atob(b64);
            const u8 = new Uint8Array(bin.length);
            for (let i = 0; i < bin.length; i++) { u8[i] = bin.charCodeAt(i); }
            return u8;
        };

        const encodeValue = function (v) {
            if (v instanceof ArrayBuffer) {
                return { "__n2d_bin": bytesToB64(new Uint8Array(v)), "__n2d_type": "ArrayBuffer" };
            }
            if (ArrayBuffer.isView(v)) {
                const name = v.constructor && v.constructor.name;
                if (TYPED[name]) {
                    return {
                        "__n2d_bin": bytesToB64(new Uint8Array(v.buffer, v.byteOffset, v.byteLength)),
                        "__n2d_type": name
                    };
                }
            }
            if (Array.isArray(v)) { return v.map(encodeValue); }
            if (v && typeof v === "object") {
                const out = {};
                for (const k of Object.keys(v)) { out[k] = encodeValue(v[k]); }
                return out;
            }
            return v;
        };
        const decodeValue = function (v) {
            if (v && typeof v === "object") {
                if (typeof v.__n2d_bin === "string") {
                    const u8 = b64ToBytes(v.__n2d_bin);
                    if (v.__n2d_type === "ArrayBuffer") { return u8.buffer; }
                    const Ctor = TYPED[v.__n2d_type] || Uint8Array;
                    return new Ctor(u8.buffer, 0, u8.byteLength / Ctor.BYTES_PER_ELEMENT);
                }
                if (Array.isArray(v)) { return v.map(decodeValue); }
                const out = {};
                for (const k of Object.keys(v)) { out[k] = decodeValue(v[k]); }
                return out;
            }
            return v;
        };

        const loadDb = function (name) {
            try {
                const raw = global.__next2d_storage_load("idb_" + name);
                if (raw) { return JSON.parse(raw); }
            } catch (e) { /* 破損時は初期化 */ }
            return { "version": 0, "stores": {} };
        };
        const saveDb = function (name, db) {
            try { global.__next2d_storage_save("idb_" + name, JSON.stringify(db)); } catch (e) { /* 無視 */ }
        };

        const makeRequest = function () {
            return { "onsuccess": null, "onerror": null, "result": undefined, "error": null };
        };
        const fireSuccess = function (r) {
            global.queueMicrotask(function () {
                if (typeof r.onsuccess === "function") { r.onsuccess({ "target": r }); }
            });
        };
        const fireError = function (r, e) {
            r.error = e;
            global.queueMicrotask(function () {
                if (typeof r.onerror === "function") { r.onerror({ "target": r }); }
            });
        };

        global.indexedDB = {
            open(name, version) {
                const req = makeRequest();
                req.onupgradeneeded = null;
                req.onblocked = null;
                global.queueMicrotask(function () {
                    const data = loadDb(name);

                    const storeApi = function (storeName) {
                        const entry = data.stores[storeName] ||
                            (data.stores[storeName] = { "keyPath": null, "data": {} });
                        const run = function (fn) {
                            const r = makeRequest();
                            global.queueMicrotask(function () {
                                try { r.result = fn(); fireSuccess(r); }
                                catch (e) { fireError(r, e); }
                            });
                            return r;
                        };
                        return {
                            "keyPath": entry.keyPath,
                            get(key) {
                                return run(function () {
                                    const raw = entry.data[String(key)];
                                    return raw === undefined ? undefined : decodeValue(raw);
                                });
                            },
                            put(value, key) {
                                return run(function () {
                                    let k = key;
                                    if (k === undefined && entry.keyPath && value &&
                                        typeof value === "object") {
                                        k = value[entry.keyPath];
                                    }
                                    if (k === undefined) { throw new Error("key required"); }
                                    entry.data[String(k)] = encodeValue(value);
                                    saveDb(name, data);
                                    return k;
                                });
                            },
                            add(value, key) { return this.put(value, key); },
                            "delete": function (key) {
                                return run(function () {
                                    delete entry.data[String(key)];
                                    saveDb(name, data);
                                });
                            },
                            clear() {
                                return run(function () {
                                    entry.data = {};
                                    saveDb(name, data);
                                });
                            },
                            count() { return run(function () { return Object.keys(entry.data).length; }); },
                            getAll() {
                                return run(function () {
                                    return Object.keys(entry.data).map(function (k) {
                                        return decodeValue(entry.data[k]);
                                    });
                                });
                            },
                            getAllKeys() { return run(function () { return Object.keys(entry.data); }); }
                        };
                    };

                    const dbObj = {
                        "name": name,
                        "objectStoreNames": {
                            contains(s) { return Object.prototype.hasOwnProperty.call(data.stores, s); },
                            get length() { return Object.keys(data.stores).length; }
                        },
                        createObjectStore(storeName, options) {
                            if (!data.stores[storeName]) {
                                data.stores[storeName] = {
                                    "keyPath": options && options.keyPath ? options.keyPath : null,
                                    "data": {}
                                };
                                saveDb(name, data);
                            }
                            return storeApi(storeName);
                        },
                        deleteObjectStore(storeName) {
                            delete data.stores[storeName];
                            saveDb(name, data);
                        },
                        transaction(_names, _mode) {
                            const tx = {
                                "oncomplete": null, "onerror": null, "onabort": null,
                                objectStore(s) { return storeApi(s); },
                                abort() { /* 未対応 */ },
                                commit() { /* 自動コミット */ }
                            };
                            // 全リクエストのマイクロタスク消化後に complete を発火
                            global.queueMicrotask(function () {
                                global.queueMicrotask(function () {
                                    global.queueMicrotask(function () {
                                        if (typeof tx.oncomplete === "function") {
                                            tx.oncomplete({ "target": tx });
                                        }
                                    });
                                });
                            });
                            return tx;
                        },
                        close() { /* no-op */ },
                        "onversionchange": null
                    };

                    const oldVersion = data.version || 0;
                    const newVersion = typeof version === "number" ? version : Math.max(1, oldVersion);
                    req.result = dbObj;
                    if (newVersion > oldVersion) {
                        data.version = newVersion;
                        if (typeof req.onupgradeneeded === "function") {
                            req.onupgradeneeded({
                                "target": req,
                                "oldVersion": oldVersion,
                                "newVersion": newVersion
                            });
                        }
                        saveDb(name, data);
                    }
                    if (typeof req.onsuccess === "function") {
                        req.onsuccess({ "target": req });
                    }
                });
                return req;
            },
            deleteDatabase(name) {
                const req = makeRequest();
                global.queueMicrotask(function () {
                    try {
                        global.__next2d_storage_save("idb_" + name, "");
                        fireSuccess(req);
                    } catch (e) {
                        fireError(req, e);
                    }
                });
                return req;
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

const char* const kPolyfillSections[] = {
    kPolyfills1,
    kPolyfills2,
    kPolyfills3,
    kPolyfills4,
    kPolyfills5,
};

} // namespace

void InstallPolyfills(v8::Isolate* isolate, v8::Local<v8::Context> context)
{
    v8::Context::Scope cs(context);
    for (const char* source : kPolyfillSections) {
        v8::TryCatch tc(isolate);
        v8::Local<v8::String> src = v8util::Str(isolate, source);
        v8::Local<v8::Script> script;
        if (v8::Script::Compile(context, src).ToLocal(&script)) {
            (void) script->Run(context);
        }
    }
}

} // namespace next2d
