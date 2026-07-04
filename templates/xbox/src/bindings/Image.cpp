#include "Bindings.h"

#include "HostContext.h"
#include "AssetLoader.h"
#include "ImageSource.h"
#include "platform/WicDecoder.h"
#include "v8/V8Util.h"
#include "v8/WeakHandle.h"

#include <objbase.h>

#include <cstring>
#include <vector>

namespace next2d {

using v8util::Str;
using v8util::ToStdString;

namespace {

// Image コンストラクタ: src セッターで読み込み+デコードし onload を発火する。
void ImageConstructor(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    if (!args.IsConstructCall()) {
        v8util::ThrowTypeError(isolate, "Image must be called with new");
        return;
    }
    // 実体は通常オブジェクト。src セッターで decode をトリガする。
    args.This()->Set(isolate->GetCurrentContext(),
                     Str(isolate, "complete"),
                     v8::Boolean::New(isolate, false)).Check();
    args.GetReturnValue().Set(args.This());
}

// createImageBitmap(source) -> Promise<ImageBitmap>
// source は Blob / Response.arrayBuffer 結果(ArrayBuffer/TypedArray) を想定。
void CreateImageBitmap(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

    v8::Local<v8::Promise::Resolver> resolver =
        v8::Promise::Resolver::New(ctx).ToLocalChecked();
    args.GetReturnValue().Set(resolver->GetPromise());

    if (args.Length() < 1) {
        resolver->Reject(ctx, v8::Exception::TypeError(Str(isolate, "source required"))).Check();
        return;
    }

    std::vector<uint8_t> input;
    if (args[0]->IsArrayBuffer()) {
        auto ab = args[0].As<v8::ArrayBuffer>();
        auto store = ab->GetBackingStore();
        input.assign(static_cast<uint8_t*>(store->Data()),
                     static_cast<uint8_t*>(store->Data()) + store->ByteLength());
    } else if (args[0]->IsArrayBufferView()) {
        auto view = args[0].As<v8::ArrayBufferView>();
        input.resize(view->ByteLength());
        view->CopyContents(input.data(), input.size());
    } else if (args[0]->IsString()) {
        // URL 文字列: assets から読み込む
        AssetLoader* assets = HostContext::From(isolate)->assets;
        auto bytes = assets->ReadBinary(ToStdString(isolate, args[0]));
        if (bytes) {
            input = std::move(*bytes);
        }
    } else if (args[0]->IsObject()) {
        v8::Local<v8::Object> obj = args[0].As<v8::Object>();

        // 1) ImageData ({ data: Uint8ClampedArray, width, height }):
        //    デコード不要でそのまま RGBA として取り込む
        //    (webgpu レンダラのキャプチャ/フィルタ→ImageBitmap 化の経路)。
        v8::Local<v8::Value> data_v, w_v, h_v;
        if (obj->Get(ctx, Str(isolate, "data")).ToLocal(&data_v) && data_v->IsArrayBufferView() &&
            obj->Get(ctx, Str(isolate, "width")).ToLocal(&w_v) && w_v->IsNumber() &&
            obj->Get(ctx, Str(isolate, "height")).ToLocal(&h_v) && h_v->IsNumber()) {
            const auto w = static_cast<uint32_t>(w_v.As<v8::Number>()->Value());
            const auto h = static_cast<uint32_t>(h_v.As<v8::Number>()->Value());
            auto view = data_v.As<v8::ArrayBufferView>();
            if (view->ByteLength() < static_cast<size_t>(w) * h * 4) {
                resolver->Reject(ctx, v8::Exception::TypeError(
                    Str(isolate, "ImageData size mismatch"))).Check();
                return;
            }
            auto* raw = new DecodedImage();
            raw->width = w;
            raw->height = h;
            raw->rgba.resize(static_cast<size_t>(w) * h * 4);
            view->CopyContents(raw->rgba.data(), raw->rgba.size());
            resolver->Resolve(ctx, WrapImageBitmap(isolate, raw)).Check();
            return;
        }

        // 2) ImageBitmap / 2D 済み canvas / video: ピクセルを複製して新規 ImageBitmap
        const uint8_t* px = nullptr;
        uint32_t sw = 0, sh = 0;
        if (GetImageSourcePixels(isolate, obj, &px, &sw, &sh) && px) {
            auto* raw = new DecodedImage();
            raw->width = sw;
            raw->height = sh;
            raw->rgba.assign(px, px + static_cast<size_t>(sw) * sh * 4);
            resolver->Resolve(ctx, WrapImageBitmap(isolate, raw)).Check();
            return;
        }

        // 3) Blob: 内包バイト列 (エンコード済み画像) を WIC デコードへ回す
        v8::Local<v8::Value> blob_v;
        if (obj->Get(ctx, Str(isolate, "__blobData")).ToLocal(&blob_v) && blob_v->IsArrayBuffer()) {
            auto store = blob_v.As<v8::ArrayBuffer>()->GetBackingStore();
            input.assign(static_cast<uint8_t*>(store->Data()),
                         static_cast<uint8_t*>(store->Data()) + store->ByteLength());
        }
    }

    auto* img = new DecodedImage();
    if (input.empty() || !DecodeImageWithWIC(input, *img)) {
        delete img;
        resolver->Reject(ctx, v8::Exception::Error(Str(isolate, "Image decode failed"))).Check();
        return;
    }

    resolver->Resolve(ctx, WrapImageBitmap(isolate, img)).Check();
}

} // namespace

// ImageBitmap をラップ (内部フィールド0=External<DecodedImage>, __isImageBitmap=true)。
v8::Local<v8::Object> WrapImageBitmap(v8::Isolate* isolate, DecodedImage* image)
{
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(isolate);
    tmpl->SetInternalFieldCount(1);
    v8::Local<v8::Object> obj = tmpl->NewInstance(ctx).ToLocalChecked();
    obj->SetInternalField(0, v8::External::New(isolate, image));

    v8util::SetValue(isolate, obj, "__isImageBitmap", v8::Boolean::New(isolate, true));
    v8util::SetValue(isolate, obj, "width", v8::Integer::NewFromUnsigned(isolate, image->width));
    v8util::SetValue(isolate, obj, "height", v8::Integer::NewFromUnsigned(isolate, image->height));
    v8util::SetMethod(isolate, obj, "close", [](const v8::FunctionCallbackInfo<v8::Value>&) {});

    v8util::AttachWeak(isolate, obj, image);
    return obj;
}

// 画像ソース(ImageBitmap / 2Dコンテキストを持つ canvas) から RGBA を取得する。
bool GetImageSourcePixels(v8::Isolate* isolate, v8::Local<v8::Value> source,
                          const uint8_t** out_rgba, uint32_t* out_width, uint32_t* out_height)
{
    if (!source->IsObject()) {
        return false;
    }
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    v8::Local<v8::Object> obj = source.As<v8::Object>();

    // ImageBitmap
    v8::Local<v8::Value> is_bitmap;
    if (obj->Get(ctx, Str(isolate, "__isImageBitmap")).ToLocal(&is_bitmap) &&
        is_bitmap->IsBoolean() && is_bitmap.As<v8::Boolean>()->Value()) {
        auto* img = static_cast<DecodedImage*>(
            obj->GetInternalField(0).As<v8::External>()->Value());
        *out_rgba = img->rgba.data();
        *out_width = img->width;
        *out_height = img->height;
        return true;
    }

    // video 要素: 現在フレームを返す
    v8::Local<v8::Value> is_video;
    if (obj->Get(ctx, Str(isolate, "__isVideoElement")).ToLocal(&is_video) &&
        is_video->IsBoolean() && is_video.As<v8::Boolean>()->Value()) {
        return GetVideoFramePixels(isolate, obj, out_rgba, out_width, out_height);
    }

    // canvas / OffscreenCanvas: getContext('2d') 済みなら __ctx2d を持つ
    v8::Local<v8::Value> ctx2d;
    if (obj->Get(ctx, Str(isolate, "__ctx2d")).ToLocal(&ctx2d) && ctx2d->IsObject()) {
        return GetCanvas2DPixels(ctx2d.As<v8::Object>(), out_rgba, out_width, out_height);
    }
    return false;
}

void InstallImage(v8::Isolate* isolate, v8::Local<v8::Object> global, HostContext* /*host*/)
{
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

    // COM (WIC 用) 初期化
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    v8::Local<v8::FunctionTemplate> image_tmpl =
        v8::FunctionTemplate::New(isolate, ImageConstructor);
    image_tmpl->SetClassName(Str(isolate, "Image"));
    global->Set(ctx, Str(isolate, "Image"),
                image_tmpl->GetFunction(ctx).ToLocalChecked()).Check();

    v8util::SetMethod(isolate, global, "createImageBitmap", CreateImageBitmap);
}

} // namespace next2d
