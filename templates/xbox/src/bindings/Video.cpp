// HTMLVideoElement の最小実装 (Media Foundation)。
//
// player は document.createElement("video") で生成し、描画ループ中に
// ctx2d.drawImage(videoElement) でフレームを OffscreenCanvas に描いて GPU へ渡す
// (VideoGenerateRenderQueueUseCase)。よって video 要素は「画像ソース」として
// 現在フレームの RGBA を返せる必要がある (GetImageSourcePixels が __isVideoElement を扱う)。
//
// NOTE: フレーム精度・シーク・音声同期・ストリーミングは最小限。実機で要検証。
#include "Bindings.h"

#include "HostContext.h"
#include "AssetLoader.h"
#include "ImageSource.h"
#include "EventTarget.h"
#include "v8/V8Util.h"
#include "v8/WeakHandle.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shlwapi.lib")

using Microsoft::WRL::ComPtr;

namespace next2d {

using v8util::Str;
using v8util::SetMethod;
using v8util::SetValue;
using v8util::ToStdString;

namespace {

struct VideoDecoder {
    ComPtr<IMFSourceReader> reader;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> frame;   // RGBA8 (現在フレーム)
    bool playing = false;
    bool ended = false;
    double duration = 0.0;
    double position = 0.0;        // 現在位置 (秒, サンプルタイムスタンプ由来)

    // 先頭 (または指定秒) へシークする。loop 再生と currentTime 設定で使用。
    bool Seek(double seconds)
    {
        if (!reader) return false;
        PROPVARIANT var;
        PropVariantInit(&var);
        var.vt = VT_I8;
        var.hVal.QuadPart = static_cast<LONGLONG>(seconds * 1e7);   // s -> 100ns
        const bool ok = SUCCEEDED(reader->SetCurrentPosition(GUID_NULL, var));
        PropVariantClear(&var);
        if (ok) {
            ended = false;
            position = seconds;
        }
        return ok;
    }

    bool Open(const std::vector<uint8_t>& bytes)
    {
        MFStartup(MF_VERSION);
        ComPtr<IStream> stream = SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size()));
        if (!stream) return false;
        ComPtr<IMFByteStream> byte_stream;
        if (FAILED(MFCreateMFByteStreamOnStream(stream.Get(), &byte_stream))) return false;
        if (FAILED(MFCreateSourceReaderFromByteStream(byte_stream.Get(), nullptr, &reader))) return false;

        // 出力を RGB32 (BGRX) に指定
        ComPtr<IMFMediaType> out;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out.Get()))) {
            return false;
        }

        ComPtr<IMFMediaType> actual;
        reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actual);
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &w, &h);
        width = w; height = h;
        frame.assign(static_cast<size_t>(w) * h * 4, 0);

        PROPVARIANT var;
        if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE,
                                                       MF_PD_DURATION, &var))) {
            duration = static_cast<double>(var.uhVal.QuadPart) / 1e7;  // 100ns -> s
            PropVariantClear(&var);
        }
        return width > 0 && height > 0;
    }

    // 次サンプルを RGBA へデコードして frame を更新する。
    bool ReadNextFrame()
    {
        if (!reader) return false;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                      nullptr, &flags, &timestamp, &sample))) {
            return false;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { ended = true; return false; }
        if (!sample) return false;
        position = static_cast<double>(timestamp) / 1e7;   // 100ns -> s

        ComPtr<IMFMediaBuffer> buffer;
        sample->ConvertToContiguousBuffer(&buffer);
        BYTE* data = nullptr; DWORD len = 0;
        buffer->Lock(&data, nullptr, &len);
        // RGB32 = BGRX。上下反転して RGBA へ (MF は bottom-up の場合がある → ここでは top-down 前提)
        const size_t px = static_cast<size_t>(width) * height;
        for (size_t i = 0; i < px && (i * 4 + 3) < len; ++i) {
            frame[i*4+0] = data[i*4+2]; // R <- B 位置
            frame[i*4+1] = data[i*4+1]; // G
            frame[i*4+2] = data[i*4+0]; // B <- R 位置
            frame[i*4+3] = 255;
        }
        buffer->Unlock();
        return true;
    }
};

VideoDecoder* Decoder(v8::Local<v8::Object> obj)
{
    return static_cast<VideoDecoder*>(obj->GetInternalField(0).As<v8::External>()->Value());
}

void FireVideoEvent(v8::Isolate* isolate, v8::Local<v8::Object> self, const char* type)
{
    v8::Local<v8::Object> ev = v8::Object::New(isolate);
    SetValue(isolate, ev, "type", Str(isolate, type));
    SetValue(isolate, ev, "target", self);
    DispatchEvent(isolate, self, ev);
}

void LoadVideo(v8::Isolate* isolate, v8::Local<v8::Object> self, const std::string& url)
{
    HostContext* host = HostContext::From(isolate);
    std::vector<uint8_t> bytes;
    std::string text;
    if (url.rfind("blob:", 0) == 0 && ResolveObjectURL(url, &text)) {
        bytes.assign(text.begin(), text.end());
    } else if (host->assets) {
        auto b = host->assets->ReadBinary(url);
        if (b) bytes = std::move(*b);
    }
    VideoDecoder* dec = Decoder(self);
    if (!bytes.empty() && dec->Open(bytes)) {
        SetValue(isolate, self, "videoWidth", v8::Integer::NewFromUnsigned(isolate, dec->width));
        SetValue(isolate, self, "videoHeight", v8::Integer::NewFromUnsigned(isolate, dec->height));
        SetValue(isolate, self, "duration", v8::Number::New(isolate, dec->duration));
        SetValue(isolate, self, "readyState", v8::Integer::New(isolate, 4));
        // player は loadedmetadata / progress / canplaythrough を購読する
        // (VideoRegisterEventUseCase)。ローカル読み込みは即完了のため順に発火する。
        FireVideoEvent(isolate, self, "loadedmetadata");
        FireVideoEvent(isolate, self, "progress");
        FireVideoEvent(isolate, self, "canplaythrough");
    } else {
        FireVideoEvent(isolate, self, "error");
    }
}

} // namespace

// GetImageSourcePixels (Image.cpp) から呼ばれる: video の現在フレーム RGBA を返す。
bool GetVideoFramePixels(v8::Isolate* isolate, v8::Local<v8::Object> obj,
                         const uint8_t** out_rgba, uint32_t* out_w, uint32_t* out_h)
{
    VideoDecoder* dec = Decoder(obj);
    if (!dec || dec->width == 0) return false;
    if (dec->playing && !dec->ended) {
        dec->ReadNextFrame();  // 描画のたびに次フレームへ進める(簡易フレーム進行)
        if (dec->ended) {
            // loop 指定時は先頭へ戻して継続、そうでなければ ended を通知
            v8::Local<v8::Value> loop_v;
            const bool loop =
                obj->Get(isolate->GetCurrentContext(),
                         v8util::Str(isolate, "loop")).ToLocal(&loop_v) &&
                loop_v->BooleanValue(isolate);
            if (loop && dec->Seek(0.0)) {
                dec->ReadNextFrame();
            } else {
                dec->playing = false;
                SetValue(isolate, obj, "ended", v8::Boolean::New(isolate, true));
                SetValue(isolate, obj, "paused", v8::Boolean::New(isolate, true));
                FireVideoEvent(isolate, obj, "ended");
            }
        }
    }
    *out_rgba = dec->frame.data();
    *out_w = dec->width;
    *out_h = dec->height;
    return true;
}

v8::Local<v8::Object> CreateVideoElement(v8::Isolate* isolate, HostContext* /*host*/)
{
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(isolate);
    tmpl->SetInternalFieldCount(1);
    v8::Local<v8::Object> self = tmpl->NewInstance(ctx).ToLocalChecked();

    auto* dec = new VideoDecoder();
    self->SetInternalField(0, v8::External::New(isolate, dec));
    v8util::AttachWeak(isolate, self, dec);

    SetValue(isolate, self, "__isVideoElement", v8::Boolean::New(isolate, true));
    SetValue(isolate, self, "tagName", Str(isolate, "VIDEO"));
    SetValue(isolate, self, "videoWidth", v8::Integer::New(isolate, 0));
    SetValue(isolate, self, "videoHeight", v8::Integer::New(isolate, 0));
    SetValue(isolate, self, "readyState", v8::Integer::New(isolate, 0));
    SetValue(isolate, self, "loop", v8::Boolean::New(isolate, false));
    SetValue(isolate, self, "paused", v8::Boolean::New(isolate, true));
    SetValue(isolate, self, "ended", v8::Boolean::New(isolate, false));
    SetValue(isolate, self, "muted", v8::Boolean::New(isolate, false));
    SetValue(isolate, self, "volume", v8::Number::New(isolate, 1.0));
    SetValue(isolate, self, "autoplay", v8::Boolean::New(isolate, false));
    SetValue(isolate, self, "style", v8::Object::New(isolate));
    InstallEventTarget(isolate, self);

    // currentTime: 取得はデコーダの現在位置、設定はシーク
    self->SetNativeDataProperty(ctx, Str(isolate, "currentTime"),
        [](v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
            VideoDecoder* d = Decoder(info.This());
            info.GetReturnValue().Set(
                v8::Number::New(info.GetIsolate(), d ? d->position : 0.0));
        },
        [](v8::Local<v8::Name>, v8::Local<v8::Value> value,
           const v8::PropertyCallbackInfo<void>& info) {
            if (!value->IsNumber()) return;
            VideoDecoder* d = Decoder(info.This());
            if (d) {
                d->Seek(value.As<v8::Number>()->Value());
            }
        }).Check();

    // setAttribute / getAttribute (player は playsinline を設定する)
    SetMethod(isolate, self, "setAttribute",
        [](const v8::FunctionCallbackInfo<v8::Value>& a) {
            if (a.Length() < 2) return;
            v8::Isolate* iso = a.GetIsolate();
            v8::Local<v8::Context> c = iso->GetCurrentContext();
            v8::Local<v8::Value> attrs;
            v8::Local<v8::Object> store;
            if (a.This()->Get(c, v8util::Str(iso, "__attrs")).ToLocal(&attrs) && attrs->IsObject()) {
                store = attrs.As<v8::Object>();
            } else {
                store = v8::Object::New(iso);
                SetValue(iso, a.This(), "__attrs", store);
            }
            store->Set(c, a[0], a[1]).Check();
        });
    SetMethod(isolate, self, "getAttribute",
        [](const v8::FunctionCallbackInfo<v8::Value>& a) {
            a.GetReturnValue().SetNull();
            if (a.Length() < 1) return;
            v8::Isolate* iso = a.GetIsolate();
            v8::Local<v8::Context> c = iso->GetCurrentContext();
            v8::Local<v8::Value> attrs;
            if (a.This()->Get(c, v8util::Str(iso, "__attrs")).ToLocal(&attrs) && attrs->IsObject()) {
                v8::Local<v8::Value> v;
                if (attrs.As<v8::Object>()->Get(c, a[0]).ToLocal(&v) && !v->IsUndefined()) {
                    a.GetReturnValue().Set(v);
                }
            }
        });

    // src セッター: 代入で読み込み開始
    self->SetNativeDataProperty(ctx, Str(isolate, "src"),
        [](v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
            v8::Local<v8::Value> v;
            if (info.This()->Get(info.GetIsolate()->GetCurrentContext(),
                v8util::Str(info.GetIsolate(), "__src")).ToLocal(&v)) {
                info.GetReturnValue().Set(v);
            }
        },
        [](v8::Local<v8::Name>, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<void>& info) {
            v8::Isolate* iso = info.GetIsolate();
            SetValue(iso, info.This(), "__src", value);
            LoadVideo(iso, info.This(), ToStdString(iso, value));
        }).Check();

    SetMethod(isolate, self, "load", [](const v8::FunctionCallbackInfo<v8::Value>& a) {
        v8::Isolate* iso = a.GetIsolate();
        v8::Local<v8::Value> src;
        if (a.This()->Get(iso->GetCurrentContext(), Str(iso, "__src")).ToLocal(&src) && src->IsString()) {
            LoadVideo(iso, a.This(), ToStdString(iso, src));
        }
    });
    SetMethod(isolate, self, "play", [](const v8::FunctionCallbackInfo<v8::Value>& a) {
        v8::Isolate* iso = a.GetIsolate();
        Decoder(a.This())->playing = true;
        SetValue(iso, a.This(), "paused", v8::Boolean::New(iso, false));
        SetValue(iso, a.This(), "ended", v8::Boolean::New(iso, false));
        auto r = v8::Promise::Resolver::New(iso->GetCurrentContext()).ToLocalChecked();
        r->Resolve(iso->GetCurrentContext(), v8::Undefined(iso)).Check();
        a.GetReturnValue().Set(r->GetPromise());
    });
    SetMethod(isolate, self, "pause", [](const v8::FunctionCallbackInfo<v8::Value>& a) {
        v8::Isolate* iso = a.GetIsolate();
        Decoder(a.This())->playing = false;
        SetValue(iso, a.This(), "paused", v8::Boolean::New(iso, true));
    });
    SetMethod(isolate, self, "canPlayType", [](const v8::FunctionCallbackInfo<v8::Value>& a) {
        a.GetReturnValue().Set(Str(a.GetIsolate(), "maybe"));
    });

    return self;
}

} // namespace next2d
