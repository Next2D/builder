// V8 バインディング用の小さなヘルパー群。
#pragma once

#include <v8.h>
#include <iostream>
#include <fstream>
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

// TryCatch が例外を保持していれば stderr へ報告する。
// rAF / タイマー / onmessage / イベントリスナーのコールバックは TryCatch で
// 保護されるが、報告しないと例外が無音で消えて原因究明が不可能になる
// (レンダラ worker が毎フレーム throw していても黒画面にしか見えない)。
// 例外を stderr に加えてログファイルにも残す。GUI 実行(ダブルクリック)では stderr が
// 捨てられて例外が見えないため、rAF/タイマー/リスナーで無音死する不具合(例: Tween の
// rAF チェーンが UPDATE リスナの throw で再登録されず停止)を実行後に追跡できるように
// する。暴走ログ防止に先頭 200 件までに制限。ファイルは実行ディレクトリの next2d-error.log。
inline void AppendErrorLog(const std::string& line)
{
    static int written = 0;
    if (written >= 200) {
        return;
    }
    ++written;
    std::ofstream ofs("next2d-error.log", std::ios::app);
    if (ofs) {
        ofs << line << std::endl;
    }
}

inline void ReportCaught(v8::Isolate* isolate, v8::TryCatch* tc, const char* where)
{
    if (!tc->HasCaught()) {
        return;
    }
    v8::HandleScope hs(isolate);
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    const std::string text = ToStdString(isolate, tc->Exception());
    const std::string head = std::string("[JS] exception in ") + where + ": " + text;
    std::cerr << head << std::endl;
    AppendErrorLog(head);
    v8::Local<v8::Value> stack;
    if (!ctx.IsEmpty() && tc->StackTrace(ctx).ToLocal(&stack)) {
        const std::string s = ToStdString(isolate, stack);
        if (!s.empty() && s != text) {
            std::cerr << s << std::endl;
            AppendErrorLog(s);
        }
    }
    v8::Local<v8::Message> message = tc->Message();
    if (!message.IsEmpty() && !ctx.IsEmpty()) {
        const std::string loc = std::string("  at ")
            + ToStdString(isolate, message->GetScriptOrigin().ResourceName())
            + ":" + std::to_string(message->GetLineNumber(ctx).FromMaybe(0));
        std::cerr << loc << std::endl;
        AppendErrorLog(loc);
    }
}

} // namespace next2d::v8util
