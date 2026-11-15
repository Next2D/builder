#include "Bindings.h"

#include "HostContext.h"
#include "EventTarget.h"
#include "v8/V8Util.h"

#ifdef _WIN32
#include <Windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <random>
#endif

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace next2d {

using v8util::Str;
using v8util::SetMethod;
using v8util::SetValue;

namespace {

// document.createElement(tag)
void CreateElement(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    HostContext* host = HostContext::From(isolate);
    const std::string tag = args.Length() > 0 ? v8util::ToStdString(isolate, args[0]) : "";

    if (tag == "canvas") {
        args.GetReturnValue().Set(CreateCanvasElement(isolate, host, 0, 0));
        return;
    }
    if (tag == "video") {
        args.GetReturnValue().Set(CreateVideoElement(isolate, host));
        return;
    }

    // その他の要素は最小スタブ (style/子要素操作/イベントを no-op で備える)
    v8::Local<v8::Object> el = v8::Object::New(isolate);
    SetValue(isolate, el, "style", v8::Object::New(isolate));
    SetValue(isolate, el, "tagName", Str(isolate, tag));
    SetMethod(isolate, el, "appendChild", [](const v8::FunctionCallbackInfo<v8::Value>& a) {
        if (a.Length() > 0) a.GetReturnValue().Set(a[0]);
    });
    SetMethod(isolate, el, "removeChild", [](const v8::FunctionCallbackInfo<v8::Value>&) {});
    SetMethod(isolate, el, "setAttribute", [](const v8::FunctionCallbackInfo<v8::Value>&) {});
    SetMethod(isolate, el, "addEventListener", [](const v8::FunctionCallbackInfo<v8::Value>&) {});
    SetMethod(isolate, el, "removeEventListener", [](const v8::FunctionCallbackInfo<v8::Value>&) {});
    args.GetReturnValue().Set(el);
}

// window/document/body 用の EventTarget を設置する (実ディスパッチ対応)。
void NoopEventTarget(v8::Isolate* isolate, v8::Local<v8::Object> obj)
{
    InstallEventTarget(isolate, obj);
}

} // namespace

namespace {

// ---- OS に依存する部分 ------------------------------------------------------
// クリップボード、乱数、名前付きストレージの保存先だけが OS ごとに異なる。
// Windows 以外では、クリップボードは空として扱い、乱数は標準ライブラリから取り、
// ストレージは埋め込み側が渡した保存先 (HostContext::storage_dir) を使う。

#ifdef _WIN32

std::string ReadClipboardText()
{
    std::string text;
    if (OpenClipboard(nullptr)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            auto* w = static_cast<const wchar_t*>(GlobalLock(h));
            if (w) {
                int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
                if (n > 0) {
                    text.resize(n - 1);
                    WideCharToMultiByte(CP_UTF8, 0, w, -1, text.data(), n, nullptr, nullptr);
                }
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    }
    return text;
}

void WriteClipboardText(const std::string& s)
{
    if (!OpenClipboard(nullptr)) {
        return;
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n > 0) {
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, static_cast<size_t>(n) * sizeof(wchar_t));
        if (h) {
            auto* w = static_cast<wchar_t*>(GlobalLock(h));
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w, n);
            GlobalUnlock(h);
            EmptyClipboard();
            SetClipboardData(CF_UNICODETEXT, h);
        }
    }
    CloseClipboard();
}

void FillRandom(uint8_t* bytes, size_t size)
{
    BCryptGenRandom(nullptr, bytes, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

// 保存先の既定: %LOCALAPPDATA%\Next2D (PC / Gaming.Desktop / CI)。
//
// コンソール実機 (Gaming.Xbox.*) のストレージ対応 (要 devkit 検証):
//   - localStorage / indexedDB のようなタイトルローカル永続化は XPersistentLocalStorage
//     (XPersistentLocalStorageGetPath で得たパス配下へ同じファイル I/O) が対応先。
//     同期 API のため現行の同期ストレージ契約 (StorageLoad/StorageSave) にそのまま載る。
//   - ユーザー毎のクラウド同期セーブが要る場合のみ XGameSave (非同期・XUser 必須) を使う。
// いずれも console GDK ヘッダと signed-in user / SCID 構成が必要で、devkit 実機でしか
// コンパイル・実行検証できないため未実装 (現状はファイルパスのフォールバックで動作)。
std::filesystem::path DefaultStorageDirectory()
{
    wchar_t base[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) {
        return {};   // フォールバック: カレント
    }
    return std::filesystem::path(base) / L"Next2D";
}

#else // !_WIN32

std::string ReadClipboardText()
{
    return {};
}

void WriteClipboardText(const std::string&) {}

void FillRandom(uint8_t* bytes, size_t size)
{
    std::random_device device;
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<uint8_t>(device());
    }
}

// 既定の保存先は無い。埋め込み側が storage_dir を渡さなければ、保存せずメモリ上に持つ。
std::filesystem::path DefaultStorageDirectory()
{
    return {};
}

#endif // _WIN32

// navigator.clipboard.readText() -> Promise<string>
void ClipboardReadText(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    auto resolver = v8::Promise::Resolver::New(ctx).ToLocalChecked();
    args.GetReturnValue().Set(resolver->GetPromise());
    resolver->Resolve(ctx, Str(isolate, ReadClipboardText())).Check();
}

// navigator.clipboard.writeText(text) -> Promise
void ClipboardWriteText(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    auto resolver = v8::Promise::Resolver::New(ctx).ToLocalChecked();
    args.GetReturnValue().Set(resolver->GetPromise());

    if (args.Length() > 0) {
        WriteClipboardText(v8util::ToStdString(isolate, args[0]));
    }
    resolver->Resolve(ctx, v8::Undefined(isolate)).Check();
}

// crypto.getRandomValues(typedArray): OS 乱数で埋めて同じ view を返す
void CryptoGetRandomValues(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    if (args.Length() < 1 || !args[0]->IsArrayBufferView()) {
        v8util::ThrowTypeError(isolate, "getRandomValues requires a TypedArray");
        return;
    }
    auto view = args[0].As<v8::ArrayBufferView>();
    std::vector<uint8_t> bytes(view->ByteLength());
    if (!bytes.empty()) {
        FillRandom(bytes.data(), bytes.size());
        // view のバッキングへ書き戻す
        auto ab = view->Buffer();
        auto* dst = static_cast<uint8_t*>(ab->GetBackingStore()->Data()) + view->ByteOffset();
        memcpy(dst, bytes.data(), bytes.size());
    }
    args.GetReturnValue().Set(args[0]);
}

// crypto.randomUUID(): RFC 4122 v4
void CryptoRandomUUID(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    uint8_t b[16] = {};
    FillRandom(b, sizeof(b));
    b[6] = static_cast<uint8_t>((b[6] & 0x0f) | 0x40);   // version 4
    b[8] = static_cast<uint8_t>((b[8] & 0x3f) | 0x80);   // variant
    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
        b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    args.GetReturnValue().Set(Str(isolate, buf));
}

// --- 名前付きストレージの永続化バックエンド ---------------------------------
// localStorage / indexedDB のセマンティクスは Polyfills.cpp の JS 側が実装し、
// ここは名前ごとのファイル I/O のみを担う。
// 保存先は、埋め込み側が指定したディレクトリ (HostContext::storage_dir)、無ければ OS の既定
// (DefaultStorageDirectory)。どちらも無ければファイルへは保存せず、メモリ上にだけ持つ
// (プロセスが終わると消える)。ファイル名は <name>.json。

std::map<std::string, std::string>& MemoryStorage()
{
    static std::map<std::string, std::string> storage;
    return storage;
}

// 保存先のファイルパス。保存先が無ければ空。
std::filesystem::path StorageFilePath(HostContext* host, const std::string& name)
{
    // ファイル名として安全な文字だけを残す
    std::string safe;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        safe.push_back(ok ? c : '_');
    }
    if (safe.empty()) {
        safe = "storage";
    }

    std::filesystem::path dir = (host && !host->storage_dir.empty())
        ? std::filesystem::u8path(host->storage_dir)
        : DefaultStorageDirectory();
    if (dir.empty()) {
        return {};
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / (safe + ".json");
}

// __next2d_storage_load(name) -> string
void StorageLoad(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    const std::string name = args.Length() > 0
        ? v8util::ToStdString(isolate, args[0]) : "localStorage";
    const std::filesystem::path path = StorageFilePath(HostContext::From(isolate), name);
    if (path.empty()) {
        const auto it = MemoryStorage().find(name);
        args.GetReturnValue().Set(Str(isolate, it == MemoryStorage().end() ? std::string() : it->second));
        return;
    }
    std::ifstream ifs(path, std::ios::binary);
    std::ostringstream data;
    if (ifs) {
        data << ifs.rdbuf();
    }
    args.GetReturnValue().Set(Str(isolate, data.str()));
}

// __next2d_storage_save(name, data)
void StorageSave(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    if (args.Length() < 2) {
        return;
    }
    v8::Isolate* isolate = args.GetIsolate();
    const std::string name = v8util::ToStdString(isolate, args[0]);
    const std::string data = v8util::ToStdString(isolate, args[1]);
    const std::filesystem::path path = StorageFilePath(HostContext::From(isolate), name);
    if (path.empty()) {
        MemoryStorage()[name] = data;
        return;
    }
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return;
        }
        ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    // ファイルを閉じてから、埋め込み側へ知らせる
    HostContext* host = HostContext::From(isolate);
    if (host && host->storage_saved) {
        host->storage_saved(name.c_str(), host->storage_saved_user);
    }
}

// new OffscreenCanvas(width, height)
void OffscreenCanvasConstructor(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    if (!args.IsConstructCall()) {
        v8util::ThrowTypeError(isolate, "OffscreenCanvas must be called with new");
        return;
    }
    HostContext* host = HostContext::From(isolate);
    int w = args.Length() > 0 && args[0]->IsNumber()
        ? static_cast<int>(args[0].As<v8::Number>()->Value()) : host->viewport_width;
    int h = args.Length() > 1 && args[1]->IsNumber()
        ? static_cast<int>(args[1].As<v8::Number>()->Value()) : host->viewport_height;
    args.GetReturnValue().Set(CreateOffscreenCanvas(isolate, host, w, h));
}

} // namespace

void InstallDomShims(v8::Isolate* isolate, v8::Local<v8::Object> global, HostContext* host)
{
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

    // OffscreenCanvas コンストラクタ (メッシュ生成/テキスト/点内包判定で使用)
    {
        v8::Local<v8::FunctionTemplate> oc =
            v8::FunctionTemplate::New(isolate, OffscreenCanvasConstructor);
        oc->SetClassName(Str(isolate, "OffscreenCanvas"));
        global->Set(ctx, Str(isolate, "OffscreenCanvas"),
                    oc->GetFunction(ctx).ToLocalChecked()).Check();
    }

    // globalThis / window / self は同一オブジェクトを指す
    SetValue(isolate, global, "globalThis", global);
    SetValue(isolate, global, "window", global);
    SetValue(isolate, global, "self", global);
    NoopEventTarget(isolate, global);

    // crypto (getRandomValues / randomUUID — OS 乱数)。
    // player の WebGLUtil が ID 生成に crypto.randomUUID を使う。
    {
        v8::Local<v8::Object> crypto = v8::Object::New(isolate);
        SetMethod(isolate, crypto, "getRandomValues", CryptoGetRandomValues);
        SetMethod(isolate, crypto, "randomUUID", CryptoRandomUUID);
        SetValue(isolate, global, "crypto", crypto);
    }

    // localStorage のファイル I/O バックエンド (Polyfills.cpp の JS が利用)
    SetMethod(isolate, global, "__next2d_storage_load", StorageLoad);
    SetMethod(isolate, global, "__next2d_storage_save", StorageSave);

    // devicePixelRatio
    SetValue(isolate, global, "devicePixelRatio",
             v8::Number::New(isolate, host->device_pixel_ratio));

    // screen
    v8::Local<v8::Object> screen = v8::Object::New(isolate);
    SetValue(isolate, screen, "width", v8::Integer::New(isolate, host->viewport_width));
    SetValue(isolate, screen, "height", v8::Integer::New(isolate, host->viewport_height));
    SetValue(isolate, global, "screen", screen);

    // innerWidth / innerHeight
    SetValue(isolate, global, "innerWidth", v8::Integer::New(isolate, host->viewport_width));
    SetValue(isolate, global, "innerHeight", v8::Integer::New(isolate, host->viewport_height));

    // scrollX / scrollY (非スクロールの全画面アプリでは常に 0)。
    // 未定義だと player の PlayerSetCurrentMousePointService 冒頭
    // `let x = window.scrollX;` が undefined になり、以降 x が NaN 化して
    // stage.pointer 全体が NaN → ヒットテストが全 miss → ボタンが一切反応しない。
    SetValue(isolate, global, "scrollX", v8::Integer::New(isolate, 0));
    SetValue(isolate, global, "scrollY", v8::Integer::New(isolate, 0));
    SetValue(isolate, global, "pageXOffset", v8::Integer::New(isolate, 0));
    SetValue(isolate, global, "pageYOffset", v8::Integer::New(isolate, 0));

    // navigator (gpu/getGamepads は後段のバインディングが付与)
    v8::Local<v8::Object> navigator = v8::Object::New(isolate);
    SetValue(isolate, navigator, "userAgent",
             Str(isolate, "Next2D/Xbox (GDK; V8; Dawn/WebGPU)"));
    SetValue(isolate, navigator, "language", Str(isolate, "en-US"));
    SetValue(isolate, navigator, "platform", Str(isolate, "Xbox"));

    // navigator.clipboard (テキスト貼付/コピー。Windows 以外では常に空)
    v8::Local<v8::Object> clipboard = v8::Object::New(isolate);
    SetMethod(isolate, clipboard, "readText", ClipboardReadText);
    SetMethod(isolate, clipboard, "writeText", ClipboardWriteText);
    SetValue(isolate, navigator, "clipboard", clipboard);

    SetValue(isolate, global, "navigator", navigator);

    // document
    v8::Local<v8::Object> document = v8::Object::New(isolate);
    SetMethod(isolate, document, "createElement", CreateElement);
    // 起動時点で読み込み完了扱い (ゲームの boot 分岐が readyState === "loading" で待機しないように)
    SetValue(isolate, document, "readyState", Str(isolate, "complete"));
    SetMethod(isolate, document, "getElementById",
        [](const v8::FunctionCallbackInfo<v8::Value>& a) { a.GetReturnValue().SetNull(); });
    SetMethod(isolate, document, "querySelector",
        [](const v8::FunctionCallbackInfo<v8::Value>& a) { a.GetReturnValue().SetNull(); });
    SetMethod(isolate, document, "getElementsByTagName",
        [](const v8::FunctionCallbackInfo<v8::Value>& a) {
            a.GetReturnValue().Set(v8::Array::New(a.GetIsolate(), 0));
        });
    NoopEventTarget(isolate, document);

    // document.body / documentElement は canvas を格納できる最小要素
    v8::Local<v8::Object> body = v8::Object::New(isolate);
    SetValue(isolate, body, "style", v8::Object::New(isolate));
    SetMethod(isolate, body, "appendChild", [](const v8::FunctionCallbackInfo<v8::Value>& a) {
        if (a.Length() > 0) a.GetReturnValue().Set(a[0]);
    });
    NoopEventTarget(isolate, body);
    SetValue(isolate, document, "body", body);
    SetValue(isolate, document, "documentElement", body);

    // メインの canvas をあらかじめ 1 枚用意し document.__mainCanvas として公開する。
    // bootstrap.js はこれを既定キャンバスとして Next2D に渡す。
    v8::Local<v8::Object> canvas = CreateCanvasElement(isolate, host, 0, 0);
    SetValue(isolate, document, "__mainCanvas", canvas);

    global->Set(ctx, Str(isolate, "document"), document).Check();
}

void InstallGlobalBindings(v8::Isolate* isolate, v8::Local<v8::Object> global, HostContext* host)
{
    // 依存順: console -> DOM -> timers -> fetch -> image -> audio -> webgpu -> gamepad
    InstallConsole(isolate, global);
    InstallDomShims(isolate, global, host);
    InstallTimers(isolate, global, host);
    InstallFetch(isolate, global, host);
    InstallNetwork(isolate, global, host);  // XMLHttpRequest/Blob/URL/ImageData
    InstallImage(isolate, global, host);
    InstallAudio(isolate, global, host);
#if !defined(NEXT2D_ENABLE_WEBGPU) || NEXT2D_ENABLE_WEBGPU
    InstallWebGPU(isolate, global, host);   // navigator.gpu
#endif
    InstallGamepad(isolate, global, host);  // navigator.getGamepads

    // TextEncoder/TextDecoder/queueMicrotask。bootstrap.js はメインコンテキスト
    // でのみ実行されるため、worker を含む全コンテキストにはここで注入する
    // (Vite バンドルの worker は評価時に TextDecoder を要求する)。
    InstallPolyfills(isolate, isolate->GetCurrentContext());
}

} // namespace next2d
