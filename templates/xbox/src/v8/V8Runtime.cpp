#include "V8Runtime.h"

#include "V8Util.h"
#include "HostContext.h"
#include "bindings/Bindings.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace next2d {

std::unique_ptr<v8::Platform> V8Runtime::platform_;

// isolate data slot 1: V8Runtime* (ESM resolve コールバックから参照)
constexpr uint32_t kRuntimeSlot = 1;

namespace {

std::string ReadTextFile(const std::string& path)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return {};
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

} // namespace

V8Runtime::V8Runtime() = default;

V8Runtime::~V8Runtime()
{
    Dispose();
}

void V8Runtime::InitializeProcess(const char* exec_path)
{
    // Xbox(GDK) / Nintendo Switch のリテール環境は動的コード生成(JIT)を禁止する。
    // V8 を jitless (Ignition インタプリタのみ・TurboFan/Sparkplug/RWXページ無し) で動かす。
    // prebuilt V8 (build-v8.yml) は v8_jitless=true + turbofan/wasm 無効でビルドされる。
    // wasm/opt 系フラグはそのビルドに存在しないため渡さない (未知フラグ警告を避ける)。
    v8::V8::SetFlagsFromString("--jitless");

    v8::V8::InitializeICUDefaultLocation(exec_path);
    v8::V8::InitializeExternalStartupData(exec_path);
    platform_ = v8::platform::NewDefaultPlatform();
    v8::V8::InitializePlatform(platform_.get());
    v8::V8::Initialize();
}

void V8Runtime::ShutdownProcess()
{
    v8::V8::Dispose();
    v8::V8::DisposePlatform();
    platform_.reset();
}

bool V8Runtime::Initialize(HostContext* host)
{
    host_ = host;

    create_params_.array_buffer_allocator =
        v8::ArrayBuffer::Allocator::NewDefaultAllocator();

    isolate_ = v8::Isolate::New(create_params_);
    isolate_->SetData(kHostContextSlot, host);
    isolate_->SetData(kRuntimeSlot, this);

    // 例外時に未処理を潰さないよう、明示的に MicrotasksPolicy を制御する
    isolate_->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);

    v8::Isolate::Scope isolate_scope(isolate_);
    v8::HandleScope handle_scope(isolate_);

    v8::Local<v8::Context> context = v8::Context::New(isolate_);
    context_.Reset(isolate_, context);

    v8::Context::Scope context_scope(context);

    // ブラウザ相当グローバルをインストール
    InstallGlobalBindings(isolate_, context->Global(), host_);

    return true;
}

void V8Runtime::Dispose()
{
    module_cache_.clear();
    module_paths_.clear();
    context_.Reset();

    if (isolate_) {
        isolate_->Dispose();
        isolate_ = nullptr;
    }
    if (create_params_.array_buffer_allocator) {
        delete create_params_.array_buffer_allocator;
        create_params_.array_buffer_allocator = nullptr;
    }
}

v8::Local<v8::Context> V8Runtime::context() const
{
    return context_.Get(isolate_);
}

bool V8Runtime::RunScript(const std::string& source, const std::string& name)
{
    v8::Isolate::Scope isolate_scope(isolate_);
    v8::HandleScope handle_scope(isolate_);
    v8::Local<v8::Context> ctx = context();
    v8::Context::Scope context_scope(ctx);

    v8::TryCatch try_catch(isolate_);

    v8::ScriptOrigin origin(v8util::Str(isolate_, name));
    v8::Local<v8::String> src = v8util::Str(isolate_, source);

    v8::Local<v8::Script> script;
    if (!v8::Script::Compile(ctx, src, &origin).ToLocal(&script)) {
        ReportException(&try_catch);
        return false;
    }

    v8::Local<v8::Value> result;
    if (!script->Run(ctx).ToLocal(&result)) {
        ReportException(&try_catch);
        return false;
    }

    PumpMicrotasks();
    return true;
}

v8::MaybeLocal<v8::Module> V8Runtime::LoadModule(const std::string& path,
                                                const std::string& /*referrer_dir*/)
{
    const std::string abs = fs::weakly_canonical(path).string();

    auto cached = module_cache_.find(abs);
    if (cached != module_cache_.end()) {
        return cached->second.Get(isolate_);
    }

    const std::string source = ReadTextFile(abs);
    if (source.empty() && !fs::exists(abs)) {
        v8util::ThrowTypeError(isolate_, "Module not found: " + abs);
        return v8::MaybeLocal<v8::Module>();
    }

    v8::Local<v8::String> src = v8util::Str(isolate_, source);

    v8::ScriptOrigin origin(
        v8util::Str(isolate_, abs),
        0, 0, false, -1, v8::Local<v8::Value>(),
        false, false, /*is_module*/ true
    );

    v8::ScriptCompiler::Source compiler_source(src, origin);
    v8::Local<v8::Module> module;
    if (!v8::ScriptCompiler::CompileModule(isolate_, &compiler_source).ToLocal(&module)) {
        return v8::MaybeLocal<v8::Module>();
    }

    module_cache_[abs].Reset(isolate_, module);
    module_paths_[module->GetIdentityHash()] = abs;

    // 依存モジュールを再帰的にロード
    v8::Local<v8::FixedArray> requests = module->GetModuleRequests();
    const std::string dir = fs::path(abs).parent_path().string();
    for (int i = 0; i < requests->Length(); ++i) {
        v8::Local<v8::ModuleRequest> request =
            requests->Get(context(), i).As<v8::ModuleRequest>();
        const std::string specifier =
            v8util::ToStdString(isolate_, request->GetSpecifier());
        const std::string resolved = (fs::path(dir) / specifier).string();
        if (LoadModule(resolved, dir).IsEmpty()) {
            return v8::MaybeLocal<v8::Module>();
        }
    }

    return module;
}

v8::MaybeLocal<v8::Module> V8Runtime::ResolveModuleCallback(
    v8::Local<v8::Context> context,
    v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> /*import_assertions*/,
    v8::Local<v8::Module> referrer)
{
    v8::Isolate* isolate = context->GetIsolate();
    auto* self = static_cast<V8Runtime*>(isolate->GetData(kRuntimeSlot));

    auto it = self->module_paths_.find(referrer->GetIdentityHash());
    const std::string referrer_dir = (it != self->module_paths_.end())
        ? fs::path(it->second).parent_path().string()
        : std::string();

    const std::string spec = v8util::ToStdString(isolate, specifier);
    const std::string resolved = (fs::path(referrer_dir) / spec).string();
    return self->LoadModule(resolved, referrer_dir);
}

bool V8Runtime::RunModule(const std::string& /*source*/, const std::string& path)
{
    v8::Isolate::Scope isolate_scope(isolate_);
    v8::HandleScope handle_scope(isolate_);
    v8::Local<v8::Context> ctx = context();
    v8::Context::Scope context_scope(ctx);

    v8::TryCatch try_catch(isolate_);

    v8::Local<v8::Module> module;
    if (!LoadModule(path, fs::path(path).parent_path().string()).ToLocal(&module)) {
        ReportException(&try_catch);
        return false;
    }

    if (module->InstantiateModule(ctx, ResolveModuleCallback).IsNothing()) {
        ReportException(&try_catch);
        return false;
    }

    v8::Local<v8::Value> result;
    if (!module->Evaluate(ctx).ToLocal(&result)) {
        ReportException(&try_catch);
        return false;
    }

    // トップレベル await の完了を待つためマイクロタスクを回す
    PumpMicrotasks();

    if (module->GetStatus() == v8::Module::kErrored) {
        v8::Local<v8::Value> exception = module->GetException();
        std::cerr << "[V8] Module evaluation error: "
                  << v8util::ToStdString(isolate_, exception) << std::endl;
        return false;
    }

    return true;
}

void V8Runtime::PumpMicrotasks()
{
    isolate_->PerformMicrotaskCheckpoint();
}

void V8Runtime::PumpPlatformTasks()
{
    while (v8::platform::PumpMessageLoop(platform_.get(), isolate_)) {
        // フォアグラウンドタスクを空になるまで処理
    }
}

void V8Runtime::ReportException(v8::TryCatch* try_catch)
{
    v8::HandleScope handle_scope(isolate_);
    const std::string exception = v8util::ToStdString(isolate_, try_catch->Exception());

    v8::Local<v8::Message> message = try_catch->Message();
    if (message.IsEmpty()) {
        std::cerr << "[V8] " << exception << std::endl;
        return;
    }

    v8::Local<v8::Context> ctx = context();
    const std::string filename =
        v8util::ToStdString(isolate_, message->GetScriptOrigin().ResourceName());
    const int linenum = message->GetLineNumber(ctx).FromMaybe(0);
    std::cerr << "[V8] " << filename << ":" << linenum << ": " << exception << std::endl;

    v8::Local<v8::Value> stack_trace;
    if (try_catch->StackTrace(ctx).ToLocal(&stack_trace)) {
        std::cerr << v8util::ToStdString(isolate_, stack_trace) << std::endl;
    }
}

} // namespace next2d
