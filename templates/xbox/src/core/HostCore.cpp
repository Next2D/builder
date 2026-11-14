// HostCore: ホスト共通コアの C 窓口 (next2d_host_core.h) の実装。
//
// main.cpp (Win32 + Dawn の実行ファイル) が行っている起動・フレーム進行・後始末を、
// ウィンドウや GL コンテキストの生成を含まない形でまとめ直したもの。描画は WebGL2
// バインディング (関数テーブルモード) で、GL 関数の解決と提示は埋め込み側のコールバック。
#include "next2d_host_core.h"

#include "../HostContext.h"
#include "../AssetLoader.h"
#include "../EmbeddedAssets.h"
#include "../EventLoop.h"
#include "../gpu/GlesContext.h"
#include "../platform/DecodeQueue.h"
#include "../platform/GamepadManager.h"
#include "../platform/AudioEngine.h"
#include "../platform/StbTextRasterizer.h"
#include "../worker/WorkerRuntime.h"
#include "../bindings/Bindings.h"
#include "../bindings/EventTarget.h"
#include "../bindings/webgl/GlApi.h"
#include "../v8/V8Runtime.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace next2d;

namespace {

// 埋め込み側のコールバックで提示する GlesContext。
class CallbackGlesContext final : public GlesContext {
public:
    explicit CallbackGlesContext(const Next2DHostConfig& c)
        : user_(c.user)
        , swap_(c.gl_swap_buffers)
        , make_current_(c.gl_make_current)
        , width_(c.width)
        , height_(c.height)
        , default_framebuffer_(c.default_framebuffer)
    {}

    bool MakeCurrent() override
    {
        return make_current_ ? make_current_(user_) != 0 : true;
    }

    void SwapBuffers() override
    {
        if (swap_) {
            swap_(user_);
        }
        needs_present = false;
    }

    int width() const override { return width_; }
    int height() const override { return height_; }
    uint32_t default_framebuffer() const override { return default_framebuffer_; }

private:
    void* user_;
    void (*swap_)(void*);
    int32_t (*make_current_)(void*);
    int width_;
    int height_;
    uint32_t default_framebuffer_;
};

std::string ReadFile(const fs::path& path)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return {};
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

void RegisterFontsFrom(const fs::path& dir)
{
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const std::string ext = entry.path().extension().string();
        if (ext != ".ttf" && ext != ".otf" && ext != ".TTF" && ext != ".OTF") {
            continue;
        }
        std::ifstream ifs(entry.path(), std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(ifs)),
                                   std::istreambuf_iterator<char>());
        const std::string name = entry.path().stem().string();
        if (stbtext::RegisterFont(name, std::move(bytes))) {
            std::cerr << "[Font] registered: " << name << std::endl;
        }
    }
}

bool g_process_initialized = false;

} // namespace

struct Next2DHost {
    HostContext host;
    std::unique_ptr<CallbackGlesContext> gles;
    std::unique_ptr<V8Runtime> runtime;
    std::unique_ptr<EventLoop> event_loop;
    std::unique_ptr<AssetLoader> assets;
    std::unique_ptr<GamepadManager> gamepad;
    std::unique_ptr<AudioEngine> audio;
    std::unique_ptr<WorkerRuntime> workers;
    bool running = true;

    // window(global) / document / 主要 canvas へイベントを配送する (main.cpp と同じ規則)。
    void FireEvent(const char* type,
                   const std::function<void(v8::Isolate*, v8::Local<v8::Object>)>& fill)
    {
        v8::Isolate* isolate = runtime->isolate();
        v8::Isolate::Scope isolate_scope(isolate);
        v8::HandleScope hs(isolate);
        v8::Local<v8::Context> ctx = runtime->context();
        v8::Context::Scope cs(ctx);

        v8::Local<v8::Object> event = v8::Object::New(isolate);
        event->Set(ctx, v8::String::NewFromUtf8(isolate, "type").ToLocalChecked(),
                   v8::String::NewFromUtf8(isolate, type).ToLocalChecked()).Check();
        auto noop = [](const v8::FunctionCallbackInfo<v8::Value>&) {};
        event->Set(ctx, v8::String::NewFromUtf8(isolate, "preventDefault").ToLocalChecked(),
                   v8::Function::New(ctx, noop).ToLocalChecked()).Check();
        event->Set(ctx, v8::String::NewFromUtf8(isolate, "stopPropagation").ToLocalChecked(),
                   v8::Function::New(ctx, noop).ToLocalChecked()).Check();
        fill(isolate, event);

        v8::Local<v8::Object> global = ctx->Global();
        DispatchEvent(isolate, global, event);
        v8::Local<v8::Value> doc;
        if (global->Get(ctx, v8::String::NewFromUtf8(isolate, "document").ToLocalChecked()).ToLocal(&doc) &&
            doc->IsObject()) {
            DispatchEvent(isolate, doc.As<v8::Object>(), event);
        }
        if (!host.main_canvas.IsEmpty()) {
            DispatchEvent(isolate, host.main_canvas.Get(isolate), event);
        }
    }
};

namespace {

void SetNum(v8::Isolate* iso, v8::Local<v8::Object> o, const char* k, double v)
{
    o->Set(iso->GetCurrentContext(), v8::String::NewFromUtf8(iso, k).ToLocalChecked(),
           v8::Number::New(iso, v)).Check();
}

void SetBool(v8::Isolate* iso, v8::Local<v8::Object> o, const char* k, bool v)
{
    o->Set(iso->GetCurrentContext(), v8::String::NewFromUtf8(iso, k).ToLocalChecked(),
           v8::Boolean::New(iso, v)).Check();
}

void SetStr(v8::Isolate* iso, v8::Local<v8::Object> o, const char* k, const char* v)
{
    o->Set(iso->GetCurrentContext(), v8::String::NewFromUtf8(iso, k).ToLocalChecked(),
           v8::String::NewFromUtf8(iso, v).ToLocalChecked()).Check();
}

void DestroyHost(Next2DHost* h)
{
    // v8::Global を保持するものは Isolate 破棄前に明示解放する (main.cpp と同じ順序)。
    decodequeue::Shutdown();
    if (h->workers) {
        h->workers->Shutdown();
    }
    if (h->event_loop) {
        h->event_loop->Shutdown();
    }
    ShutdownAudioEvents();
    h->host.main_canvas.Reset();
    if (h->runtime) {
        h->runtime->Dispose();
    }
    delete h;
}

} // namespace

extern "C" {

Next2DHost* next2d_host_create(const Next2DHostConfig* config)
{
    if (!config || config->api_version != NEXT2D_HOST_CORE_API_VERSION) {
        std::cerr << "[HostCore] api_version mismatch" << std::endl;
        return nullptr;
    }
    if (!config->gl_get_proc_address || !config->gl_swap_buffers ||
        !config->assets_dir || !config->host_script_dir ||
        config->width <= 0 || config->height <= 0) {
        std::cerr << "[HostCore] invalid config" << std::endl;
        return nullptr;
    }

    // GL 関数テーブルを埋める (コンテキストは埋め込み側が current にしてある前提)。
    const char* first_missing = nullptr;
    const int missing = glapi::Load(config->gl_get_proc_address, config->user, &first_missing);
    if (missing != 0) {
        std::cerr << "[HostCore] " << missing << " GL function(s) unresolved, first: "
                  << (first_missing ? first_missing : "?") << std::endl;
        return nullptr;
    }

    const fs::path assets_app = fs::u8path(config->assets_dir);
    const fs::path bootstrap_js = fs::u8path(config->host_script_dir) / "bootstrap.js";

    if (config->fonts_dir) {
        RegisterFontsFrom(fs::u8path(config->fonts_dir));
    }
    // 資材内のフォント (assets/app 直下と fonts/)
    RegisterFontsFrom(assets_app);
    RegisterFontsFrom(assets_app / "fonts");

    auto* h = new Next2DHost();
    h->host.viewport_width = config->width;
    h->host.viewport_height = config->height;
    h->host.device_pixel_ratio = config->device_pixel_ratio > 0.0 ? config->device_pixel_ratio : 1.0;
    h->host.backend = GraphicsBackend::WebGL2;
    h->gles = std::make_unique<CallbackGlesContext>(*config);
    h->host.gles = h->gles.get();

    if (!g_process_initialized) {
        V8Runtime::InitializeProcess(nullptr);
        g_process_initialized = true;
    }
    h->runtime = std::make_unique<V8Runtime>();
    if (!h->runtime->Initialize(&h->host)) {
        std::cerr << "[HostCore] V8 init failed" << std::endl;
        DestroyHost(h);
        return nullptr;
    }

    h->event_loop = std::make_unique<EventLoop>(h->runtime->isolate());
    h->assets = std::make_unique<AssetLoader>(assets_app.string());
    h->gamepad = std::make_unique<GamepadManager>();
    h->audio = std::make_unique<AudioEngine>();
    h->workers = std::make_unique<WorkerRuntime>(h->runtime->isolate(), &h->host);

    h->host.event_loop = h->event_loop.get();
    h->host.assets = h->assets.get();
    h->host.gamepad = h->gamepad.get();
    h->host.audio = h->audio.get();
    h->host.workers = h->workers.get();

    h->gamepad->Initialize();
    if (!h->audio->Initialize()) {
        std::cerr << "[HostCore] warning: audio initialization failed" << std::endl;
    }

    {
        v8::Isolate::Scope iso_scope(h->runtime->isolate());
        v8::HandleScope hs(h->runtime->isolate());
        v8::Local<v8::Context> ctx = h->runtime->context();
        v8::Context::Scope cs(ctx);
        ctx->SetAlignedPointerInEmbedderData(kEventLoopEmbedderSlot, h->event_loop.get());
        InstallWorker(h->runtime->isolate(), ctx->Global(), &h->host);
    }

    const std::string boot = ReadFile(bootstrap_js);
    if (boot.empty()) {
        std::cerr << "[HostCore] bootstrap.js not found: " << bootstrap_js.string() << std::endl;
    } else if (!h->runtime->RunScript(boot, "js/bootstrap.js")) {
        std::cerr << "[HostCore] bootstrap.js execution failed" << std::endl;
    }

    auto entry = h->assets->ResolveEntryModule();
    if (!entry) {
        std::cerr << "[HostCore] application entry module not found under "
                  << assets_app.string() << std::endl;
        DestroyHost(h);
        return nullptr;
    }
    if (!h->runtime->RunModule("", *entry)) {
        std::cerr << "[HostCore] application module execution failed" << std::endl;
    }
    return h;
}

int32_t next2d_host_frame(Next2DHost* h)
{
    if (!h || !h->running) {
        return 0;
    }
    V8Runtime& runtime = *h->runtime;
    EventLoop& event_loop = *h->event_loop;

    const double now = event_loop.Now();
    {
        v8::Isolate::Scope isolate_scope(runtime.isolate());
        v8::HandleScope handle_scope(runtime.isolate());
        v8::Local<v8::Context> ctx = runtime.context();
        v8::Context::Scope context_scope(ctx);

        h->gamepad->Poll(now);
        PumpAudioEvents(runtime.isolate());
        decodequeue::Pump();

        event_loop.PumpTimers();
        runtime.PumpMicrotasks();

        event_loop.RunAnimationFrame(now);
        runtime.PumpMicrotasks();

        // レンダラ worker がここで WebGL2 コマンドを発行する
        h->workers->Pump(now);

        runtime.PumpPlatformTasks();
    }

    // デフォルトフレームバッファへ描いたフレームだけ提示する
    // (描いていないフレームで swap すると未定義内容が提示される)。
    if (h->gles->needs_present) {
        h->gles->SwapBuffers();
    }

    // フレームペーシング: ブラウザの rAF 同様 ~60Hz に揃える
    // (提示しないフレームや VSync 無しの環境で空回りしないように)。
    const double frame_ms = event_loop.Now() - now;
    if (frame_ms < 15.0) {
        std::this_thread::sleep_for(
            std::chrono::microseconds(static_cast<long long>((15.0 - frame_ms) * 1000.0)));
    }
    return h->running ? 1 : 0;
}

void next2d_host_pointer_event(Next2DHost* h, int32_t kind, double x, double y,
                               int32_t button, int32_t pointer_id, const char* pointer_type)
{
    if (!h) {
        return;
    }
    const char* type = kind == NEXT2D_POINTER_DOWN ? "pointerdown"
                     : kind == NEXT2D_POINTER_UP   ? "pointerup" : "pointermove";
    const std::string ptype = pointer_type ? pointer_type : "mouse";
    h->FireEvent(type, [=](v8::Isolate* iso, v8::Local<v8::Object> e) {
        SetNum(iso, e, "clientX", x); SetNum(iso, e, "clientY", y);
        SetNum(iso, e, "offsetX", x); SetNum(iso, e, "offsetY", y);
        SetNum(iso, e, "pageX", x);   SetNum(iso, e, "pageY", y);
        SetNum(iso, e, "button", button);
        SetNum(iso, e, "pointerId", pointer_id);
        SetStr(iso, e, "pointerType", ptype.c_str());
    });
}

void next2d_host_key_event(Next2DHost* h, int32_t down, int32_t key_code,
                           const char* key, int32_t repeat)
{
    if (!h) {
        return;
    }
    const std::string k = key ? key : "";
    h->FireEvent(down ? "keydown" : "keyup", [=](v8::Isolate* iso, v8::Local<v8::Object> e) {
        SetNum(iso, e, "keyCode", key_code);
        SetNum(iso, e, "which", key_code);
        SetBool(iso, e, "repeat", repeat != 0);
        SetBool(iso, e, "shiftKey", false);
        SetBool(iso, e, "ctrlKey", false);
        SetBool(iso, e, "altKey", false);
        SetStr(iso, e, "key", k.c_str());
    });
}

void next2d_host_set_gamepad(Next2DHost* h, int32_t index, const Next2DGamepadState* state)
{
    if (!h || !state) {
        return;
    }
    GamepadSnapshot snap;
    snap.connected = state->connected != 0;
    snap.timestamp = h->event_loop->Now();
    for (int i = 0; i < 4; ++i) {
        snap.axes[i] = state->axes[i];
    }
    for (int i = 0; i < 17; ++i) {
        snap.buttons[i] = state->buttons[i];
        snap.pressed[i] = state->buttons[i] > 0.5f;
    }
    h->gamepad->SetSnapshot(index, snap);
}

void next2d_host_destroy(Next2DHost* h)
{
    if (!h) {
        return;
    }
    DestroyHost(h);
    // V8 はプロセスで 1 度しか初期化できないため、ShutdownProcess は呼ばない
    // (コアを作り直す埋め込み側のために初期化済みのまま残す)。
}

} // extern "C"
