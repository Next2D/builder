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

    // history (framework の gotoView が SPA 設定時に history.pushState を呼ぶ)。
    // pushState/replaceState は location の pathname/search/hash にも反映する
    if (typeof global.history === "undefined") {
        const applyUrl = (url) => {
            if (typeof url !== "string" || url === "") {
                return;
            }
            let rest = url;
            const origin = global.location.origin;
            if (rest.indexOf(origin) === 0) {
                rest = rest.slice(origin.length);
            }
            if (rest === "" || rest[0] !== "/") {
                rest = "/" + rest;
            }
            let hash = "";
            const hi = rest.indexOf("#");
            if (hi >= 0) {
                hash = rest.slice(hi);
                rest = rest.slice(0, hi);
            }
            let search = "";
            const qi = rest.indexOf("?");
            if (qi >= 0) {
                search = rest.slice(qi);
                rest = rest.slice(0, qi);
            }
            global.location.pathname = rest;
            global.location.search = search;
            global.location.hash = hash;
            global.location.href = origin + rest + search + hash;
        };
        global.history = {
            "state": null,
            "length": 1,
            pushState(state, _title, url) {
                this.state = state;
                this.length++;
                applyUrl(url);
            },
            replaceState(state, _title, url) {
                this.state = state;
                applyUrl(url);
            },
            back() { /* no-op */ },
            forward() { /* no-op */ },
            go() { /* no-op */ }
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

// section 5: TextDecoder
const char kPolyfills5[] = R"JS(
(function (global) {
    "use strict";
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

// section 6: DOM 要素ツリー (parentElement / children / getElementById / Event)。
// player の boot は document.body.appendChild(div) 後に div.parentElement.tagName を
// 参照し、無いと throw する (async 起動フロー内のため無報告で停止していた)。
// canvas/video はネイティブ実装 (C++) をそのまま使い、他タグを JS で本格化する。
const char kPolyfills6[] = R"JS(
(function (global) {
    "use strict";
    var doc = global.document;
    if (!doc || doc.__domUpgraded) {
        return;
    }
    doc.__domUpgraded = true;

    if (typeof global.Event !== "function") {
        var Ev = function (type, init) {
            init = init || {};
            this.type = String(type);
            this.bubbles = !!init.bubbles;
            this.cancelable = !!init.cancelable;
            this.defaultPrevented = false;
            this.target = null;
            this.currentTarget = null;
        };
        Ev.prototype.preventDefault = function () {
            if (this.cancelable) { this.defaultPrevented = true; }
        };
        Ev.prototype.stopPropagation = function () {};
        Ev.prototype.stopImmediatePropagation = function () {};
        global.Event = Ev;
    }
    if (typeof global.CustomEvent !== "function") {
        global.CustomEvent = function (type, init) {
            var e = new global.Event(type, init);
            e.detail = init && init.detail !== undefined ? init.detail : null;
            return e;
        };
    }

    function styleSize(el, key) {
        var v = el.style && el.style[key];
        var n = parseFloat(v);
        if (isFinite(n) && n > 0) { return n; }
        var attrs = el.__attrs;
        if (attrs && typeof attrs.style === "string") {
            var m = attrs.style.match(
                new RegExp("(?:^|;)\\s*" + key + "\\s*:\\s*([0-9.]+)px")
            );
            if (m) { return parseFloat(m[1]); }
        }
        if (el.tagName === "BODY" || el.tagName === "HTML") {
            return key === "width" ? global.innerWidth : global.innerHeight;
        }
        return 0;
    }

    // el を DOM 要素相当へ拡張する。addEventListener はネイティブ実装
    // (__listeners: ホストの入力配送先) が既にある場合は温存する。
    function upgrade(el, tag) {
        tag = String(tag || "div").toLowerCase();
        el.tagName = tag.toUpperCase();
        el.localName = tag;
        if (el.id === undefined) { el.id = ""; }
        if (!el.style) { el.style = {}; }
        if (!el.__attrs) { el.__attrs = {}; }
        el.children = [];
        el.childNodes = el.children;
        el.parentElement = null;
        el.parentNode = null;
        el.dataset = {};

        Object.defineProperty(el, "firstChild", {
            get: function () { return this.children.length ? this.children[0] : null; },
            configurable: true
        });
        Object.defineProperty(el, "lastChild", {
            get: function () {
                var c = this.children;
                return c.length ? c[c.length - 1] : null;
            },
            configurable: true
        });
        Object.defineProperty(el, "clientWidth", {
            get: function () { return styleSize(this, "width"); },
            configurable: true
        });
        Object.defineProperty(el, "clientHeight", {
            get: function () { return styleSize(this, "height"); },
            configurable: true
        });

        var html = "";
        Object.defineProperty(el, "innerHTML", {
            get: function () { return html; },
            set: function (v) { html = String(v); this.children.length = 0; },
            configurable: true
        });

        el.appendChild = function (child) {
            if (child.parentElement && child.parentElement.children) {
                var cs = child.parentElement.children;
                var i = cs.indexOf(child);
                if (i >= 0) { cs.splice(i, 1); }
            }
            this.children.push(child);
            child.parentElement = this;
            child.parentNode = this;
            return child;
        };
        el.removeChild = function (child) {
            var i = this.children.indexOf(child);
            if (i >= 0) { this.children.splice(i, 1); }
            child.parentElement = null;
            child.parentNode = null;
            return child;
        };
        el.insertBefore = function (child, ref) {
            var i = ref ? this.children.indexOf(ref) : -1;
            if (i < 0) { return this.appendChild(child); }
            this.children.splice(i, 0, child);
            child.parentElement = this;
            child.parentNode = this;
            return child;
        };
        el.contains = function (node) {
            if (node === this) { return true; }
            for (var i = 0; i < this.children.length; i++) {
                var c = this.children[i];
                if (c === node || (c.contains && c.contains(node))) { return true; }
            }
            return false;
        };
        el.remove = function () {
            if (this.parentElement) { this.parentElement.removeChild(this); }
        };
        el.setAttribute = function (name, value) {
            this.__attrs[name] = String(value);
            if (name === "id") { this.id = String(value); }
        };
        el.getAttribute = function (name) {
            return this.__attrs[name] !== undefined ? this.__attrs[name] : null;
        };
        el.removeAttribute = function (name) { delete this.__attrs[name]; };
        el.getBoundingClientRect = function () {
            var w = this.clientWidth;
            var h = this.clientHeight;
            return { x: 0, y: 0, top: 0, left: 0, right: w, bottom: h, width: w, height: h };
        };
        el.focus = function () {};
        el.blur = function () {};

        if (typeof el.addEventListener !== "function") {
            var listeners = {};
            el.addEventListener = function (type, fn) {
                (listeners[type] || (listeners[type] = [])).push(fn);
            };
            el.removeEventListener = function (type, fn) {
                var a = listeners[type];
                if (!a) { return; }
                var i = a.indexOf(fn);
                if (i >= 0) { a.splice(i, 1); }
            };
            el.dispatchEvent = function (ev) {
                ev.target = ev.target || this;
                ev.currentTarget = this;
                var a = listeners[ev.type];
                if (a) {
                    for (var i = 0; i < a.length; i++) {
                        try { a[i].call(this, ev); } catch (e) {
                            if (global.console) { global.console.error(e); }
                        }
                    }
                }
                var on = this["on" + ev.type];
                if (typeof on === "function") {
                    try { on.call(this, ev); } catch (e2) {
                        if (global.console) { global.console.error(e2); }
                    }
                }
                return !ev.defaultPrevented;
            };
        }
        return el;
    }

    var nativeCreate = doc.createElement.bind(doc);
    doc.createElement = function (tag) {
        var t = String(tag).toLowerCase();
        if (t === "canvas" || t === "video") {
            var n = nativeCreate(t);
            if (!n.localName) {
                n.localName = t;
                n.tagName = t.toUpperCase();
            }
            if (n.parentElement === undefined) {
                n.parentElement = null;
                n.parentNode = null;
            }
            return n;
        }
        return upgrade({}, t);
    };

    // body / documentElement (同一オブジェクト)。ネイティブの addEventListener
    // は upgrade 内で温存される。
    var body = doc.body;
    if (body) {
        upgrade(body, "body");
        Object.defineProperty(body, "clientWidth", {
            get: function () { return global.innerWidth; },
            configurable: true
        });
        Object.defineProperty(body, "clientHeight", {
            get: function () { return global.innerHeight; },
            configurable: true
        });
    }

    doc.getElementById = function (id) {
        id = String(id);
        var find = function (el) {
            if (el.id === id) { return el; }
            var cs = el.children || [];
            for (var i = 0; i < cs.length; i++) {
                var r = find(cs[i]);
                if (r) { return r; }
            }
            return null;
        };
        return body ? find(body) : null;
    };

    // window.addEventListener("resize", ...) 用 (main コンテキストの global)
    if (typeof global.addEventListener !== "function") {
        var winListeners = {};
        global.addEventListener = function (type, fn) {
            (winListeners[type] || (winListeners[type] = [])).push(fn);
        };
        global.removeEventListener = function (type, fn) {
            var a = winListeners[type];
            if (!a) { return; }
            var i = a.indexOf(fn);
            if (i >= 0) { a.splice(i, 1); }
        };
        global.dispatchEvent = function (ev) {
            var a = winListeners[ev.type];
            if (a) {
                for (var i = 0; i < a.length; i++) {
                    try { a[i].call(global, ev); } catch (e) {
                        if (global.console) { global.console.error(e); }
                    }
                }
            }
            return true;
        };
    }
})(globalThis);
)JS";

// section 7: indexedDB (完全実装: cursor/index/複合キー/IDBKeyRange/autoIncrement)。
// 31KB あり MSVC の文字列リテラル上限(~16KB)を超えるため 3 リテラルに分割し、
// InstallPolyfills で連結して 1 スクリプトとして評価する。atob/btoa(section3)と
// queueMicrotask(section4)に依存するため、それらの後で評価すること。
const char kIndexedDB1[] = R"JS(
(function (global) {
    "use strict";
    // indexedDB (ファイル永続化・単一スレッド同期ストア + 非同期 API 面)。
    // 対応: open/upgrade, objectStore(keyPath / 複合keyPath / autoIncrement),
    //       get/getKey/getAll/getAllKeys/count/put/add/delete/clear,
    //       createIndex/index/deleteIndex(unique/multiEntry),
    //       openCursor/openKeyCursor(next/nextunique/prev/prevunique, IDBKeyRange),
    //       IDBKeyRange(only/lowerBound/upperBound/bound)。
    // 値は JSON 化可能なもの + ArrayBuffer/TypedArray (base64 タグ付け)。
    if (typeof global.indexedDB !== "undefined" ||
        typeof global.__next2d_storage_load !== "function") {
        return;
    }

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

    // --- キー順序 (IndexedDB 仕様: number < string < array) -------------------
    const keyType = function (k) {
        if (Array.isArray(k)) { return 3; }
        if (typeof k === "number") { return 0; }
        if (typeof k === "string") { return 2; }
        return -1;
    };
    const cmpKeys = function (a, b) {
        const ta = keyType(a), tb = keyType(b);
        if (ta !== tb) { return ta < tb ? -1 : 1; }
        if (ta === 3) {
            const n = Math.min(a.length, b.length);
            for (let i = 0; i < n; i++) {
                const c = cmpKeys(a[i], b[i]);
                if (c !== 0) { return c; }
            }
            return a.length === b.length ? 0 : (a.length < b.length ? -1 : 1);
        }
        if (a < b) { return -1; }
        if (a > b) { return 1; }
        return 0;
    };
    const isValidKey = function (k) { return keyType(k) !== -1; };
    // ストレージのマップ用一意文字列
    const encKey = function (k) { return JSON.stringify(k); };

    // objectStoreNames / indexNames は配列 + DOMStringList 互換 (contains/item)。
    const nameList = function (names) {
        names.sort();
        names.contains = function (n) { return names.indexOf(n) !== -1; };
        names.item = function (i) { return names[i] != null ? names[i] : null; };
        return names;
    };

    // --- keyPath 抽出 (ネスト "a.b" / 複合 ["a","b"]) -------------------------
    const evalPath = function (value, path) {
        if (path === "") { return value; }
        const parts = path.split(".");
        let cur = value;
        for (let i = 0; i < parts.length; i++) {
            if (cur == null || typeof cur !== "object") { return undefined; }
            cur = cur[parts[i]];
        }
        return cur;
    };
    const extractKey = function (value, keyPath) {
        if (keyPath == null) { return undefined; }
        if (Array.isArray(keyPath)) {
            const out = [];
            for (let i = 0; i < keyPath.length; i++) {
                const k = evalPath(value, keyPath[i]);
                if (k === undefined) { return undefined; }
                out.push(k);
            }
            return out;
        }
        return evalPath(value, keyPath);
    };
    // in-line key を value へ書き戻す (autoIncrement 生成キー用、ネスト対応)
    const injectKey = function (value, keyPath, key) {
        if (typeof keyPath !== "string" || value == null || typeof value !== "object") { return; }
        const parts = keyPath.split(".");
        let cur = value;
        for (let i = 0; i < parts.length - 1; i++) {
            if (cur[parts[i]] == null || typeof cur[parts[i]] !== "object") { cur[parts[i]] = {}; }
            cur = cur[parts[i]];
        }
        cur[parts[parts.length - 1]] = key;
    };

    // --- IDBKeyRange --------------------------------------------------------
    const makeRange = function (lower, upper, lowerOpen, upperOpen) {
        return {
            "lower": lower, "upper": upper,
            "lowerOpen": !!lowerOpen, "upperOpen": !!upperOpen,
            includes(key) { return rangeIncludes(this, key); }
        };
    };
    const rangeIncludes = function (range, key) {
        if (range == null) { return true; }
        // 素のキー (range でない) は only 相当
        if (typeof range !== "object" || !("lower" in range)) {
            return cmpKeys(key, range) === 0;
        }
        if (range.lower !== undefined) {
            const c = cmpKeys(key, range.lower);
            if (c < 0 || (c === 0 && range.lowerOpen)) { return false; }
        }
        if (range.upper !== undefined) {
            const c = cmpKeys(key, range.upper);
            if (c > 0 || (c === 0 && range.upperOpen)) { return false; }
        }
        return true;
    };
    global.IDBKeyRange = {
        only(v) { return makeRange(v, v, false, false); },
        lowerBound(v, open) { return makeRange(v, undefined, open, false); },
        upperBound(v, open) { return makeRange(undefined, v, false, open); },
        bound(l, u, lo, uo) { return makeRange(l, u, lo, uo); }
    };

    // --- 永続化 -------------------------------------------------------------
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

    // --- 非同期 request/event ----------------------------------------------
    const makeRequest = function () {
        return {
            "onsuccess": null, "onerror": null, "onupgradeneeded": null, "onblocked": null,
            "result": undefined, "error": null, "readyState": "pending", "source": null,
            "transaction": null
        };
    };
    const fireSuccess = function (r) {
        global.queueMicrotask(function () {
            r.readyState = "done";
            if (typeof r.onsuccess === "function") { r.onsuccess({ "target": r }); }
        });
    };
    const fireError = function (r, e) {
        r.error = e;
        global.queueMicrotask(function () {
            r.readyState = "done";
            if (typeof r.onerror === "function") { r.onerror({ "target": r }); }
        });
    };

    global.indexedDB = {
        open(name, version) {
            const req = makeRequest();
            global.queueMicrotask(function () {
                const data = loadDb(name);

)JS";
const char kIndexedDB2[] = R"JS(
                // records: encKey -> { "key": rawKey, "value": encodedValue }
                const storeApi = function (storeName, tx) {
                    const entry = data.stores[storeName];
                    if (!entry) { throw new Error("NotFoundError: object store '" + storeName + "'"); }
                    if (!entry.records) { entry.records = {}; }
                    if (!entry.indexes) { entry.indexes = {}; }

                    const run = function (fn) {
                        const r = makeRequest();
                        r.transaction = tx;
                        global.queueMicrotask(function () {
                            try { r.result = fn(); fireSuccess(r); }
                            catch (e) { fireError(r, e); }
                        });
                        return r;
                    };

                    const allRecords = function () {
                        return Object.keys(entry.records).map(function (ek) {
                            return entry.records[ek];
                        });
                    };
                    const sortedRecords = function () {
                        return allRecords().sort(function (a, b) { return cmpKeys(a.key, b.key); });
                    };

                    const resolveKey = function (value, key) {
                        let k = key;
                        if (k === undefined && entry.keyPath != null) {
                            k = extractKey(value, entry.keyPath);
                        }
                        if (k === undefined && entry.autoIncrement) {
                            entry.keyGen = (entry.keyGen || 0) + 1;
                            k = entry.keyGen;
                            if (typeof entry.keyPath === "string") { injectKey(value, entry.keyPath, k); }
                        } else if (typeof k === "number" && entry.autoIncrement && k > (entry.keyGen || 0)) {
                            entry.keyGen = Math.floor(k);
                        }
                        return k;
                    };

                    const putImpl = function (value, key, noOverwrite) {
                        const k = resolveKey(value, key);
                        if (k === undefined || !isValidKey(k)) { throw new Error("DataError: invalid key"); }
                        const ek = encKey(k);
                        if (noOverwrite && Object.prototype.hasOwnProperty.call(entry.records, ek)) {
                            throw new Error("ConstraintError: key already exists");
                        }
                        entry.records[ek] = { "key": k, "value": encodeValue(value) };
                        saveDb(name, data);
                        return k;
                    };

                    // カーソル生成 (records: 配列 [{key, primaryKey, value(encoded)}])
                    const makeCursor = function (req2, source, records, direction, keyOnly) {
                        const dir = direction || "next";
                        records.sort(function (a, b) {
                            return cmpKeys(a.key, b.key) || cmpKeys(a.primaryKey, b.primaryKey);
                        });
                        if (dir === "prev" || dir === "prevunique") { records.reverse(); }
                        if (dir === "nextunique" || dir === "prevunique") {
                            const seen = [];
                            const uniq = [];
                            for (let i = 0; i < records.length; i++) {
                                const ek = encKey(records[i].key);
                                if (seen.indexOf(ek) === -1) { seen.push(ek); uniq.push(records[i]); }
                            }
                            records = uniq;
                        }
                        let pos = -1;
                        const cursor = {
                            "direction": dir,
                            "source": source,
                            get key() { return pos >= 0 && pos < records.length ? records[pos].key : undefined; },
                            get primaryKey() { return pos >= 0 && pos < records.length ? records[pos].primaryKey : undefined; },
                            get value() {
                                if (keyOnly || pos < 0 || pos >= records.length) { return undefined; }
                                return decodeValue(records[pos].value);
                            },
                            continue(key) {
                                if (key === undefined) { pos++; }
                                else {
                                    const forward = (dir === "next" || dir === "nextunique");
                                    let np = pos + 1;
                                    while (np < records.length) {
                                        const c = cmpKeys(records[np].key, key);
                                        if ((forward && c >= 0) || (!forward && c <= 0)) { break; }
                                        np++;
                                    }
                                    pos = np;
                                }
                                step();
                            },
                            advance(count) { pos += (count > 0 ? count : 1); step(); },
                            "delete": function () {
                                const rec = records[pos];
                                return run(function () {
                                    if (rec) { delete entry.records[encKey(rec.primaryKey)]; saveDb(name, data); }
                                });
                            },
                            update(value) {
                                const rec = records[pos];
                                return run(function () {
                                    if (!rec) { throw new Error("InvalidStateError"); }
                                    entry.records[encKey(rec.primaryKey)] = {
                                        "key": rec.primaryKey, "value": encodeValue(value)
                                    };
                                    saveDb(name, data);
                                    return rec.primaryKey;
                                });
                            }
                        };
                        const step = function () {
                            if (pos >= 0 && pos < records.length) { req2.result = cursor; }
                            else { req2.result = null; }
                            fireSuccess(req2);
                        };
                        // カーソルを最初のレコードへ位置づけて success を発火する
                        pos++;
                        step();
                    };

                    const api = {
                        "name": storeName,
                        "keyPath": entry.keyPath != null ? entry.keyPath : null,
                        "autoIncrement": !!entry.autoIncrement,
                        get indexNames() { return nameList(Object.keys(entry.indexes)); },
                        "transaction": tx,
                        get(key) {
                            return run(function () {
                                const recs = sortedRecords();
                                for (let i = 0; i < recs.length; i++) {
                                    if (rangeIncludes(key, recs[i].key)) { return decodeValue(recs[i].value); }
                                }
                                return undefined;
                            });
                        },
                        getKey(key) {
                            return run(function () {
                                const recs = sortedRecords();
                                for (let i = 0; i < recs.length; i++) {
                                    if (rangeIncludes(key, recs[i].key)) { return recs[i].key; }
                                }
                                return undefined;
                            });
                        },
                        put(value, key) { return run(function () { return putImpl(value, key, false); }); },
                        add(value, key) { return run(function () { return putImpl(value, key, true); }); },
                        "delete": function (key) {
                            return run(function () {
                                const recs = sortedRecords();
                                for (let i = 0; i < recs.length; i++) {
                                    if (rangeIncludes(key, recs[i].key)) { delete entry.records[encKey(recs[i].key)]; }
                                }
                                saveDb(name, data);
                            });
                        },
                        clear() { return run(function () { entry.records = {}; saveDb(name, data); }); },
                        count(key) {
                            return run(function () {
                                const recs = sortedRecords();
                                let n = 0;
                                for (let i = 0; i < recs.length; i++) {
                                    if (rangeIncludes(key, recs[i].key)) { n++; }
                                }
                                return n;
                            });
                        },
                        getAll(query, count) {
                            return run(function () {
                                const out = [];
                                const recs = sortedRecords();
                                for (let i = 0; i < recs.length; i++) {
                                    if (rangeIncludes(query, recs[i].key)) { out.push(decodeValue(recs[i].value)); }
                                    if (count && out.length >= count) { break; }
                                }
                                return out;
                            });
                        },
                        getAllKeys(query, count) {
                            return run(function () {
                                const out = [];
                                const recs = sortedRecords();
                                for (let i = 0; i < recs.length; i++) {
                                    if (rangeIncludes(query, recs[i].key)) { out.push(recs[i].key); }
                                    if (count && out.length >= count) { break; }
                                }
                                return out;
                            });
                        },
                        openCursor(query, direction) {
                            const r = makeRequest();
                            r.transaction = tx;
                            global.queueMicrotask(function () {
                                const recs = sortedRecords()
                                    .filter(function (rec) { return rangeIncludes(query, rec.key); })
                                    .map(function (rec) {
                                        return { "key": rec.key, "primaryKey": rec.key, "value": rec.value };
                                    });
                                makeCursor(r, api, recs, direction, false);
                            });
                            return r;
                        },
                        openKeyCursor(query, direction) {
                            const r = makeRequest();
                            r.transaction = tx;
                            global.queueMicrotask(function () {
                                const recs = sortedRecords()
                                    .filter(function (rec) { return rangeIncludes(query, rec.key); })
                                    .map(function (rec) {
                                        return { "key": rec.key, "primaryKey": rec.key, "value": rec.value };
                                    });
                                makeCursor(r, api, recs, direction, true);
                            });
)JS";
const char kIndexedDB3[] = R"JS(
                            return r;
                        },
                        createIndex(indexName, keyPath, options) {
                            entry.indexes[indexName] = {
                                "keyPath": keyPath,
                                "unique": !!(options && options.unique),
                                "multiEntry": !!(options && options.multiEntry)
                            };
                            saveDb(name, data);
                            return indexApi(indexName);
                        },
                        deleteIndex(indexName) {
                            delete entry.indexes[indexName];
                            saveDb(name, data);
                        },
                        index(indexName) { return indexApi(indexName); }
                    };

                    // インデックス: レコードを走査してインデックスキーを抽出する。
                    const indexApi = function (indexName) {
                        const meta = entry.indexes[indexName];
                        if (!meta) { throw new Error("NotFoundError: index '" + indexName + "'"); }
                        // [{ indexKey, primaryKey, value }] を multiEntry 展開込みで生成
                        const indexRecords = function () {
                            const out = [];
                            const recs = allRecords();
                            for (let i = 0; i < recs.length; i++) {
                                const decoded = decodeValue(recs[i].value);
                                let ik = extractKey(decoded, meta.keyPath);
                                if (ik === undefined) { continue; }
                                if (meta.multiEntry && Array.isArray(ik)) {
                                    for (let j = 0; j < ik.length; j++) {
                                        if (isValidKey(ik[j])) {
                                            out.push({ "key": ik[j], "primaryKey": recs[i].key, "value": recs[i].value });
                                        }
                                    }
                                } else if (isValidKey(ik)) {
                                    out.push({ "key": ik, "primaryKey": recs[i].key, "value": recs[i].value });
                                }
                            }
                            out.sort(function (a, b) {
                                return cmpKeys(a.key, b.key) || cmpKeys(a.primaryKey, b.primaryKey);
                            });
                            return out;
                        };
                        return {
                            "name": indexName,
                            "keyPath": meta.keyPath,
                            "unique": meta.unique,
                            "multiEntry": meta.multiEntry,
                            "objectStore": api,
                            get(key) {
                                return run(function () {
                                    const recs = indexRecords();
                                    for (let i = 0; i < recs.length; i++) {
                                        if (rangeIncludes(key, recs[i].key)) { return decodeValue(recs[i].value); }
                                    }
                                    return undefined;
                                });
                            },
                            getKey(key) {
                                return run(function () {
                                    const recs = indexRecords();
                                    for (let i = 0; i < recs.length; i++) {
                                        if (rangeIncludes(key, recs[i].key)) { return recs[i].primaryKey; }
                                    }
                                    return undefined;
                                });
                            },
                            getAll(query, count) {
                                return run(function () {
                                    const out = [];
                                    const recs = indexRecords();
                                    for (let i = 0; i < recs.length; i++) {
                                        if (rangeIncludes(query, recs[i].key)) { out.push(decodeValue(recs[i].value)); }
                                        if (count && out.length >= count) { break; }
                                    }
                                    return out;
                                });
                            },
                            getAllKeys(query, count) {
                                return run(function () {
                                    const out = [];
                                    const recs = indexRecords();
                                    for (let i = 0; i < recs.length; i++) {
                                        if (rangeIncludes(query, recs[i].key)) { out.push(recs[i].primaryKey); }
                                        if (count && out.length >= count) { break; }
                                    }
                                    return out;
                                });
                            },
                            count(key) {
                                return run(function () {
                                    const recs = indexRecords();
                                    let n = 0;
                                    for (let i = 0; i < recs.length; i++) {
                                        if (rangeIncludes(key, recs[i].key)) { n++; }
                                    }
                                    return n;
                                });
                            },
                            openCursor(query, direction) {
                                const r = makeRequest();
                                r.transaction = tx;
                                global.queueMicrotask(function () {
                                    const recs = indexRecords().filter(function (rec) {
                                        return rangeIncludes(query, rec.key);
                                    });
                                    makeCursor(r, this, recs, direction, false);
                                });
                                return r;
                            },
                            openKeyCursor(query, direction) {
                                const r = makeRequest();
                                r.transaction = tx;
                                global.queueMicrotask(function () {
                                    const recs = indexRecords().filter(function (rec) {
                                        return rangeIncludes(query, rec.key);
                                    });
                                    makeCursor(r, this, recs, direction, true);
                                });
                                return r;
                            }
                        };
                    };

                    return api;
                };

                const dbObj = {
                    "name": name,
                    "version": 0,
                    get objectStoreNames() { return nameList(Object.keys(data.stores)); },
                    createObjectStore(storeName, options) {
                        if (!data.stores[storeName]) {
                            data.stores[storeName] = {
                                "keyPath": options && options.keyPath !== undefined ? options.keyPath : null,
                                "autoIncrement": !!(options && options.autoIncrement),
                                "records": {}, "indexes": {}, "keyGen": 0
                            };
                            saveDb(name, data);
                        }
                        return storeApi(storeName, currentTx);
                    },
                    deleteObjectStore(storeName) {
                        delete data.stores[storeName];
                        saveDb(name, data);
                    },
                    transaction(_names, _mode) {
                        const tx = {
                            "oncomplete": null, "onerror": null, "onabort": null,
                            "db": dbObj, "mode": _mode || "readonly", "error": null,
                            objectStore(s) { return storeApi(s, tx); },
                            abort() { /* 単純化: ロールバック無し */ },
                            commit() { /* 自動コミット */ }
                        };
                        // 全リクエストのマイクロタスク消化後に complete
                        let depth = 0;
                        const settle = function () {
                            if (depth++ < 4) { global.queueMicrotask(settle); return; }
                            if (typeof tx.oncomplete === "function") { tx.oncomplete({ "target": tx }); }
                        };
                        global.queueMicrotask(settle);
                        return tx;
                    },
                    close() { /* no-op */ },
                    "onversionchange": null, "onabort": null, "onerror": null, "onclose": null
                };

                const oldVersion = data.version || 0;
                const newVersion = typeof version === "number" ? version : Math.max(1, oldVersion);
                dbObj.version = newVersion;
                let currentTx = null;

                req.result = dbObj;
                req.readyState = "done";
                if (newVersion > oldVersion) {
                    data.version = newVersion;
                    // upgrade トランザクション (versionchange)
                    currentTx = {
                        "oncomplete": null, "onerror": null, "onabort": null,
                        "db": dbObj, "mode": "versionchange", "error": null,
                        objectStore(s) { return storeApi(s, currentTx); },
                        abort() {}, commit() {}
                    };
                    if (typeof req.onupgradeneeded === "function") {
                        req.onupgradeneeded({
                            "target": req, "oldVersion": oldVersion, "newVersion": newVersion
                        });
                    }
                    saveDb(name, data);
                    currentTx = null;
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
        },
        cmp(a, b) { return cmpKeys(a, b); }
    };
})(globalThis);
)JS";
const char* const kIndexedDBChunks[] = {
    kIndexedDB1, kIndexedDB2, kIndexedDB3,
};

const char* const kPolyfillSections[] = {
    kPolyfills1,
    kPolyfills2,
    kPolyfills3,
    kPolyfills4,
    kPolyfills5,
    kPolyfills6,
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

    // indexedDB は 3 リテラルを連結して 1 スクリプトとして評価する
    // (単一 IIFE のため分割評価できない)。
    {
        std::string idb;
        for (const char* chunk : kIndexedDBChunks) {
            idb += chunk;
        }
        v8::TryCatch tc(isolate);
        v8::Local<v8::String> src = v8util::Str(isolate, idb);
        v8::Local<v8::Script> script;
        if (v8::Script::Compile(context, src).ToLocal(&script)) {
            (void) script->Run(context);
        }
    }
}

} // namespace next2d
