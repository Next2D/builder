#include "Bindings.h"

#include "HostContext.h"
#include "AssetLoader.h"
#include "v8/V8Util.h"
#include "v8/WeakHandle.h"

#include <cstring>
#include <vector>

namespace next2d {

using v8util::Str;
using v8util::ToStdString;

namespace {

// Response.arrayBuffer(): 内部データ(External の std::vector) から ArrayBuffer を作る。
void ResponseArrayBuffer(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::Value> ext =
        self->GetInternalField(0).As<v8::Value>();
    auto* data = static_cast<std::vector<uint8_t>*>(ext.As<v8::External>()->Value());

    std::unique_ptr<v8::BackingStore> store =
        v8::ArrayBuffer::NewBackingStore(isolate, data->size());
    if (!data->empty()) {
        std::memcpy(store->Data(), data->data(), data->size());
    }
    v8::Local<v8::ArrayBuffer> buffer =
        v8::ArrayBuffer::New(isolate, std::move(store));

    v8::Local<v8::Promise::Resolver> resolver =
        v8::Promise::Resolver::New(ctx).ToLocalChecked();
    resolver->Resolve(ctx, buffer).Check();
    args.GetReturnValue().Set(resolver->GetPromise());
}

void ResponseText(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

    v8::Local<v8::Object> self = args.This();
    auto* data = static_cast<std::vector<uint8_t>*>(
        self->GetInternalField(0).As<v8::External>()->Value());

    v8::Local<v8::String> text = v8::String::NewFromUtf8(
        isolate, reinterpret_cast<const char*>(data->data()),
        v8::NewStringType::kNormal, static_cast<int>(data->size())
    ).ToLocalChecked();

    v8::Local<v8::Promise::Resolver> resolver =
        v8::Promise::Resolver::New(ctx).ToLocalChecked();
    resolver->Resolve(ctx, text).Check();
    args.GetReturnValue().Set(resolver->GetPromise());
}

void ResponseJson(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

    v8::Local<v8::Object> self = args.This();
    auto* data = static_cast<std::vector<uint8_t>*>(
        self->GetInternalField(0).As<v8::External>()->Value());

    v8::Local<v8::String> text = v8::String::NewFromUtf8(
        isolate, reinterpret_cast<const char*>(data->data()),
        v8::NewStringType::kNormal, static_cast<int>(data->size())
    ).ToLocalChecked();

    v8::Local<v8::Promise::Resolver> resolver =
        v8::Promise::Resolver::New(ctx).ToLocalChecked();
    v8::Local<v8::Value> parsed;
    if (v8::JSON::Parse(ctx, text).ToLocal(&parsed)) {
        resolver->Resolve(ctx, parsed).Check();
    } else {
        resolver->Reject(ctx, v8::Exception::SyntaxError(Str(isolate, "Invalid JSON"))).Check();
    }
    args.GetReturnValue().Set(resolver->GetPromise());
}

v8::Local<v8::Object> MakeResponse(v8::Isolate* isolate, v8::Local<v8::Context> ctx,
                                   std::vector<uint8_t>&& bytes, bool ok, int status)
{
    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(isolate);
    tmpl->SetInternalFieldCount(1);
    v8::Local<v8::Object> response = tmpl->NewInstance(ctx).ToLocalChecked();

    auto* data = new std::vector<uint8_t>(std::move(bytes));
    response->SetInternalField(0, v8::External::New(isolate, data));

    // GC 時に解放
    v8util::AttachWeak(isolate, response, data);

    v8util::SetValue(isolate, response, "ok", v8::Boolean::New(isolate, ok));
    v8util::SetValue(isolate, response, "status", v8::Integer::New(isolate, status));
    v8util::SetMethod(isolate, response, "arrayBuffer", ResponseArrayBuffer);
    v8util::SetMethod(isolate, response, "text", ResponseText);
    v8util::SetMethod(isolate, response, "json", ResponseJson);
    return response;
}

// fetch(url) -> Promise<Response>
void Fetch(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    AssetLoader* assets = HostContext::From(isolate)->assets;

    v8::Local<v8::Promise::Resolver> resolver =
        v8::Promise::Resolver::New(ctx).ToLocalChecked();
    args.GetReturnValue().Set(resolver->GetPromise());

    if (args.Length() < 1) {
        resolver->Reject(ctx, v8::Exception::TypeError(Str(isolate, "url required"))).Check();
        return;
    }

    const std::string url = ToStdString(isolate, args[0]);
    auto bytes = assets->ReadBinary(url);
    if (!bytes) {
        // 404 相当の Response を返す (fetch は存在しないリソースでも reject しない)
        resolver->Resolve(ctx, MakeResponse(isolate, ctx, {}, false, 404)).Check();
        return;
    }

    resolver->Resolve(ctx, MakeResponse(isolate, ctx, std::move(*bytes), true, 200)).Check();
}

} // namespace

void InstallFetch(v8::Isolate* isolate, v8::Local<v8::Object> global, HostContext* /*host*/)
{
    v8util::SetMethod(isolate, global, "fetch", Fetch);
}

} // namespace next2d
