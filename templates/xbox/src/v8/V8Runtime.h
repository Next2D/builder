// V8Runtime: V8 プラットフォーム / Isolate / Context のライフサイクルを管理し、
// スクリプト(クラシック/ESM)の評価とマイクロタスクのポンプを提供する。
#pragma once

#include <v8.h>
#include <libplatform/libplatform.h>

#include <map>
#include <memory>
#include <string>

namespace next2d {

class HostContext;

class V8Runtime {
public:
    V8Runtime();
    ~V8Runtime();

    // process 全体で一度だけ呼ぶ (argv[0] を渡す)
    static void InitializeProcess(const char* exec_path);
    static void ShutdownProcess();

    // Isolate + Context を生成し、HostContext を data slot に紐付ける
    bool Initialize(HostContext* host);
    void Dispose();

    v8::Isolate* isolate() const { return isolate_; }
    v8::Local<v8::Context> context() const;

    // クラシックスクリプトを評価 (bootstrap.js 等)。失敗時 false。
    bool RunScript(const std::string& source, const std::string& name);

    // ES モジュールを評価 (Next2D アプリ本体)。base_dir は相対 import 解決用。
    bool RunModule(const std::string& source, const std::string& path);

    // Promise 等のマイクロタスクを処理する (タスク実行の直後に呼ぶ)
    void PumpMicrotasks();

    // 保留中のフォアグラウンドタスク(V8 プラットフォーム)を処理する
    void PumpPlatformTasks();

private:
    // ESM 解決
    v8::MaybeLocal<v8::Module> LoadModule(const std::string& path, const std::string& referrer_dir);
    static v8::MaybeLocal<v8::Module> ResolveModuleCallback(
        v8::Local<v8::Context> context,
        v8::Local<v8::String> specifier,
        v8::Local<v8::FixedArray> import_assertions,
        v8::Local<v8::Module> referrer);

    void ReportException(v8::TryCatch* try_catch);

    static std::unique_ptr<v8::Platform> platform_;

    v8::Isolate* isolate_ = nullptr;
    v8::Isolate::CreateParams create_params_;
    v8::Global<v8::Context> context_;
    HostContext* host_ = nullptr;

    // ESM キャッシュ: 絶対パス -> Module。resolve コールバックから参照する。
    std::map<std::string, v8::Global<v8::Module>> module_cache_;
    // Module identity hash -> 絶対パス (相対 import 解決の referrer 特定に使用)
    std::map<int, std::string> module_paths_;
};

} // namespace next2d
