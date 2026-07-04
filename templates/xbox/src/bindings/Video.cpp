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
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                      nullptr, &flags, nullptr, &sample))) {
            return false;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { ended = true; return false; }
        if (!sample) return false;

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
        FireVideoEvent(isolate, self, "loadedmetadata");
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
            FireVideoEvent(isolate, obj, "ended");
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
    SetValue(isolate, self, "currentTime", v8::Number::New(isolate, 0));
    SetValue(isolate, self, "readyState", v8::Integer::New(isolate, 0));
    SetValue(isolate, self, "loop", v8::Boolean::New(isolate, false));
    SetValue(isolate, self, "style", v8::Object::New(isolate));
    InstallEventTarget(isolate, self);

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
        auto r = v8::Promise::Resolver::New(iso->GetCurrentContext()).ToLocalChecked();
        r->Resolve(iso->GetCurrentContext(), v8::Undefined(iso)).Check();
        a.GetReturnValue().Set(r->GetPromise());
    });
    SetMethod(isolate, self, "pause", [](const v8::FunctionCallbackInfo<v8::Value>& a) {
        Decoder(a.This())->playing = false;
    });
    SetMethod(isolate, self, "canPlayType", [](const v8::FunctionCallbackInfo<v8::Value>& a) {
        a.GetReturnValue().Set(Str(a.GetIsolate(), "maybe"));
    });

    return self;
}

} // namespace next2d
