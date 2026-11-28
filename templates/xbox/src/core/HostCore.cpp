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
#include "../platform/HostCache.h"
#include "../platform/GamepadManager.h"
#include "../platform/AudioEngine.h"
#include "../platform/StbTextRasterizer.h"
#include "../worker/WorkerRuntime.h"
#include "../bindings/Bindings.h"
#include "../bindings/EventTarget.h"
#include "../bindings/webgl/GlApi.h"
#include "../v8/V8Runtime.h"

#include <v8-profiler.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace next2d;

namespace {

// レンダラを専用スレッドで動かすための、GL コンテキストの受け渡し (next2d_host_set_render_thread)。
int32_t (*g_render_make_current)(void* user) = nullptr;
void (*g_render_release_current)(void* user) = nullptr;
void* g_render_user = nullptr;

// 埋め込み側のコールバックで提示する GlesContext。
class CallbackGlesContext final : public GlesContext {
public:
    CallbackGlesContext(const Next2DHostConfig& c, bool render_thread)
        : user_(c.user)
        , swap_(c.gl_swap_buffers)
        , make_current_(c.gl_make_current)
        , render_thread_(render_thread)
        , width_(c.width)
        , height_(c.height)
        , default_framebuffer_(c.default_framebuffer)
    {}

    bool MakeCurrent() override
    {
        if (render_thread_) {
            return g_render_make_current(g_render_user) != 0;
        }
        return make_current_ ? make_current_(user_) != 0 : true;
    }

    void ReleaseCurrent() override
    {
        if (render_thread_) {
            g_render_release_current(g_render_user);
        }
    }

    void SwapBuffers() override
    {
        if (swap_) {
            swap_(user_);
        }
        needs_present = false;
    }

    void Resize(int width, int height)
    {
        width_ = width;
        height_ = height;
    }

    int width() const override { return width_; }
    int height() const override { return height_; }
    uint32_t default_framebuffer() const override { return default_framebuffer_; }

private:
    void* user_;
    void (*swap_)(void*);
    int32_t (*make_current_)(void*);
    bool render_thread_;
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

// 負荷の判定 (next2d_host_set_load_callback): 1 フレームの処理がこの時間を超えたら「重い」とし、
// 重くないフレームがこの回数続いたら「落ち着いた」とする。
constexpr double kHeavyFrameMs = 40.0;
constexpr int kQuietFramesToSettle = 20;

// ---- CPU プロファイル (環境変数 NEXT2D_HOST_PROFILE) ------------------------
// V8 の sampling CPU プロファイラで、メインの JS が長くかかったフレームを含む区間の
// self 時間上位を標準エラー出力へ出す。jitless でも動く。画面遷移の引っかかりの原因調査用。
constexpr unsigned kProfileIntervalUs = 500;

std::string ProfileNodeKey(const v8::CpuProfileNode* node)
{
    const char* name = node->GetFunctionNameStr();
    const char* script = node->GetScriptResourceNameStr();
    std::string key = (name && *name) ? name : "(anonymous)";
    if (script && *script) {
        std::string s(script);
        if (s.rfind("data:", 0) == 0 || s.rfind("blob:", 0) == 0) {
            // data URL / blob URL から起動した worker のスクリプト (URL 全体は長すぎるので出さない)
            s = "worker";
        } else {
            const auto slash = s.find_last_of("/\\");
            if (slash != std::string::npos) {
                s = s.substr(slash + 1);
            }
        }
        key += " (" + s + ":" + std::to_string(node->GetLineNumber())
             + ":" + std::to_string(node->GetColumnNumber()) + ")";
    }
    return key;
}

void PrintProfileTop(const char* title, const std::map<std::string, unsigned>& hits, unsigned total, int limit)
{
    std::vector<std::pair<std::string, unsigned>> sorted(hits.begin(), hits.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& l, const auto& r) { return l.second > r.second; });
    std::cerr << "[cpuprofile]  " << title << ":" << std::endl;
    int rank = 0;
    for (const auto& [key, count] : sorted) {
        if (count == 0 || ++rank > limit) {
            break;
        }
        char line[512];
        std::snprintf(line, sizeof(line), "  %5.1f%%  %7.1fms  %s",
            total ? 100.0 * count / total : 0.0,
            count * (kProfileIntervalUs / 1000.0), key.c_str());
        std::cerr << line << std::endl;
    }
}

// 直近 window_ms の間に採ったサンプルだけを集計して出す (時間のかかったフレームの中身だけを見るため)。
//   self      … その関数自身で使った時間
//   inclusive … その関数と、そこから呼ばれた関数で使った時間 (どの処理の配下が重いかを見る)
void DumpAndRestartProfile(v8::CpuProfiler* profiler, v8::Isolate* isolate, const char* reason,
                           double window_ms)
{
    v8::HandleScope hs(isolate);
    v8::CpuProfile* profile = profiler->StopProfiling(
        v8::String::NewFromUtf8Literal(isolate, "next2d"));
    if (profile) {
        const int64_t from = profile->GetEndTime() - static_cast<int64_t>(window_ms * 1000.0);
        std::map<std::string, unsigned> self_hits;
        std::map<std::string, unsigned> inclusive_hits;
        unsigned total = 0;
        std::vector<std::string> seen;
        for (int i = 0; i < profile->GetSamplesCount(); ++i) {
            if (profile->GetSampleTimestamp(i) < from) {
                continue;
            }
            const v8::CpuProfileNode* node = profile->GetSample(i);
            if (!node) {
                continue;
            }
            ++total;
            ++self_hits[ProfileNodeKey(node)];
            // 再帰で同じ関数が何度も積まれていても、1 サンプルにつき 1 回だけ数える
            seen.clear();
            for (const v8::CpuProfileNode* n = node; n; n = n->GetParent()) {
                std::string key = ProfileNodeKey(n);
                if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
                    ++inclusive_hits[key];
                    seen.push_back(std::move(key));
                }
            }
        }
        std::cerr << "[cpuprofile] " << reason << ": " << total << " samples in the last "
                  << static_cast<int>(window_ms) << " ms (interval " << kProfileIntervalUs << "us)" << std::endl;
        PrintProfileTop("self", self_hits, total, 25);
        PrintProfileTop("inclusive", inclusive_hits, total, 45);
        profile->Delete();
    }
    profiler->StartProfiling(v8::String::NewFromUtf8Literal(isolate, "next2d"), true);
}

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
    v8::CpuProfiler* cpu_profiler = nullptr;   // NEXT2D_HOST_PROFILE 指定時のみ
    bool running = true;

    // 負荷の状態 (next2d_host_set_load_callback)。起動直後は負荷が高い状態から始める。
    bool loading = true;
    int quiet_frames = 0;
    void (*load_callback)(int32_t loading, void* user) = nullptr;
    void* load_callback_user = nullptr;

    // レンダラ (worker) を専用スレッドで動かしているか (next2d_host_set_render_thread)
    bool render_thread = false;
    // レンダラのスレッドが 1 回の描画にかかった時間の最大値 (HostContext::worker_turn_ms)
    std::atomic<int> worker_turn_ms{0};
    // レンダラのスレッドが描画を始めた時刻 (HostContext::worker_turn_started_ms)
    std::atomic<long long> worker_turn_started_ms{0};

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
    if (h->cpu_profiler) {
        h->cpu_profiler->Dispose();
        h->cpu_profiler = nullptr;
    }
    // v8::Global を保持するものは Isolate 破棄前に明示解放する。
    // worker を先に止める: 専用スレッドの worker は、自分の isolate に属するものを自分のスレッドで
    // 手放してから終わる (レンダラのスレッドは GL のコンテキストも手放す)。
    if (h->workers) {
        h->workers->Shutdown();
    }
    decodequeue::Shutdown();
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
    // ビューポート (innerWidth / innerHeight、CSS ピクセル) はフレームバッファのピクセル数を
    // devicePixelRatio で割った値。ブラウザと同じ関係で、アプリは CSS ピクセルで配置を決め、
    // 描画バッファだけを devicePixelRatio 倍の解像度で持つ。
    const double dpr = config->device_pixel_ratio > 0.0 ? config->device_pixel_ratio : 1.0;
    h->host.device_pixel_ratio = dpr;
    h->host.viewport_width = std::max(1, static_cast<int>(std::lround(config->width / dpr)));
    h->host.viewport_height = std::max(1, static_cast<int>(std::lround(config->height / dpr)));
    h->host.backend = GraphicsBackend::WebGL2;
    if (config->storage_dir) {
        h->host.storage_dir = config->storage_dir;
    }
    h->host.storage_saved = config->storage_saved;
    h->host.storage_saved_user = config->user;
    // レンダラを専用スレッドで動かす構成 (next2d_host_set_render_thread)。
    // 環境変数 NEXT2D_HOST_NO_RENDER_THREAD を設定すると使わない (比較と切り分けのため)。
    h->render_thread = g_render_make_current && g_render_release_current
        && std::getenv("NEXT2D_HOST_NO_RENDER_THREAD") == nullptr;
    h->host.threaded_workers = h->render_thread;
    h->host.worker_turn_ms = &h->worker_turn_ms;
    h->host.worker_turn_started_ms = &h->worker_turn_started_ms;
    h->gles = std::make_unique<CallbackGlesContext>(*config, h->render_thread);
    h->host.gles = h->gles.get();
    if (h->render_thread) {
        // GL の関数は解決し終えた。以後メインのスレッドは GL を呼ばないので、コンテキストを
        // 手放す。レンダラのスレッドが、WebGL2 のコンテキストを作るときに自分のものにする。
        g_render_release_current(g_render_user);
        std::cerr << "[HostCore] render thread: on" << std::endl;
    }

    if (!g_process_initialized) {
        if (h->render_thread) {
            // isolate を複数持つ構成では、若い世代の GC を並列に行う機能 (parallel scavenge) を止める。
            // 有効のままだと、worker の isolate で GC の管理情報 (WeakMap の書き込み記録) が壊れて
            // 異常終了することがある (PC 環境で 6〜7 回に 1 回ほど再現し、止めると再現しなくなった。
            // GC の他の並列処理だけを止めても再現した)。若い世代の GC は生き残ったオブジェクトの量に
            // 比例する短い処理なので、1 スレッドで行っても停止時間への影響は小さい。
            v8::V8::SetFlagsFromString("--no-parallel-scavenge");
        }
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

    if (std::getenv("NEXT2D_HOST_PROFILE") != nullptr) {
        v8::Isolate::Scope iso_scope(h->runtime->isolate());
        v8::HandleScope hs(h->runtime->isolate());
        h->cpu_profiler = v8::CpuProfiler::New(h->runtime->isolate());
        h->cpu_profiler->SetSamplingInterval(kProfileIntervalUs);
        h->cpu_profiler->StartProfiling(
            v8::String::NewFromUtf8Literal(h->runtime->isolate(), "next2d"), true);
        std::cerr << "[cpuprofile] profiling started" << std::endl;
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

        // 作り直せるデータ (シェーダのリンク結果など) を保存先へ書いたら、埋め込み側へ知らせる
        // (書いた内容を確定する操作が必要な保存先のため)。名前は "cache"。
        if (hostcache::TakeWritten() && h->host.storage_saved) {
            h->host.storage_saved("cache", h->host.storage_saved_user);
        }

        h->gamepad->Poll(now);
        PumpAudioEvents(runtime.isolate());
        decodequeue::Pump();

        // レンダラが別のスレッドで動いているときは、その返事 (描画の完了など) をフレームの最初に
        // 受け取る。メインは返事を受けてから次の描画を送るので、rAF より後で受け取ると、
        // レンダラが間に合っていても 1 フレームおきにしか描画を送れない。
        if (h->render_thread) {
            h->workers->Pump(now);
            runtime.PumpMicrotasks();
        }

        event_loop.PumpTimers();
        runtime.PumpMicrotasks();
        const double t_tasks = event_loop.Now();

        event_loop.RunAnimationFrame(now);
        runtime.PumpMicrotasks();
        const double t_js = event_loop.Now();

        // レンダラ worker がここで WebGL2 コマンドを発行する
        // (専用スレッドの構成では、worker からのメッセージを受け取るだけ)
        h->workers->Pump(now);
        const double t_worker = event_loop.Now();

        runtime.PumpPlatformTasks();

        // プロファイル中: メインの JS が長くかかったフレームを含む区間を書き出して仕切り直す
        // (タイマー側やレンダラ側が長くかかったフレームも対象。しきい値は NEXT2D_HOST_PROFILE に
        //  ミリ秒で指定でき、数値でなければ 300)
        static const double profile_threshold = [] {
            const char* value = std::getenv("NEXT2D_HOST_PROFILE");
            const double ms = value ? std::atof(value) : 0.0;
            return ms > 0.0 ? ms : 300.0;
        }();
        const double longest = std::max({ t_tasks - now, t_js - t_tasks, t_worker - t_js });
        if (h->cpu_profiler && longest > profile_threshold) {
            char reason[96];
            std::snprintf(reason, sizeof(reason), "spike (tasks %.0f ms, js %.0f ms, worker %.0f ms)",
                          t_tasks - now, t_js - t_tasks, t_worker - t_js);
            DumpAndRestartProfile(h->cpu_profiler, runtime.isolate(), reason, t_worker - now + 1.0);
        }

        // 環境変数 NEXT2D_HOST_PERF を設定すると、時間のかかったフレームの内訳を出す
        // (tasks = タイマー/デコード完了/マイクロタスク、js = メインの rAF、worker = レンダラ)。
        static const bool perf = std::getenv("NEXT2D_HOST_PERF") != nullptr;
        if (perf) {
            // worker の時間のうち、WebGL2 のメソッド (GL の呼び出し) にかかった分。残りはレンダラの JS
            // (専用スレッドの構成では、レンダラのスレッドが自分で内訳を出す)
            const std::string gl = h->render_thread ? std::string("render thread") : TakeWebGL2PerfSummary();
            if (t_worker - now > 50.0) {
                std::cerr << "[HostCore] slow frame: tasks " << static_cast<int>(t_tasks - now)
                          << " ms, js " << static_cast<int>(t_js - t_tasks)
                          << " ms, worker " << static_cast<int>(t_worker - t_js) << " ms (" << gl << ")"
                          << std::endl;
            }

            // 2 秒ごとに、フレームの開始時刻 (rAF に渡す時刻) の間隔を出す。
            // アプリ側は「前回の進行から 16 ms を超えたら 1 コマ進める」という作りが多いので、間隔が
            // 揃っていない (短い間隔と長い間隔が交互に来る) と、フレームの数は足りていてもコマが抜ける。
            // ticks は、その規則 (16 ms を超えたら進め、余りを次へ持ち越す) で進むコマの数。
            static double stat_started = now;
            static double previous = now;
            static double tick_start = now;
            static int frames = 0;
            static int ticks = 0;
            static int shorter = 0;
            static int longer = 0;
            static double min_ms = 1e9;
            static double max_ms = 0.0;
            const double interval = now - previous;
            previous = now;
            ++frames;
            min_ms = std::min(min_ms, interval);
            max_ms = std::max(max_ms, interval);
            if (interval < 14.0) {
                ++shorter;
            } else if (interval > 19.5) {
                ++longer;
            }
            const double since_tick = now - tick_start;
            if (since_tick > 16.0) {
                tick_start = now - std::fmod(since_tick, 16.0);
                ++ticks;
            }
            if (now - stat_started >= 2000.0) {
                char line[200];
                std::snprintf(line, sizeof(line),
                    "[HostCore] %d frames / 2 s: interval min %.1f ms, max %.1f ms, under 14 ms: %d, "
                    "over 19.5 ms: %d; ticks %d",
                    frames, min_ms, max_ms, shorter, longer, ticks);
                std::cerr << line << std::endl;
                stat_started = now;
                frames = 0;
                ticks = 0;
                shorter = 0;
                longer = 0;
                min_ms = 1e9;
                max_ms = 0.0;
            }
        }

        // 負荷の状態を埋め込み側へ知らせる (next2d_host_set_load_callback)。
        // 区間は、画面の資材 (画像) の読み込みが始まったときに入る。読み込みの開始は画面の切り替えの
        // 前触れなので、その後に来る重いフレーム (画面の組み立て、初回の描画) も同じ区間に入る。
        // 区間の中では、重いフレームが出ている間は延長し、軽いフレームが続いたら抜ける。
        // 重いフレームだけでは区間に入らない (通常の進行中に、性能の切り替えを繰り返さないため)。
        const bool scene_loading = decodequeue::SceneLoads() > 0;
        // レンダラが別のスレッドのときは、そちらの 1 回の描画にかかった時間も見る
        // (終わった描画の時間と、いま進行中の描画が始まってからの時間の長い方)
        double render_ms = 0.0;
        if (h->render_thread) {
            render_ms = h->worker_turn_ms.exchange(0);
            const long long started = h->worker_turn_started_ms.load();
            if (started != 0) {
                const long long current = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                render_ms = std::max(render_ms, static_cast<double>(current - started));
            }
        }
        const bool heavy_frame = std::max(t_worker - now, render_ms) > kHeavyFrameMs;
        if (scene_loading || (h->loading && heavy_frame)) {
            h->quiet_frames = 0;
        } else if (h->quiet_frames < kQuietFramesToSettle) {
            ++h->quiet_frames;
        }
        const bool loading = h->quiet_frames < kQuietFramesToSettle;
        if (loading != h->loading) {
            h->loading = loading;
            if (h->load_callback) {
                h->load_callback(loading ? 1 : 0, h->load_callback_user);
            }
        }
    }

    // デフォルトフレームバッファへ描いたフレームだけ提示する
    // (描いていないフレームで swap すると未定義内容が提示される)。
    // (専用スレッドの構成では、レンダラのスレッドが描いた直後に提示する)
    if (!h->render_thread && h->gles->needs_present) {
        CaptureWebGL2Frame(h->gles.get());
        h->gles->SwapBuffers();
    }

    // フレームペーシング: ブラウザの rAF 同様 ~60Hz に揃える
    // (提示しないフレームや VSync 無しの環境で空回りしないように)。
    // 環境変数 NEXT2D_HOST_NO_PACING を設定すると、ここでは待たない。埋め込み側が画面の更新
    // (垂直同期) に合わせて next2d_host_frame を呼ぶ場合に使う。ここでの待ちは画面の更新とは
    // 無関係な一定間隔なので、垂直同期のある環境で併用すると、フレームの間隔が画面の更新と
    // 少しずつずれて、動きが周期的に引っかかって見える。
    static const bool pacing = std::getenv("NEXT2D_HOST_NO_PACING") == nullptr;
    const double frame_ms = event_loop.Now() - now;
    if (pacing && frame_ms < 15.0) {
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
    // 入力はフレームバッファのピクセル座標で受け取り、DOM イベントの CSS ピクセルへ直す
    x /= h->host.device_pixel_ratio;
    y /= h->host.device_pixel_ratio;
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

void next2d_host_resize(Next2DHost* h, int32_t width, int32_t height)
{
    if (!h || width <= 0 || height <= 0) {
        return;
    }
    const double dpr = h->host.device_pixel_ratio;
    const int vw = std::max(1, static_cast<int>(std::lround(width / dpr)));
    const int vh = std::max(1, static_cast<int>(std::lround(height / dpr)));
    h->gles->Resize(width, height);
    if (vw == h->host.viewport_width && vh == h->host.viewport_height) {
        return;
    }
    h->host.viewport_width = vw;
    h->host.viewport_height = vh;

    // ブラウザのウィンドウサイズ変更と同じ: innerWidth などを更新して resize を配送する。
    // アプリ (player) は resize を受けて、新しいサイズで描画バッファを作り直す。
    {
        v8::Isolate* isolate = h->runtime->isolate();
        v8::Isolate::Scope isolate_scope(isolate);
        v8::HandleScope hs(isolate);
        v8::Local<v8::Context> ctx = h->runtime->context();
        v8::Context::Scope cs(ctx);
        v8::Local<v8::Object> global = ctx->Global();
        SetNum(isolate, global, "innerWidth", vw);
        SetNum(isolate, global, "innerHeight", vh);
        v8::Local<v8::Value> screen;
        if (global->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "screen")).ToLocal(&screen) &&
            screen->IsObject()) {
            SetNum(isolate, screen.As<v8::Object>(), "width", vw);
            SetNum(isolate, screen.As<v8::Object>(), "height", vh);
        }
        if (!h->host.main_canvas.IsEmpty()) {
            v8::Local<v8::Object> canvas = h->host.main_canvas.Get(isolate);
            SetNum(isolate, canvas, "clientWidth", vw);
            SetNum(isolate, canvas, "clientHeight", vh);
        }
    }
    h->FireEvent("resize", [](v8::Isolate*, v8::Local<v8::Object>) {});
}

int32_t next2d_host_render_audio(Next2DHost* h, float* out, int32_t frames,
                                 int32_t channels, int32_t sample_rate)
{
    if (!h || !h->audio) {
        return 0;
    }
    return h->audio->Render(out, frames, channels, sample_rate) ? 1 : 0;
}

int32_t next2d_host_register_font(const char* name, const uint8_t* data, size_t size)
{
    if (!name || !data || size == 0) {
        return 0;
    }
    return stbtext::RegisterFont(name, std::vector<uint8_t>(data, data + size)) ? 1 : 0;
}

void next2d_host_set_load_callback(Next2DHost* h, void (*callback)(int32_t loading, void* user), void* user)
{
    if (!h) {
        return;
    }
    h->load_callback = callback;
    h->load_callback_user = user;
    if (callback) {
        callback(h->loading ? 1 : 0, user);   // 現在の状態を最初に知らせる
    }
}

void next2d_host_set_render_thread(int32_t (*make_current)(void* user),
                                   void (*release_current)(void* user), void* user)
{
    g_render_make_current = make_current;
    g_render_release_current = release_current;
    g_render_user = user;
}

void next2d_host_set_cache_dir(const char* directory)
{
    hostcache::SetDirectory(directory ? directory : "");
}

void next2d_host_set_language(const char* language)
{
    SetNavigatorLanguage(language ? language : "");
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
