// WebGL2 バインディングの実行テスト用 Node アドオン。
//
// Node は V8 を内蔵しているので、ホスト本体 (V8 モノリス + 実機) が無くても
// src/bindings/webgl/WebGL2.cpp をそのままコンパイルして実 GPU 上で動かせる。
// OpenGL ES 3 は ANGLE (EGL + libGLESv2) の pbuffer で用意する。
//   createContext(width, height) -> WebGL2RenderingContext 相当
//   needsPresent() / swap() / swapCount() : 提示フラグの検証用
#include <node.h>

#include "../../src/HostContext.h"
#include "../../src/bindings/Bindings.h"
#include "../../src/bindings/ImageSource.h"
#include "../../src/gpu/EglContext.h"

#include <memory>
#include <string>

namespace next2d {

// テスト用の画像ソース: {width, height, data: Uint8Array(RGBA8 straight alpha)}。
// 本体では Image.cpp が canvas / ImageBitmap から取り出す。
bool GetImageSourcePixels(v8::Isolate* isolate, v8::Local<v8::Value> source,
                          const uint8_t** out_rgba, uint32_t* out_width, uint32_t* out_height)
{
    if (!source->IsObject()) {
        return false;
    }
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    v8::Local<v8::Object> obj = source.As<v8::Object>();
    auto get = [&](const char* key) {
        return obj->Get(ctx, v8::String::NewFromUtf8(isolate, key).ToLocalChecked())
            .FromMaybe(v8::Local<v8::Value>());
    };
    v8::Local<v8::Value> data = get("data");
    v8::Local<v8::Value> w = get("width");
    v8::Local<v8::Value> h = get("height");
    if (data.IsEmpty() || !data->IsUint8Array() || w.IsEmpty() || h.IsEmpty()) {
        return false;
    }
    v8::Local<v8::Uint8Array> bytes = data.As<v8::Uint8Array>();
    *out_width = w->Uint32Value(ctx).FromMaybe(0);
    *out_height = h->Uint32Value(ctx).FromMaybe(0);
    if (bytes->ByteLength() < static_cast<size_t>(*out_width) * *out_height * 4) {
        return false;
    }
    *out_rgba = static_cast<const uint8_t*>(bytes->Buffer()->Data()) + bytes->ByteOffset();
    return true;
}

} // namespace next2d

namespace {

next2d::HostContext g_host;
std::unique_ptr<next2d::EglContext> g_egl;

void CreateContext(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    const int width = args.Length() > 0 ? args[0]->Int32Value(ctx).FromMaybe(256) : 256;
    const int height = args.Length() > 1 ? args[1]->Int32Value(ctx).FromMaybe(256) : 256;

    std::string error;
    g_egl = next2d::EglContext::Create(nullptr, nullptr, width, height, &error);
    if (!g_egl) {
        isolate->ThrowException(v8::Exception::Error(
            v8::String::NewFromUtf8(isolate, error.c_str()).ToLocalChecked()));
        return;
    }
    g_host.gles = g_egl.get();
    g_host.backend = next2d::GraphicsBackend::WebGL2;
    g_host.viewport_width = width;
    g_host.viewport_height = height;

    v8::Local<v8::Object> gl = next2d::CreateWebGL2Context(isolate, &g_host);
    if (gl.IsEmpty()) {
        isolate->ThrowException(v8::Exception::Error(
            v8::String::NewFromUtf8(isolate, "CreateWebGL2Context failed").ToLocalChecked()));
        return;
    }
    args.GetReturnValue().Set(gl);
}

void NeedsPresent(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    args.GetReturnValue().Set(g_egl && g_egl->needs_present);
}

void Swap(const v8::FunctionCallbackInfo<v8::Value>&)
{
    if (g_egl) {
        g_egl->SwapBuffers();
    }
}

void SwapCount(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    args.GetReturnValue().Set(g_egl ? g_egl->swap_count() : 0);
}

void Initialize(v8::Local<v8::Object> exports, v8::Local<v8::Value> /*module*/, void* /*priv*/)
{
    NODE_SET_METHOD(exports, "createContext", CreateContext);
    NODE_SET_METHOD(exports, "needsPresent", NeedsPresent);
    NODE_SET_METHOD(exports, "swap", Swap);
    NODE_SET_METHOD(exports, "swapCount", SwapCount);
}

} // namespace

NODE_MODULE(NODE_GYP_MODULE_NAME, Initialize)
