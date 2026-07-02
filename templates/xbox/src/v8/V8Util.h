// V8 バインディング用の小さなヘルパー群。
#pragma once

#include <v8.h>
#include <string>
#include <string_view>

namespace next2d::v8util {

// std::string -> v8::String (UTF-8)
inline v8::Local<v8::String> Str(v8::Isolate* isolate, std::string_view s)
{
    return v8::String::NewFromUtf8(
        isolate, s.data(), v8::NewStringType::kNormal,
        static_cast<int>(s.size())
    ).ToLocalChecked();
}

// v8::Value -> std::string (UTF-8)
inline std::string ToStdString(v8::Isolate* isolate, v8::Local<v8::Value> value)
{
    v8::String::Utf8Value utf8(isolate, value);
    return (*utf8) ? std::string(*utf8, utf8.length()) : std::string();
}

// object[name] = fn
inline void SetMethod(v8::Isolate* isolate,
                      v8::Local<v8::Object> target,
                      std::string_view name,
                      v8::FunctionCallback callback,
                      v8::Local<v8::Value> data = v8::Local<v8::Value>())
{
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    v8::Local<v8::Function> fn =
        v8::Function::New(ctx, callback, data).ToLocalChecked();
    fn->SetName(Str(isolate, name));
    target->Set(ctx, Str(isolate, name), fn).Check();
}

// object[name] = value
inline void SetValue(v8::Isolate* isolate,
                     v8::Local<v8::Object> target,
                     std::string_view name,
                     v8::Local<v8::Value> value)
{
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    target->Set(ctx, Str(isolate, name), value).Check();
}

// TypeError を投げる (バインディング内の引数検証で使用)
inline void ThrowTypeError(v8::Isolate* isolate, std::string_view message)
{
    isolate->ThrowException(v8::Exception::TypeError(Str(isolate, message)));
}

} // namespace next2d::v8util
