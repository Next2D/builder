#include "Bindings.h"

#include "v8/V8Util.h"

#include <iostream>

namespace next2d {

using v8util::Str;
using v8util::ToStdString;

namespace {

std::string FormatArgs(v8::Isolate* isolate, const v8::FunctionCallbackInfo<v8::Value>& args)
{
    std::string out;
    for (int i = 0; i < args.Length(); ++i) {
        if (i > 0) {
            out += ' ';
        }
        out += ToStdString(isolate, args[i]);
    }
    return out;
}

void Log(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    std::cout << FormatArgs(args.GetIsolate(), args) << std::endl;
}

void Error(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    // console.error / console.warn は GUI 実行では stderr が見えないため、
    // next2d-error.log にもミラーする (player 側のエラー・警告や WebGPU の
    // error scope 結果などを掴む)。
    const std::string msg = FormatArgs(args.GetIsolate(), args);
    std::cerr << msg << std::endl;
    v8util::AppendErrorLog("[console] " + msg);
}

} // namespace

void InstallConsole(v8::Isolate* isolate, v8::Local<v8::Object> global)
{
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    v8::Local<v8::Object> console = v8::Object::New(isolate);

    v8util::SetMethod(isolate, console, "log",   Log);
    v8util::SetMethod(isolate, console, "info",  Log);
    v8util::SetMethod(isolate, console, "debug", Log);
    v8util::SetMethod(isolate, console, "warn",  Error);
    v8util::SetMethod(isolate, console, "error", Error);

    global->Set(ctx, Str(isolate, "console"), console).Check();
}

} // namespace next2d
