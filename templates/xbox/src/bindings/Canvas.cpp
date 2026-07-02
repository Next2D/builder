#include "Bindings.h"

#include "HostContext.h"
#include "EventTarget.h"
#include "ImageSource.h"
#include "v8/V8Util.h"

namespace next2d {

using v8util::Str;
using v8util::SetMethod;
using v8util::SetValue;
using v8util::ToStdString;

namespace {

// getContext(type) を backend に応じてディスパッチする共通実装。
// '2d' → Canvas2D、'webgpu' → WebGPU(Dawn)、'webgl2' → WebGL2(Switch 用・Xboxではnull)。
void GetContextImpl(const v8::FunctionCallbackInfo<v8::Value>& args, bool /*offscreen*/)
{
    v8::Isolate* isolate = args.GetIsolate();
    HostContext* host = HostContext::From(isolate);
    const std::string type = args.Length() > 0 ? ToStdString(isolate, args[0]) : "";

    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    int width = host->viewport_width;
    int height = host->viewport_height;
    v8::Local<v8::Value> wv, hv;
    if (self->Get(ctx, Str(isolate, "width")).ToLocal(&wv) && wv->IsNumber()) {
        width = static_cast<int>(wv.As<v8::Number>()->Value());
    }
    if (self->Get(ctx, Str(isolate, "height")).ToLocal(&hv) && hv->IsNumber()) {
        height = static_cast<int>(hv.As<v8::Number>()->Value());
    }

    // 同一 canvas への同型 getContext は同じインスタンスを返す(ブラウザ準拠)。
    // player は offscreen.getContext('2d') で描画後、canvas を copyExternalImageToTexture へ
    // 渡すため、2D コンテキスト(と描画済みピクセル)を canvas 上に保持する必要がある。
    const char* cache_key =
        (type == "2d") ? "__ctx2d" :
        (type == "webgpu") ? "__ctxgpu" : "__ctxgl";
    v8::Local<v8::Value> cached;
    if (self->Get(ctx, Str(isolate, cache_key)).ToLocal(&cached) && cached->IsObject()) {
        args.GetReturnValue().Set(cached);
        return;
    }

    v8::Local<v8::Object> context;
    if (type == "2d") {
        context = CreateCanvas2DContext(isolate, host, width, height);
    } else if (type == "webgpu" && host->backend == GraphicsBackend::WebGPU) {
        context = CreateWebGPUCanvasContext(isolate, host);
    } else if ((type == "webgl2" || type == "webgl") && host->backend == GraphicsBackend::WebGL2) {
        // «SWITCH» WebGL2(GL) バインディングをここで返す。Xbox(WebGPU)では未使用。
        args.GetReturnValue().SetNull();
        return;
    } else {
        args.GetReturnValue().SetNull();
        return;
    }

    SetValue(isolate, context, "canvas", self);
    SetValue(isolate, self, cache_key, context);
    args.GetReturnValue().Set(context);
}

void Canvas_GetContext(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    GetContextImpl(args, false);
}

void Offscreen_GetContext(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    GetContextImpl(args, true);
}

void GetBoundingClientRect(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    v8::Isolate* iso = a.GetIsolate();
    HostContext* h = HostContext::From(iso);
    v8::Local<v8::Object> rect = v8::Object::New(iso);
    SetValue(iso, rect, "x", v8::Number::New(iso, 0));
    SetValue(iso, rect, "y", v8::Number::New(iso, 0));
    SetValue(iso, rect, "width", v8::Number::New(iso, h->viewport_width));
    SetValue(iso, rect, "height", v8::Number::New(iso, h->viewport_height));
    SetValue(iso, rect, "left", v8::Number::New(iso, 0));
    SetValue(iso, rect, "top", v8::Number::New(iso, 0));
    a.GetReturnValue().Set(rect);
}

// canvas.transferControlToOffscreen()
void TransferControlToOffscreen(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    HostContext* host = HostContext::From(isolate);
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

    int width = host->viewport_width;
    int height = host->viewport_height;
    v8::Local<v8::Value> wv, hv;
    if (args.This()->Get(ctx, Str(isolate, "width")).ToLocal(&wv) && wv->IsNumber()) {
        width = static_cast<int>(wv.As<v8::Number>()->Value());
    }
    if (args.This()->Get(ctx, Str(isolate, "height")).ToLocal(&hv) && hv->IsNumber()) {
        height = static_cast<int>(hv.As<v8::Number>()->Value());
    }
    args.GetReturnValue().Set(CreateOffscreenCanvas(isolate, host, width, height));
}

// OffscreenCanvas.transferToImageBitmap(): 2D 内容を ImageBitmap 化する (Video フレーム経路)。
void TransferToImageBitmap(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    v8::Local<v8::Value> ctx2d;
    if (!args.This()->Get(ctx, Str(isolate, "__ctx2d")).ToLocal(&ctx2d) || !ctx2d->IsObject()) {
        args.GetReturnValue().SetNull();
        return;
    }
    const uint8_t* rgba = nullptr; uint32_t w = 0, h = 0;
    if (!GetCanvas2DPixels(ctx2d.As<v8::Object>(), &rgba, &w, &h)) {
        args.GetReturnValue().SetNull();
        return;
    }
    auto* img = new DecodedImage();
    img->width = w; img->height = h;
    img->rgba.assign(rgba, rgba + static_cast<size_t>(w) * h * 4);
    args.GetReturnValue().Set(WrapImageBitmap(isolate, img));
}

} // namespace

v8::Local<v8::Object> CreateCanvasElement(v8::Isolate* isolate, HostContext* host,
                                          int width, int height)
{
    v8::Local<v8::Object> canvas = v8::Object::New(isolate);
    SetValue(isolate, canvas, "width",
             v8::Integer::New(isolate, width ? width : host->viewport_width));
    SetValue(isolate, canvas, "height",
             v8::Integer::New(isolate, height ? height : host->viewport_height));
    SetValue(isolate, canvas, "clientWidth", v8::Integer::New(isolate, host->viewport_width));
    SetValue(isolate, canvas, "clientHeight", v8::Integer::New(isolate, host->viewport_height));
    SetValue(isolate, canvas, "style", v8::Object::New(isolate));
    SetMethod(isolate, canvas, "getContext", Canvas_GetContext);
    SetMethod(isolate, canvas, "transferControlToOffscreen", TransferControlToOffscreen);
    InstallEventTarget(isolate, canvas);   // pointer/wheel リスナを保持
    SetMethod(isolate, canvas, "getBoundingClientRect", GetBoundingClientRect);
    // 主要 canvas を HostContext から参照できるよう記録 (WndProc の入力配送先)
    HostContext::From(isolate)->main_canvas.Reset(isolate, canvas);
    return canvas;
}

v8::Local<v8::Object> CreateOffscreenCanvas(v8::Isolate* isolate, HostContext* host,
                                            int width, int height)
{
    v8::Local<v8::Object> canvas = v8::Object::New(isolate);
    // Worker への transfer 検出用マーカー (WorkerRuntime の serializer が参照)
    SetValue(isolate, canvas, "__isOffscreenCanvas", v8::Boolean::New(isolate, true));
    SetValue(isolate, canvas, "width",
             v8::Integer::New(isolate, width ? width : host->viewport_width));
    SetValue(isolate, canvas, "height",
             v8::Integer::New(isolate, height ? height : host->viewport_height));
    SetMethod(isolate, canvas, "getContext", Offscreen_GetContext);
    SetMethod(isolate, canvas, "transferToImageBitmap", TransferToImageBitmap);
    SetMethod(isolate, canvas, "addEventListener", [](const v8::FunctionCallbackInfo<v8::Value>&) {});
    SetMethod(isolate, canvas, "removeEventListener", [](const v8::FunctionCallbackInfo<v8::Value>&) {});
    return canvas;
}

} // namespace next2d
