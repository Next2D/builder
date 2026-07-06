#include "Bindings.h"

#include "HostContext.h"
#include "AssetLoader.h"
#include "EventTarget.h"
#include "ImageSource.h"
#include "platform/WicDecoder.h"
#include "v8/V8Util.h"
#include "v8/WeakHandle.h"

#include <objbase.h>

#include <cstring>
#include <iostream>
#include <vector>

namespace next2d {

using v8util::Str;
using v8util::ToStdString;

namespace {

// base64 デコード (data: URL 用。Vite は小さい画像をインライン化する)
bool ImageDecodeBase64(const std::string& in, std::vector<uint8_t>* out)
{
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out->clear();
    out->reserve(in.size() / 4 * 3);
    int buf = 0, bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\r' || c == '\n') continue;
        const int v = val(c);
        if (v < 0) return false;
        buf = buf << 6 | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<uint8_t>(buf >> bits & 0xFF));
        }
    }
    return true;
}

// 遅延発火する load/error イベント (img.src = url; img.onload = fn の順でも拾えるよう
// microtask で発火する。メインループの PerformMicrotaskCheckpoint が実行する)
struct PendingImageEvent {
    v8::Isolate* isolate;
    v8::Global<v8::Object> image;
    bool ok;
};

void FireImageEvent(void* data)
{
    auto* p = static_cast<PendingImageEvent*>(data);
    v8::Isolate* isolate = p->isolate;
    v8::HandleScope hs(isolate);
    v8::Local<v8::Object> img = p->image.Get(isolate);
    v8::Local<v8::Context> ctx = img->GetCreationContextChecked();
    v8::Context::Scope cs(ctx);

    v8::Local<v8::Object> ev = v8::Object::New(isolate);
    v8util::SetValue(isolate, ev, "type", Str(isolate, p->ok ? "load" : "error"));
    v8util::SetValue(isolate, ev, "target", img);
    DispatchEvent(isolate, img, ev);

    p->image.Reset();
    delete p;
}

// src に指定された URL (assets 相対 / data:) を読み込み WIC でデコードする。
// 成否イベントは microtask で発火する。
void LoadImageFromSrc(v8::Isolate* isolate, v8::Local<v8::Object> self, const std::string& url)
{
    std::vector<uint8_t> input;
    if (url.rfind("data:", 0) == 0) {
        const auto comma = url.find(',');
        if (comma != std::string::npos) {
            const std::string meta = url.substr(5, comma - 5);
            const std::string payload = url.substr(comma + 1);
            if (meta.find(";base64") != std::string::npos) {
                ImageDecodeBase64(payload, &input);
            }
        }
    } else {
        AssetLoader* assets = HostContext::From(isolate)->assets;
        auto bytes = assets->ReadBinary(url);
        if (bytes) {
            input = std::move(*bytes);
        }
    }

    auto* img = new DecodedImage();
    const bool ok = !input.empty() && DecodeImageWithWIC(input, *img);
    if (ok) {
        self->SetInternalField(0, v8::External::New(isolate, img));
        v8util::AttachWeak(isolate, self, img);
        v8util::SetValue(isolate, self, "width",
                         v8::Integer::NewFromUnsigned(isolate, img->width));
        v8util::SetValue(isolate, self, "height",
                         v8::Integer::NewFromUnsigned(isolate, img->height));
        v8util::SetValue(isolate, self, "naturalWidth",
                         v8::Integer::NewFromUnsigned(isolate, img->width));
        v8util::SetValue(isolate, self, "naturalHeight",
                         v8::Integer::NewFromUnsigned(isolate, img->height));
        v8util::SetValue(isolate, self, "complete", v8::Boolean::New(isolate, true));
    } else {
        delete img;
        std::cerr << "[Image] load failed: "
                  << (url.size() > 96 ? url.substr(0, 96) + "..." : url) << std::endl;
    }

    auto* pending = new PendingImageEvent();
    pending->isolate = isolate;
    pending->image.Reset(isolate, self);
    pending->ok = ok;
    isolate->EnqueueMicrotask(FireImageEvent, pending);
}

// Image コンストラクタ: src セッターで読み込み+デコードし load イベントを発火する。
void ImageConstructor(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    if (!args.IsConstructCall()) {
        v8util::ThrowTypeError(isolate, "Image must be called with new");
        return;
    }
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();

    v8util::SetValue(isolate, self, "__isImageElement", v8::Boolean::New(isolate, true));
    v8util::SetValue(isolate, self, "tagName", Str(isolate, "IMG"));
    v8util::SetValue(isolate, self, "localName", Str(isolate, "img"));
    v8util::SetValue(isolate, self, "complete", v8::Boolean::New(isolate, false));
    v8util::SetValue(isolate, self, "width", v8::Integer::New(isolate, 0));
    v8util::SetValue(isolate, self, "height", v8::Integer::New(isolate, 0));
    v8util::SetValue(isolate, self, "naturalWidth", v8::Integer::New(isolate, 0));
    v8util::SetValue(isolate, self, "naturalHeight", v8::Integer::New(isolate, 0));
    InstallEventTarget(isolate, self);   // addEventListener("load"/"error")

    self->SetNativeDataProperty(ctx, Str(isolate, "src"),
        [](v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
            v8::Local<v8::Value> v;
            if (info.This()->Get(info.GetIsolate()->GetCurrentContext(),
                Str(info.GetIsolate(), "__src")).ToLocal(&v) && !v->IsUndefined()) {
                info.GetReturnValue().Set(v);
            } else {
                info.GetReturnValue().Set(Str(info.GetIsolate(), ""));
            }
        },
        [](v8::Local<v8::Name>, v8::Local<v8::Value> value,
           const v8::PropertyCallbackInfo<void>& info) {
            v8::Isolate* iso = info.GetIsolate();
            v8util::SetValue(iso, info.This(), "__src", value);
            LoadImageFromSrc(iso, info.This(), ToStdString(iso, value));
        }).Check();

    // decode(): 読み込み済みなら resolve、未読み込みでも resolve (簡易)
    v8util::SetMethod(isolate, self, "decode",
        [](const v8::FunctionCallbackInfo<v8::Value>& a) {
            v8::Isolate* iso = a.GetIsolate();
            auto r = v8::Promise::Resolver::New(iso->GetCurrentContext()).ToLocalChecked();
            r->Resolve(iso->GetCurrentContext(), v8::Undefined(iso)).Check();
            a.GetReturnValue().Set(r->GetPromise());
        });

    args.GetReturnValue().Set(self);
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

    // Image 要素 (new Image() + src): 読み込み完了後は内部フィールドに DecodedImage
    v8::Local<v8::Value> is_image;
    if (obj->Get(ctx, Str(isolate, "__isImageElement")).ToLocal(&is_image) &&
        is_image->IsBoolean() && is_image.As<v8::Boolean>()->Value()) {
        if (obj->InternalFieldCount() < 1) {
            return false;
        }
        v8::Local<v8::Data> field = obj->GetInternalField(0);
        if (!field->IsValue() || !field.As<v8::Value>()->IsExternal()) {
            return false;   // 未読み込み
        }
        auto* img = static_cast<DecodedImage*>(
            field.As<v8::Value>().As<v8::External>()->Value());
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
    // 内部フィールド0: デコード済み DecodedImage* (src セッターが設定)
    image_tmpl->InstanceTemplate()->SetInternalFieldCount(1);
    global->Set(ctx, Str(isolate, "Image"),
                image_tmpl->GetFunction(ctx).ToLocalChecked()).Check();

    v8util::SetMethod(isolate, global, "createImageBitmap", CreateImageBitmap);
}

} // namespace next2d
