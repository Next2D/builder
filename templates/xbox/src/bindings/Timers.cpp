#include "Bindings.h"

#include "HostContext.h"
#include "EventLoop.h"
#include "v8/V8Util.h"

namespace next2d {

namespace {

EventLoop* Loop(v8::Isolate* isolate)
{
    // 実行中の Context (main / worker) に紐づく EventLoop を返す。
    return CurrentEventLoop(isolate);
}

// setTimeout(fn, delay) / setInterval(fn, delay)
template <bool Repeat>
void SetTimer(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    if (args.Length() < 1 || !args[0]->IsFunction()) {
        v8util::ThrowTypeError(isolate, "callback function required");
        return;
    }

    v8::Local<v8::Function> callback = args[0].As<v8::Function>();
    double delay = 0.0;
    if (args.Length() >= 2 && args[1]->IsNumber()) {
        delay = args[1].As<v8::Number>()->Value();
    }

    const uint32_t id = Loop(isolate)->SetTimer(callback, delay, Repeat);
    args.GetReturnValue().Set(v8::Integer::NewFromUnsigned(isolate, id));
}

void ClearTimer(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    if (args.Length() >= 1 && args[0]->IsNumber()) {
        Loop(isolate)->ClearTimer(args[0].As<v8::Uint32>()->Value());
    }
}

void RequestAnimationFrame(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    if (args.Length() < 1 || !args[0]->IsFunction()) {
        v8util::ThrowTypeError(isolate, "callback function required");
        return;
    }
    const uint32_t id = Loop(isolate)->RequestAnimationFrame(args[0].As<v8::Function>());
    args.GetReturnValue().Set(v8::Integer::NewFromUnsigned(isolate, id));
}

void CancelAnimationFrame(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    if (args.Length() >= 1 && args[0]->IsNumber()) {
        Loop(isolate)->CancelAnimationFrame(args[0].As<v8::Uint32>()->Value());
    }
}

void PerformanceNow(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    args.GetReturnValue().Set(v8::Number::New(isolate, Loop(isolate)->Now()));
}

} // namespace

void InstallTimers(v8::Isolate* isolate, v8::Local<v8::Object> global, HostContext* /*host*/)
{
    v8util::SetMethod(isolate, global, "setTimeout",  SetTimer<false>);
    v8util::SetMethod(isolate, global, "setInterval", SetTimer<true>);
    v8util::SetMethod(isolate, global, "clearTimeout",  ClearTimer);
    v8util::SetMethod(isolate, global, "clearInterval", ClearTimer);
    v8util::SetMethod(isolate, global, "requestAnimationFrame", RequestAnimationFrame);
    v8util::SetMethod(isolate, global, "cancelAnimationFrame",  CancelAnimationFrame);

    // performance.now()
    v8::Local<v8::Object> performance = v8::Object::New(isolate);
    v8util::SetMethod(isolate, performance, "now", PerformanceNow);
    v8util::SetValue(isolate, global, "performance", performance);
}

} // namespace next2d
