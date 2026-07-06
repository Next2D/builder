#include "WorkerRuntime.h"

#include "HostContext.h"
#include "EventLoop.h"
#include "AssetLoader.h"
#include "bindings/Bindings.h"
#include "bindings/ImageSource.h"
#include "platform/ImageTypes.h"
#include "v8/V8Util.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace next2d {

using v8util::Str;
using v8util::SetMethod;
using v8util::SetValue;
using v8util::ToStdString;

// ===========================================================================
// structured clone (ValueSerializer / ValueDeserializer)
// ===========================================================================
namespace {

// ログ用: data:/blob: URL は長大になるため先頭だけに切り詰める
std::string ShortUrl(const std::string& url)
{
    if (url.size() <= 64) {
        return url;
    }
    return url.substr(0, 64) + "... (" + std::to_string(url.size()) + " chars)";
}

// base64 デコード (data: URL 用)。不正文字があれば false。
bool DecodeBase64(const std::string& in, std::string* out)
{
    auto val6 = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out->clear();
    out->reserve(in.size() * 3 / 4);
    int val = 0;
    int bits = -8;
    for (unsigned char c : in) {
        if (c == '=') {
            break;
        }
        if (c == '\r' || c == '\n' || c == ' ') {
            continue;
        }
        const int d = val6(c);
        if (d < 0) {
            return false;
        }
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 0) {
            out->push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return true;
}

// %xx パーセントデコード (非 base64 の data: URL 用)
std::string PercentDecode(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(s[i + 1]);
            const int lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

bool HasBoolMarker(v8::Isolate* isolate, v8::Local<v8::Object> obj, const char* name)
{
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    v8::Local<v8::Value> v;
    return obj->Get(ctx, Str(isolate, name)).ToLocal(&v) &&
           v->IsBoolean() && v.As<v8::Boolean>()->Value();
}

bool IsOffscreen(v8::Isolate* isolate, v8::Local<v8::Object> obj)
{
    return HasBoolMarker(isolate, obj, "__isOffscreenCanvas");
}

bool IsImageBitmapObject(v8::Isolate* isolate, v8::Local<v8::Object> obj)
{
    return HasBoolMarker(isolate, obj, "__isImageBitmap");
}

// WriteHostObject / ReadHostObject のタグ
constexpr uint32_t kHostObjectOffscreenCanvas = 1;
constexpr uint32_t kHostObjectImageBitmap = 2;

// メッセージトレース: 定常時のログ洪水を防ぐため、最初の 120 件のあとは
// 200 件ごとに 1 回だけ出力する (render は 60fps で双方向に流れる)。
bool TraceMessage()
{
    static uint64_t count = 0;
    ++count;
    return count <= 120 || count % 200 == 0;
}

class SerializerDelegate : public v8::ValueSerializer::Delegate {
public:
    explicit SerializerDelegate(v8::Isolate* isolate) : isolate_(isolate) {}

    void ThrowDataCloneError(v8::Local<v8::String> message) override
    {
        isolate_->ThrowException(v8::Exception::Error(message));
    }

    // すべてのオブジェクトで IsHostObject を照会させる。
    // OffscreenCanvas は ObjectTemplate 由来ではない plain object のため、
    // これが無いと V8 は IsHostObject を呼ばず、getContext 等の関数プロパティで
    // 「could not be cloned」になる。
    bool HasCustomHostObject(v8::Isolate*) override { return true; }

    // OffscreenCanvas / ImageBitmap をホストオブジェクトとして WriteHostObject に回す。
    v8::Maybe<bool> IsHostObject(v8::Isolate*, v8::Local<v8::Object> object) override
    {
        return v8::Just(IsOffscreen(isolate_, object) ||
                        IsImageBitmapObject(isolate_, object));
    }

    v8::Maybe<bool> WriteHostObject(v8::Isolate*, v8::Local<v8::Object> object) override
    {
        v8::Local<v8::Context> ctx = isolate_->GetCurrentContext();

        // ImageBitmap: ピクセルごと複製する (render メッセージの imageBitmaps 用)
        if (IsImageBitmapObject(isolate_, object)) {
            const uint8_t* px = nullptr;
            uint32_t w = 0, h = 0;
            if (!GetImageSourcePixels(isolate_, object, &px, &w, &h) || !px) {
                isolate_->ThrowException(v8::Exception::Error(
                    Str(isolate_, "ImageBitmap clone failed")));
                return v8::Nothing<bool>();
            }
            serializer->WriteUint32(kHostObjectImageBitmap);
            serializer->WriteUint32(w);
            serializer->WriteUint32(h);
            serializer->WriteRawBytes(px, static_cast<size_t>(w) * h * 4);
            return v8::Just(true);
        }

        // OffscreenCanvas
        // Get 失敗時に空 Local を触らないようフォールバックを入れる
        v8::Local<v8::Value> w = v8::Undefined(isolate_);
        v8::Local<v8::Value> h = v8::Undefined(isolate_);
        (void) object->Get(ctx, Str(isolate_, "width")).ToLocal(&w);
        (void) object->Get(ctx, Str(isolate_, "height")).ToLocal(&h);
        serializer->WriteUint32(kHostObjectOffscreenCanvas);
        serializer->WriteUint32(w->IsNumber() ? static_cast<uint32_t>(w.As<v8::Number>()->Value()) : 0);
        serializer->WriteUint32(h->IsNumber() ? static_cast<uint32_t>(h.As<v8::Number>()->Value()) : 0);
        return v8::Just(true);
    }

    v8::ValueSerializer* serializer = nullptr;

private:
    v8::Isolate* isolate_;
};

class DeserializerDelegate : public v8::ValueDeserializer::Delegate {
public:
    DeserializerDelegate(v8::Isolate* isolate, HostContext* host)
        : isolate_(isolate), host_(host) {}

    v8::MaybeLocal<v8::Object> ReadHostObject(v8::Isolate*) override
    {
        uint32_t tag = 0;
        if (!deserializer->ReadUint32(&tag)) {
            return v8::MaybeLocal<v8::Object>();
        }

        if (tag == kHostObjectImageBitmap) {
            uint32_t w = 0, h = 0;
            const void* px = nullptr;
            if (!deserializer->ReadUint32(&w) || !deserializer->ReadUint32(&h) ||
                !deserializer->ReadRawBytes(static_cast<size_t>(w) * h * 4, &px)) {
                return v8::MaybeLocal<v8::Object>();
            }
            auto* img = new DecodedImage();
            img->width = w;
            img->height = h;
            img->rgba.assign(static_cast<const uint8_t*>(px),
                             static_cast<const uint8_t*>(px) + static_cast<size_t>(w) * h * 4);
            return WrapImageBitmap(isolate_, img);
        }

        uint32_t w = 0, h = 0;
        deserializer->ReadUint32(&w);
        deserializer->ReadUint32(&h);
        return CreateOffscreenCanvas(isolate_, host_, static_cast<int>(w), static_cast<int>(h));
    }

    v8::ValueDeserializer* deserializer = nullptr;

private:
    v8::Isolate* isolate_;
    HostContext* host_;
};

} // namespace

WorkerMessage SerializeMessage(v8::Isolate* isolate, v8::Local<v8::Context> source_ctx,
                               v8::Local<v8::Value> message,
                               v8::Local<v8::Value> transfer_list)
{
    v8::Context::Scope scope(source_ctx);
    SerializerDelegate delegate(isolate);
    v8::ValueSerializer serializer(isolate, &delegate);
    delegate.serializer = &serializer;

    // transfer_list の ArrayBuffer は中身を埋め込まず所有権を移譲する
    // (player の render キューは数十 MB — 毎フレームのコピーは不可)。
    std::vector<v8::Local<v8::ArrayBuffer>> transfer_abs;
    if (!transfer_list.IsEmpty() && transfer_list->IsArray()) {
        auto arr = transfer_list.As<v8::Array>();
        for (uint32_t i = 0; i < arr->Length(); ++i) {
            v8::Local<v8::Value> item;
            if (arr->Get(source_ctx, i).ToLocal(&item) && item->IsArrayBuffer()) {
                auto ab = item.As<v8::ArrayBuffer>();
                if (ab->IsDetachable()) {
                    serializer.TransferArrayBuffer(
                        static_cast<uint32_t>(transfer_abs.size()), ab);
                    transfer_abs.push_back(ab);
                }
            }
        }
    }

    serializer.WriteHeader();
    if (serializer.WriteValue(source_ctx, message).IsNothing()) {
        return {};
    }

    WorkerMessage result;
    std::pair<uint8_t*, size_t> buffer = serializer.Release();
    result.data.assign(buffer.first, buffer.first + buffer.second);
    free(buffer.first);

    // 成功後に detach (ブラウザの transferable と同じく送信側では使えなくなる)
    for (auto& ab : transfer_abs) {
        result.transfers.push_back(ab->GetBackingStore());
        (void) ab->Detach(v8::Local<v8::Value>());
    }
    return result;
}

v8::MaybeLocal<v8::Value> DeserializeMessage(v8::Isolate* isolate,
                                             v8::Local<v8::Context> target_ctx,
                                             const WorkerMessage& message)
{
    v8::Context::Scope scope(target_ctx);
    HostContext* host = HostContext::From(isolate);
    DeserializerDelegate delegate(isolate, host);
    v8::ValueDeserializer deserializer(isolate, message.data.data(),
                                       message.data.size(), &delegate);
    delegate.deserializer = &deserializer;

    // transfer された ArrayBuffer を同じ backing store で再装着する (ゼロコピー)
    for (size_t i = 0; i < message.transfers.size(); ++i) {
        v8::Local<v8::ArrayBuffer> ab =
            v8::ArrayBuffer::New(isolate, message.transfers[i]);
        deserializer.TransferArrayBuffer(static_cast<uint32_t>(i), ab);
    }

    if (deserializer.ReadHeader(target_ctx).IsNothing()) {
        return v8::MaybeLocal<v8::Value>();
    }
    return deserializer.ReadValue(target_ctx);
}

// ===========================================================================
// メッセージ配送ヘルパー
// ===========================================================================
namespace {

// target(self / worker) の onmessage と addEventListener('message') 登録分へ配送する。
void DispatchMessage(v8::Isolate* isolate, v8::Local<v8::Context> ctx,
                     v8::Local<v8::Object> target, v8::Local<v8::Value> data)
{
    v8::Context::Scope scope(ctx);
    v8::Local<v8::Object> event = v8::Object::New(isolate);
    SetValue(isolate, event, "data", data);
    SetValue(isolate, event, "type", Str(isolate, "message"));
    v8::Local<v8::Value> args[1] = { event };

    // onmessage プロパティ
    v8::Local<v8::Value> on;
    if (target->Get(ctx, Str(isolate, "onmessage")).ToLocal(&on) && on->IsFunction()) {
        v8::TryCatch tc(isolate);
        (void) on.As<v8::Function>()->Call(ctx, target, 1, args);
        v8util::ReportCaught(isolate, &tc, "onmessage");
    }
    // __messageListeners 配列
    v8::Local<v8::Value> listeners;
    if (target->Get(ctx, Str(isolate, "__messageListeners")).ToLocal(&listeners) &&
        listeners->IsArray()) {
        auto arr = listeners.As<v8::Array>();
        for (uint32_t i = 0; i < arr->Length(); ++i) {
            v8::Local<v8::Value> fn;
            if (arr->Get(ctx, i).ToLocal(&fn) && fn->IsFunction()) {
                v8::TryCatch tc(isolate);
                (void) fn.As<v8::Function>()->Call(ctx, target, 1, args);
                v8util::ReportCaught(isolate, &tc, "message listener");
            }
        }
    }
}

// addEventListener(type, fn): type=='message' のみ __messageListeners へ蓄積。
void AddEventListener(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[1]->IsFunction()) return;
    if (ToStdString(isolate, args[0]) != "message") return;

    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::Value> listeners;
    v8::Local<v8::Array> arr;
    if (self->Get(ctx, Str(isolate, "__messageListeners")).ToLocal(&listeners) &&
        listeners->IsArray()) {
        arr = listeners.As<v8::Array>();
    } else {
        arr = v8::Array::New(isolate, 0);
        SetValue(isolate, self, "__messageListeners", arr);
    }
    arr->Set(ctx, arr->Length(), args[1]).Check();
}

} // namespace

// ===========================================================================
// WorkerInstance
// ===========================================================================
WorkerInstance::WorkerInstance(WorkerRuntime* runtime, v8::Isolate* isolate, std::string url)
    : runtime_(runtime), isolate_(isolate), url_(std::move(url))
{
}

WorkerInstance::~WorkerInstance() = default;

v8::Local<v8::Context> WorkerInstance::context() const
{
    return context_.Get(isolate_);
}

namespace {

// worker 側 self.postMessage: worker→main へ投函する。
void WorkerSelfPostMessage(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    auto* instance = static_cast<WorkerInstance*>(args.Data().As<v8::External>()->Value());
    v8::Local<v8::Value> msg = args.Length() > 0 ? args[0] : v8::Undefined(isolate).As<v8::Value>();
    v8::Local<v8::Value> transfer = args.Length() > 1 ? args[1] : v8::Undefined(isolate).As<v8::Value>();
    auto message = SerializeMessage(isolate, isolate->GetCurrentContext(), msg, transfer);
    if (TraceMessage()) {
        std::cerr << "[Worker] worker->main postMessage: " << message.data.size()
                  << " bytes (+" << message.transfers.size() << " transfers)" << std::endl;
    }
    if (!message.data.empty()) {
        instance->PostToMain(std::move(message));
    }
}

// worker 側 importScripts(url...): 同期でクラシックスクリプトを評価する。
void WorkerImportScripts(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    HostContext* host = HostContext::From(isolate);
    for (int i = 0; i < args.Length(); ++i) {
        const std::string url = ToStdString(isolate, args[i]);
        auto src = host->assets->ReadText(url);
        if (!src) continue;
        v8::TryCatch tc(isolate);
        v8::ScriptOrigin origin(Str(isolate, url));
        v8::Local<v8::String> code = Str(isolate, *src);
        v8::Local<v8::Script> script;
        if (v8::Script::Compile(ctx, code, &origin).ToLocal(&script)) {
            (void) script->Run(ctx);
        }
    }
}

// main 側 worker.postMessage: main→worker へ投函する。
void MainWorkerPostMessage(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    auto* instance = static_cast<WorkerInstance*>(args.Data().As<v8::External>()->Value());
    v8::Local<v8::Value> msg = args.Length() > 0 ? args[0] : v8::Undefined(isolate).As<v8::Value>();
    v8::Local<v8::Value> transfer = args.Length() > 1 ? args[1] : v8::Undefined(isolate).As<v8::Value>();
    auto message = SerializeMessage(isolate, isolate->GetCurrentContext(), msg, transfer);
    if (TraceMessage()) {
        std::cerr << "[Worker] main->worker postMessage: " << message.data.size()
                  << " bytes (+" << message.transfers.size() << " transfers)" << std::endl;
    }
    if (!message.data.empty()) {
        instance->PostToWorker(std::move(message));
    }
}

} // namespace

bool WorkerInstance::Start()
{
    std::cerr << "[Worker] start: " << ShortUrl(url_) << std::endl;
    v8::HandleScope handle_scope(isolate_);

    v8::Local<v8::Context> context = v8::Context::New(isolate_);
    context_.Reset(isolate_, context);
    std::cerr << "[Worker] context created" << std::endl;

    // worker 専用 EventLoop を Context の embedder slot に登録
    loop_ = std::make_unique<EventLoop>(isolate_);
    context->SetAlignedPointerInEmbedderData(kEventLoopEmbedderSlot, loop_.get());

    v8::Context::Scope context_scope(context);
    v8::Local<v8::Object> global = context->Global();

    // ブラウザ相当環境一式 (console/timers/fetch/image/audio/webgpu/canvas/dom)
    InstallGlobalBindings(isolate_, global, HostContext::From(isolate_));
    std::cerr << "[Worker] bindings installed" << std::endl;

    // worker スコープには document/window を出さない (ライブラリの環境判定対策)。
    global->Delete(context, Str(isolate_, "document")).Check();
    global->Delete(context, Str(isolate_, "window")).Check();

    // worker 固有: self / postMessage / importScripts / close / addEventListener
    v8::Local<v8::External> self_ext = v8::External::New(isolate_, this);
    SetValue(isolate_, global, "self", global);
    global->Set(context, Str(isolate_, "postMessage"),
        v8::Function::New(context, WorkerSelfPostMessage, self_ext).ToLocalChecked()).Check();
    SetMethod(isolate_, global, "importScripts", WorkerImportScripts);
    SetMethod(isolate_, global, "addEventListener", AddEventListener);
    SetMethod(isolate_, global, "close", [](const v8::FunctionCallbackInfo<v8::Value>&) {});

    // 入れ子 Worker を許可 (ZlibInflate 等)。
    runtime_->InstallOnGlobal(global);

    // ワーカースクリプトを読み込んで評価 (クラシック。ESM worker は «EXTEND»)。
    // Vite の `?worker&inline` は data:application/javascript;base64,... を渡す
    // (バージョンによっては blob: URL)。data: / blob: / assets の順で解決する。
    HostContext* host = HostContext::From(isolate_);
    std::string source;
    if (url_.rfind("data:", 0) == 0) {
        const auto comma = url_.find(',');
        if (comma == std::string::npos) {
            std::cerr << "[Worker] malformed data url: " << ShortUrl(url_) << std::endl;
            return false;
        }
        const std::string meta = url_.substr(5, comma - 5);   // 例: application/javascript;base64
        const std::string payload = url_.substr(comma + 1);
        if (meta.find(";base64") != std::string::npos) {
            if (!DecodeBase64(payload, &source)) {
                std::cerr << "[Worker] base64 decode failed: " << ShortUrl(url_) << std::endl;
                return false;
            }
        } else {
            source = PercentDecode(payload);
        }
    } else if (url_.rfind("blob:", 0) == 0) {
        if (!ResolveObjectURL(url_, &source)) {
            std::cerr << "[Worker] blob url not found: " << ShortUrl(url_) << std::endl;
            return false;
        }
    } else {
        auto src = host->assets->ReadText(url_);
        if (!src) {
            std::cerr << "[Worker] script not found: " << ShortUrl(url_) << std::endl;
            return false;
        }
        source = std::move(*src);
    }
    std::cerr << "[Worker] source resolved (" << source.size() << " bytes)" << std::endl;
    v8::TryCatch tc(isolate_);
    v8::ScriptOrigin origin(Str(isolate_, url_));
    v8::Local<v8::String> code = Str(isolate_, source);
    v8::Local<v8::Script> script;
    if (!v8::Script::Compile(context, code, &origin).ToLocal(&script) ||
        script->Run(context).IsEmpty()) {
        std::cerr << "[Worker] script eval failed: " << ShortUrl(url_) << std::endl;
        if (tc.HasCaught()) {
            v8::String::Utf8Value exception(isolate_, tc.Exception());
            std::cerr << "[Worker]   exception: " << (*exception ? *exception : "?") << std::endl;
            v8::Local<v8::Message> message = tc.Message();
            if (!message.IsEmpty()) {
                std::cerr << "[Worker]   at line "
                          << message->GetLineNumber(context).FromMaybe(0) << std::endl;
            }
        }
        return false;
    }
    // NOTE: ここで PerformMicrotaskCheckpoint は呼ばない。Start は JS コールバック
    // (new Worker) の最中に呼ばれるため、JS 実行中のチェックポイントは V8 の規約違反。
    // マイクロタスクはメインループの PumpMicrotasks で処理される。
    std::cerr << "[Worker] script evaluated" << std::endl;
    return true;
}

void WorkerInstance::Terminate() { terminated_ = true; }

void WorkerInstance::PostToWorker(WorkerMessage message)
{
    to_worker_.push_back(std::move(message));
}

void WorkerInstance::PostToMain(WorkerMessage message)
{
    to_main_.push_back(std::move(message));
}

void WorkerInstance::DeliverToWorker(double now_ms)
{
    if (terminated_) { to_worker_.clear(); return; }
    v8::HandleScope hs(isolate_);
    v8::Local<v8::Context> ctx = context();

    while (!to_worker_.empty()) {
        WorkerMessage m = std::move(to_worker_.front());
        to_worker_.pop_front();
        const bool trace = TraceMessage();
        if (trace) {
            std::cerr << "[Worker] deliver main->worker (" << m.data.size() << " bytes)" << std::endl;
        }
        v8::Local<v8::Value> data;
        if (DeserializeMessage(isolate_, ctx, m).ToLocal(&data)) {
            DispatchMessage(isolate_, ctx, ctx->Global(), data);
            if (trace) {
                std::cerr << "[Worker] worker onmessage done" << std::endl;
            }
        } else {
            std::cerr << "[Worker] main->worker deserialize failed ("
                      << m.data.size() << " bytes)" << std::endl;
        }
    }

    // worker のタイマー / rAF (レンダラ worker はここで描画コマンドを積む)
    v8::Context::Scope cs(ctx);
    loop_->PumpTimers();
    isolate_->PerformMicrotaskCheckpoint();
    loop_->RunAnimationFrame(now_ms);
    isolate_->PerformMicrotaskCheckpoint();
}

void WorkerInstance::DeliverToMain()
{
    if (main_worker_object.IsEmpty()) { to_main_.clear(); return; }
    v8::HandleScope hs(isolate_);
    v8::Local<v8::Object> worker_obj = main_worker_object.Get(isolate_);
    v8::Local<v8::Context> main_ctx = worker_obj->GetCreationContextChecked();

    while (!to_main_.empty()) {
        WorkerMessage m = std::move(to_main_.front());
        to_main_.pop_front();
        const bool trace = TraceMessage();
        if (trace) {
            std::cerr << "[Worker] deliver worker->main (" << m.data.size() << " bytes)" << std::endl;
        }
        v8::Local<v8::Value> data;
        if (DeserializeMessage(isolate_, main_ctx, m).ToLocal(&data)) {
            DispatchMessage(isolate_, main_ctx, worker_obj, data);
            if (trace) {
                std::cerr << "[Worker] main onmessage done" << std::endl;
            }
        } else {
            std::cerr << "[Worker] worker->main deserialize failed ("
                      << m.data.size() << " bytes)" << std::endl;
        }
    }
}

void WorkerInstance::Deliver(double now_ms)
{
    DeliverToWorker(now_ms);
    DeliverToMain();
}

// ===========================================================================
// WorkerRuntime
// ===========================================================================
WorkerRuntime::WorkerRuntime(v8::Isolate* isolate, HostContext* host)
    : isolate_(isolate), host_(host)
{
}

WorkerRuntime::~WorkerRuntime() = default;

v8::Local<v8::Object> WorkerRuntime::CreateWorker(const std::string& url)
{
    v8::Local<v8::Context> ctx = isolate_->GetCurrentContext();

    auto instance = std::make_unique<WorkerInstance>(this, isolate_, url);
    WorkerInstance* ptr = instance.get();

    // main 側 Worker オブジェクト
    v8::Local<v8::Object> worker = v8::Object::New(isolate_);
    v8::Local<v8::External> ext = v8::External::New(isolate_, ptr);
    worker->Set(ctx, Str(isolate_, "postMessage"),
        v8::Function::New(ctx, MainWorkerPostMessage, ext).ToLocalChecked()).Check();
    SetMethod(isolate_, worker, "addEventListener", AddEventListener);
    // terminate は WorkerInstance* を data (External) で受け取る必要があるため
    // SetMethod (data なし) ではなく Function::New で設置する。
    worker->Set(ctx, Str(isolate_, "terminate"),
        v8::Function::New(ctx,
            [](const v8::FunctionCallbackInfo<v8::Value>& a) {
                if (!a.Data()->IsExternal()) {
                    return;
                }
                auto* w = static_cast<WorkerInstance*>(a.Data().As<v8::External>()->Value());
                w->Terminate();
            }, ext).ToLocalChecked()).Check();

    ptr->main_worker_object.Reset(isolate_, worker);

    if (!ptr->Start()) {
        // 起動失敗でも Worker オブジェクトは返す (エラーは配送されない)
    }
    workers_.push_back(std::move(instance));
    return worker;
}

namespace {

// new Worker(url, options)
void WorkerConstructor(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    if (!args.IsConstructCall()) {
        v8util::ThrowTypeError(isolate, "Worker must be called with new");
        return;
    }
    auto* runtime = static_cast<WorkerRuntime*>(args.Data().As<v8::External>()->Value());
    std::string url;
    if (args.Length() > 0) {
        // new Worker(new URL(...)) にも対応するため toString で URL 文字列化
        url = ToStdString(isolate, args[0]);
    }
    args.GetReturnValue().Set(runtime->CreateWorker(url));
}

} // namespace

void WorkerRuntime::InstallOnGlobal(v8::Local<v8::Object> global)
{
    v8::Local<v8::Context> ctx = isolate_->GetCurrentContext();
    v8::Local<v8::External> ext = v8::External::New(isolate_, this);
    v8::Local<v8::Function> ctor =
        v8::Function::New(ctx, WorkerConstructor, ext).ToLocalChecked();
    ctor->SetName(Str(isolate_, "Worker"));
    global->Set(ctx, Str(isolate_, "Worker"), ctor).Check();
}

void WorkerRuntime::Pump(double now_ms)
{
    // 配送中の JS が new Worker() を呼ぶと workers_ に push_back され
    // vector が再割当されるため、イテレータではなく添字で回す。
    for (size_t i = 0; i < workers_.size(); ++i) {
        workers_[i]->Deliver(now_ms);
    }
}

// Bindings.h の InstallWorker: WorkerRuntime を使って Worker を設置する。
void InstallWorker(v8::Isolate* isolate, v8::Local<v8::Object> global, HostContext* host)
{
    if (host->workers) {
        host->workers->InstallOnGlobal(global);
    }
}

} // namespace next2d
