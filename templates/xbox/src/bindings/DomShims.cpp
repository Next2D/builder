#include "Bindings.h"

#include "HostContext.h"
#include "EventTarget.h"
#include "v8/V8Util.h"

#include <Windows.h>

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

// navigator.clipboard.readText() -> Promise<string> (Win32 クリップボード)
void ClipboardReadText(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    auto resolver = v8::Promise::Resolver::New(ctx).ToLocalChecked();
    args.GetReturnValue().Set(resolver->GetPromise());

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
    resolver->Resolve(ctx, Str(isolate, text)).Check();
}

// navigator.clipboard.writeText(text) -> Promise
void ClipboardWriteText(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    auto resolver = v8::Promise::Resolver::New(ctx).ToLocalChecked();
    args.GetReturnValue().Set(resolver->GetPromise());

    if (args.Length() > 0 && OpenClipboard(nullptr)) {
        const std::string s = v8util::ToStdString(isolate, args[0]);
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
    resolver->Resolve(ctx, v8::Undefined(isolate)).Check();
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

    // navigator (gpu/getGamepads は後段のバインディングが付与)
    v8::Local<v8::Object> navigator = v8::Object::New(isolate);
    SetValue(isolate, navigator, "userAgent",
             Str(isolate, "Next2D/Xbox (GDK; V8; Dawn/WebGPU)"));
    SetValue(isolate, navigator, "language", Str(isolate, "en-US"));
    SetValue(isolate, navigator, "platform", Str(isolate, "Xbox"));

    // navigator.clipboard (テキスト貼付/コピー、Win32 クリップボード)
    v8::Local<v8::Object> clipboard = v8::Object::New(isolate);
    SetMethod(isolate, clipboard, "readText", ClipboardReadText);
    SetMethod(isolate, clipboard, "writeText", ClipboardWriteText);
    SetValue(isolate, navigator, "clipboard", clipboard);

    SetValue(isolate, global, "navigator", navigator);

    // document
    v8::Local<v8::Object> document = v8::Object::New(isolate);
    SetMethod(isolate, document, "createElement", CreateElement);
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
    InstallWebGPU(isolate, global, host);   // navigator.gpu
    InstallGamepad(isolate, global, host);  // navigator.getGamepads

    // TextEncoder/TextDecoder/queueMicrotask。bootstrap.js はメインコンテキスト
    // でのみ実行されるため、worker を含む全コンテキストにはここで注入する
    // (Vite バンドルの worker は評価時に TextDecoder を要求する)。
    InstallPolyfills(isolate, isolate->GetCurrentContext());
}

} // namespace next2d
