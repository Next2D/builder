#include "EventTarget.h"

#include "v8/V8Util.h"

#include <string>

namespace next2d {

using v8util::Str;
using v8util::SetMethod;
using v8util::SetValue;
using v8util::ToStdString;

namespace {

void AddEventListener(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[1]->IsFunction()) {
        return;
    }
    const std::string type = ToStdString(isolate, args[0]);
    v8::Local<v8::Object> self = args.This();

    v8::Local<v8::Value> lv;
    v8::Local<v8::Object> listeners;
    if (self->Get(ctx, Str(isolate, "__listeners")).ToLocal(&lv) && lv->IsObject()) {
        listeners = lv.As<v8::Object>();
    } else {
        listeners = v8::Object::New(isolate);
        SetValue(isolate, self, "__listeners", listeners);
    }

    v8::Local<v8::Value> arrv;
    v8::Local<v8::Array> arr;
    if (listeners->Get(ctx, Str(isolate, type.c_str())).ToLocal(&arrv) && arrv->IsArray()) {
        arr = arrv.As<v8::Array>();
    } else {
        arr = v8::Array::New(isolate, 0);
        listeners->Set(ctx, Str(isolate, type.c_str()), arr).Check();
    }
    arr->Set(ctx, arr->Length(), args[1]).Check();
}

void RemoveEventListener(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    if (args.Length() < 2) {
        return;
    }
    const std::string type = ToStdString(isolate, args[0]);
    v8::Local<v8::Value> lv;
    if (!args.This()->Get(ctx, Str(isolate, "__listeners")).ToLocal(&lv) || !lv->IsObject()) {
        return;
    }
    v8::Local<v8::Value> arrv;
    if (!lv.As<v8::Object>()->Get(ctx, Str(isolate, type.c_str())).ToLocal(&arrv) || !arrv->IsArray()) {
        return;
    }
    // 一致する関数を null 化 (簡易)
    auto arr = arrv.As<v8::Array>();
    for (uint32_t i = 0; i < arr->Length(); ++i) {
        v8::Local<v8::Value> fn;
        if (arr->Get(ctx, i).ToLocal(&fn) && fn->StrictEquals(args[1])) {
            arr->Set(ctx, i, v8::Null(isolate)).Check();
        }
    }
}

void DispatchEventJS(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    if (args.Length() >= 1 && args[0]->IsObject()) {
        DispatchEvent(isolate, args.This(), args[0].As<v8::Object>());
    }
    args.GetReturnValue().Set(true);
}

} // namespace

void InstallEventTarget(v8::Isolate* isolate, v8::Local<v8::Object> target)
{
    SetMethod(isolate, target, "addEventListener", AddEventListener);
    SetMethod(isolate, target, "removeEventListener", RemoveEventListener);
    SetMethod(isolate, target, "dispatchEvent", DispatchEventJS);
}

void DispatchEvent(v8::Isolate* isolate, v8::Local<v8::Object> target,
                   v8::Local<v8::Object> event)
{
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    v8::Local<v8::Value> type_val;
    if (!event->Get(ctx, Str(isolate, "type")).ToLocal(&type_val)) {
        return;
    }
    const std::string type = ToStdString(isolate, type_val);

    // event.target / currentTarget を配送先に設定する。
    // player のポインタハンドラは `let t = e.target; t && (…, t.setPointerCapture(…), …)`
    // のように先頭で event.target をガードするため、未設定だと pointerdown/move が
    // 丸ごと no-op になり「ボタンが全く反応しない」。DOM 準拠でここで設定する。
    (void) event->Set(ctx, Str(isolate, "target"), target);
    (void) event->Set(ctx, Str(isolate, "currentTarget"), target);
    (void) event->Set(ctx, Str(isolate, "srcElement"), target);

    v8::Local<v8::Value> args1[1] = { event };

    // on<type> プロパティ
    v8::Local<v8::Value> on;
    const std::string onname = "on" + type;
    if (target->Get(ctx, Str(isolate, onname.c_str())).ToLocal(&on) && on->IsFunction()) {
        v8::TryCatch tc(isolate);
        (void) on.As<v8::Function>()->Call(ctx, target, 1, args1);
        v8util::ReportCaught(isolate, &tc, onname.c_str());
    }

    // __listeners[type]
    v8::Local<v8::Value> lv;
    if (target->Get(ctx, Str(isolate, "__listeners")).ToLocal(&lv) && lv->IsObject()) {
        v8::Local<v8::Value> arrv;
        if (lv.As<v8::Object>()->Get(ctx, Str(isolate, type.c_str())).ToLocal(&arrv) &&
            arrv->IsArray()) {
            auto arr = arrv.As<v8::Array>();
            for (uint32_t i = 0; i < arr->Length(); ++i) {
                v8::Local<v8::Value> fn;
                if (arr->Get(ctx, i).ToLocal(&fn) && fn->IsFunction()) {
                    v8::TryCatch tc(isolate);
                    (void) fn.As<v8::Function>()->Call(ctx, target, 1, args1);
                    v8util::ReportCaught(isolate, &tc, ("listener:" + type).c_str());
                }
            }
        }
    }
}

} // namespace next2d
