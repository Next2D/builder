#include "WorkerRuntime.h"

#include "HostContext.h"
#include "EventLoop.h"
#include "AssetLoader.h"
#include "bindings/Bindings.h"
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

bool IsOffscreen(v8::Isolate* isolate, v8::Local<v8::Object> obj)
{
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    v8::Local<v8::Value> v;
    return obj->Get(ctx, Str(isolate, "__isOffscreenCanvas")).ToLocal(&v) &&
           v->IsBoolean() && v.As<v8::Boolean>()->Value();
}

class SerializerDelegate : public v8::ValueSerializer::Delegate {
public:
    explicit SerializerDelegate(v8::Isolate* isolate) : isolate_(isolate) {}

    void ThrowDataCloneError(v8::Local<v8::String> message) override
    {
        isolate_->ThrowException(v8::Exception::Error(message));
    }

    // OffscreenCanvas をホストオブジェクトとして扱い WriteHostObject に回す。
    v8::Maybe<bool> IsHostObject(v8::Isolate*, v8::Local<v8::Object> object) override
    {
        return v8::Just(IsOffscreen(isolate_, object));
    }

    v8::Maybe<bool> WriteHostObject(v8::Isolate*, v8::Local<v8::Object> object) override
    {
        v8::Local<v8::Context> ctx = isolate_->GetCurrentContext();
        // Get 失敗時に空 Local を触らないようフォールバックを入れる
        v8::Local<v8::Value> w = v8::Undefined(isolate_);
        v8::Local<v8::Value> h = v8::Undefined(isolate_);
        (void) object->Get(ctx, Str(isolate_, "width")).ToLocal(&w);
        (void) object->Get(ctx, Str(isolate_, "height")).ToLocal(&h);
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

std::vector<uint8_t> SerializeMessage(v8::Isolate* isolate, v8::Local<v8::Context> source_ctx,
                                      v8::Local<v8::Value> message,
                                      v8::Local<v8::Value> /*transfer_list*/)
{
    v8::Context::Scope scope(source_ctx);
    SerializerDelegate delegate(isolate);
    v8::ValueSerializer serializer(isolate, &delegate);
    delegate.serializer = &serializer;

    serializer.WriteHeader();
    // 通常の ArrayBuffer は値としてシリアライズされる (transfer は将来最適化)。
    if (serializer.WriteValue(source_ctx, message).IsNothing()) {
        return {};
    }
    std::pair<uint8_t*, size_t> buffer = serializer.Release();
    std::vector<uint8_t> bytes(buffer.first, buffer.first + buffer.second);
    free(buffer.first);
    return bytes;
}

v8::MaybeLocal<v8::Value> DeserializeMessage(v8::Isolate* isolate,
                                             v8::Local<v8::Context> target_ctx,
                                             const std::vector<uint8_t>& bytes)
{
    v8::Context::Scope scope(target_ctx);
    HostContext* host = HostContext::From(isolate);
    DeserializerDelegate delegate(isolate, host);
    v8::ValueDeserializer deserializer(isolate, bytes.data(), bytes.size(), &delegate);
    delegate.deserializer = &deserializer;

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
    auto bytes = SerializeMessage(isolate, isolate->GetCurrentContext(), msg, transfer);
    if (!bytes.empty()) {
        instance->PostToMain(std::move(bytes));
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
    auto bytes = SerializeMessage(isolate, isolate->GetCurrentContext(), msg, transfer);
    if (!bytes.empty()) {
        instance->PostToWorker(std::move(bytes));
    }
}

} // namespace

bool WorkerInstance::Start()
{
    std::cerr << "[Worker] start: " << url_ << std::endl;
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
    // Vite の `?worker&inline` は blob: URL(= URL.createObjectURL(new Blob([code]))) を渡すため、
    // まず object URL レジストリ、次に assets を参照する。
    HostContext* host = HostContext::From(isolate_);
    std::string source;
    if (url_.rfind("blob:", 0) == 0) {
        if (!ResolveObjectURL(url_, &source)) {
            std::cerr << "[Worker] blob url not found: " << url_ << std::endl;
            return false;
        }
    } else {
        auto src = host->assets->ReadText(url_);
        if (!src) {
            std::cerr << "[Worker] script not found: " << url_ << std::endl;
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
        std::cerr << "[Worker] script eval failed: " << url_ << std::endl;
        return false;
    }
    isolate_->PerformMicrotaskCheckpoint();
    std::cerr << "[Worker] script evaluated" << std::endl;
    return true;
}

void WorkerInstance::Terminate() { terminated_ = true; }

void WorkerInstance::PostToWorker(std::vector<uint8_t> bytes)
{
    to_worker_.push_back(WorkerMessage{std::move(bytes)});
}

void WorkerInstance::PostToMain(std::vector<uint8_t> bytes)
{
    to_main_.push_back(WorkerMessage{std::move(bytes)});
}

void WorkerInstance::DeliverToWorker(double now_ms)
{
    if (terminated_) { to_worker_.clear(); return; }
    v8::HandleScope hs(isolate_);
    v8::Local<v8::Context> ctx = context();

    while (!to_worker_.empty()) {
        WorkerMessage m = std::move(to_worker_.front());
        to_worker_.pop_front();
        v8::Local<v8::Value> data;
        if (DeserializeMessage(isolate_, ctx, m.data).ToLocal(&data)) {
            DispatchMessage(isolate_, ctx, ctx->Global(), data);
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
        v8::Local<v8::Value> data;
        if (DeserializeMessage(isolate_, main_ctx, m.data).ToLocal(&data)) {
            DispatchMessage(isolate_, main_ctx, worker_obj, data);
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
    SetMethod(isolate_, worker, "terminate",
        [](const v8::FunctionCallbackInfo<v8::Value>& a) {
            auto* w = static_cast<WorkerInstance*>(a.Data().As<v8::External>()->Value());
            w->Terminate();
        });

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
