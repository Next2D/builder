// Next2D Xbox (GDK native) host エントリポイント。
//
// GDK ランタイム初期化 -> ウィンドウ生成 -> V8/Dawn/入力/音声のブートストラップ ->
// bootstrap.js とアプリ本体(ESM)の読み込み -> ゲームループ、という流れ。
#include <Windows.h>
#include <windowsx.h>
#include <XGameRuntime.h>

#include "HostContext.h"
#include "AssetLoader.h"
#include "EventLoop.h"
#include "gpu/DawnContext.h"
#include "platform/GamepadManager.h"
#include "platform/AudioEngine.h"
#include "worker/WorkerRuntime.h"
#include "bindings/Bindings.h"
#include "bindings/EventTarget.h"
#include "v8/V8Runtime.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace next2d;

namespace {

bool g_running = true;
DawnContext* g_dawn = nullptr;
HostContext* g_host = nullptr;
V8Runtime* g_runtime = nullptr;

// window(global) / document / 主要 canvas へイベントを配送する。
// window へのキーボード、canvas へのポインタ/ホイールは player の登録先に合わせている。
void FireEvent(const char* type, const std::function<void(v8::Isolate*, v8::Local<v8::Object>)>& fill)
{
    if (!g_runtime || !g_host) {
        return;
    }
    v8::Isolate* isolate = g_runtime->isolate();
    v8::HandleScope hs(isolate);
    v8::Local<v8::Context> ctx = g_runtime->context();
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

    // window(global) と document、主要 canvas へ配送
    v8::Local<v8::Object> global = ctx->Global();
    DispatchEvent(isolate, global, event);
    v8::Local<v8::Value> doc;
    if (global->Get(ctx, v8::String::NewFromUtf8(isolate, "document").ToLocalChecked()).ToLocal(&doc) &&
        doc->IsObject()) {
        DispatchEvent(isolate, doc.As<v8::Object>(), event);
    }
    if (!g_host->main_canvas.IsEmpty()) {
        DispatchEvent(isolate, g_host->main_canvas.Get(isolate), event);
    }
}

static void SetNum(v8::Isolate* iso, v8::Local<v8::Object> o, const char* k, double v)
{
    v8::Local<v8::Context> c = iso->GetCurrentContext();
    o->Set(c, v8::String::NewFromUtf8(iso, k).ToLocalChecked(), v8::Number::New(iso, v)).Check();
}
static void SetBoolProp(v8::Isolate* iso, v8::Local<v8::Object> o, const char* k, bool v)
{
    v8::Local<v8::Context> c = iso->GetCurrentContext();
    o->Set(c, v8::String::NewFromUtf8(iso, k).ToLocalChecked(), v8::Boolean::New(iso, v)).Check();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
        case WM_CLOSE:
        case WM_DESTROY:
            g_running = false;
            PostQuitMessage(0);
            return 0;

        // ---- ポインタ (マウス) ----
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        case WM_LBUTTONUP:   case WM_RBUTTONUP:   case WM_MBUTTONUP:
        case WM_MOUSEMOVE: {
            const double px = GET_X_LPARAM(lparam);
            const double py = GET_Y_LPARAM(lparam);
            const int button = (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 2
                             : (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP) ? 1 : 0;
            const char* type =
                (msg == WM_MOUSEMOVE) ? "pointermove" :
                (msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP) ? "pointerup" : "pointerdown";
            FireEvent(type, [=](v8::Isolate* iso, v8::Local<v8::Object> e) {
                SetNum(iso, e, "clientX", px); SetNum(iso, e, "clientY", py);
                SetNum(iso, e, "offsetX", px); SetNum(iso, e, "offsetY", py);
                SetNum(iso, e, "pageX", px);   SetNum(iso, e, "pageY", py);
                SetNum(iso, e, "button", button);
                SetNum(iso, e, "pointerId", 1);
                v8::Local<v8::Context> c = iso->GetCurrentContext();
                e->Set(c, v8::String::NewFromUtf8(iso, "pointerType").ToLocalChecked(),
                       v8::String::NewFromUtf8(iso, "mouse").ToLocalChecked()).Check();
            });
            return 0;
        }
        case WM_MOUSEWHEEL: {
            const double delta = -static_cast<double>(GET_WHEEL_DELTA_WPARAM(wparam));
            FireEvent("wheel", [=](v8::Isolate* iso, v8::Local<v8::Object> e) {
                SetNum(iso, e, "deltaY", delta); SetNum(iso, e, "deltaX", 0);
            });
            return 0;
        }

        // ---- キーボード ----
        case WM_KEYDOWN: case WM_KEYUP: {
            const int vk = static_cast<int>(wparam);
            const bool down = (msg == WM_KEYDOWN);
            const bool repeat = down && (lparam & (1 << 30)) != 0;
            FireEvent(down ? "keydown" : "keyup", [=](v8::Isolate* iso, v8::Local<v8::Object> e) {
                SetNum(iso, e, "keyCode", vk);
                SetNum(iso, e, "which", vk);
                SetBoolProp(iso, e, "repeat", repeat);
                SetBoolProp(iso, e, "shiftKey", (GetKeyState(VK_SHIFT) & 0x8000) != 0);
                SetBoolProp(iso, e, "ctrlKey", (GetKeyState(VK_CONTROL) & 0x8000) != 0);
                SetBoolProp(iso, e, "altKey", (GetKeyState(VK_MENU) & 0x8000) != 0);
                // key: A-Z/0-9 のみ簡易マッピング (それ以外は空)
                char key[2] = {0, 0};
                if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) key[0] = static_cast<char>(vk);
                v8::Local<v8::Context> c = iso->GetCurrentContext();
                e->Set(c, v8::String::NewFromUtf8(iso, "key").ToLocalChecked(),
                       v8::String::NewFromUtf8(iso, key).ToLocalChecked()).Check();
            });
            return 0;
        }

        case WM_SIZE:
            if (g_dawn && g_host && wparam != SIZE_MINIMIZED) {
                const uint32_t w = LOWORD(lparam);
                const uint32_t h = HIWORD(lparam);
                if (w > 0 && h > 0) {
                    g_host->viewport_width = static_cast<int>(w);
                    g_host->viewport_height = static_cast<int>(h);
                    if (g_dawn->ready()) {
                        g_dawn->Configure(w, h);
                    }
                }
            }
            return 0;
        default:
            return DefWindowProc(hwnd, msg, wparam, lparam);
    }
}

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

// 実行ファイル隣接ディレクトリを基準に絶対パスを得る。
fs::path ExeDir()
{
    wchar_t buffer[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return fs::path(buffer).parent_path();
}

HWND CreateHostWindow(HINSTANCE instance, int width, int height)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"Next2DXboxHostWindow";
    RegisterClassExW(&wc);

    // コンソールでは全画面相当。PC(GDK) 検証ではウィンドウ。
    const DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    return CreateWindowExW(
        0, wc.lpszClassName, L"Next2D",
        style, CW_USEDEFAULT, CW_USEDEFAULT, width, height,
        nullptr, nullptr, instance, nullptr);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmd_line, int)
{
    // --selftest: アプリの代わりに js/selftest.js を実行し、全バインディングを
    // 実機/PC(GDK) 上で検証して終了する (テスト完了で自動終了)。
    const bool selftest = cmd_line && wcsstr(cmd_line, L"--selftest") != nullptr;

    // 1. GDK ランタイム初期化。
    // デスクトップでは Gaming Services 未導入の環境 (CI ランナー等) でも
    // 起動できるよう、失敗を警告に留めて続行する (本ホストは XUser 等を未使用)。
    // コンソールでは必須のため失敗したら終了する。
    bool xgame_initialized = SUCCEEDED(XGameRuntimeInitialize());
    if (!xgame_initialized) {
#if NEXT2D_XBOX_CONSOLE
        std::cerr << "XGameRuntimeInitialize failed" << std::endl;
        return 1;
#else
        std::cerr << "warning: XGameRuntimeInitialize failed (Gaming Services not installed?)"
                  << " - continuing on desktop" << std::endl;
#endif
    }

    const fs::path exe_dir = ExeDir();
    const fs::path assets_app = exe_dir / "assets" / "app";
    const fs::path bootstrap_js = exe_dir / "js" / "bootstrap.js";
    const fs::path selftest_js = exe_dir / "js" / "selftest.js";

    // 2. HostContext と論理ビューポート
    HostContext host;
    host.viewport_width = 1920;
    host.viewport_height = 1080;
    g_host = &host;

    // 3. ウィンドウ
    HWND hwnd = CreateHostWindow(instance, host.viewport_width, host.viewport_height);
    if (!hwnd) {
        std::cerr << "CreateWindow failed" << std::endl;
        return 1;
    }

    // 4. V8 プロセス初期化 + Isolate/Context + バインディング
    V8Runtime::InitializeProcess(nullptr);
    V8Runtime runtime;
    if (!runtime.Initialize(&host)) {
        std::cerr << "V8 init failed" << std::endl;
        return 1;
    }
    g_runtime = &runtime;

    // 5. サブシステム生成と HostContext への接続
    EventLoop event_loop(runtime.isolate());
    DawnContext dawn;
    AssetLoader assets(assets_app.string());
    GamepadManager gamepad;
    AudioEngine audio;
    WorkerRuntime workers(runtime.isolate(), &host);

    host.event_loop = &event_loop;
    host.gpu = &dawn;
    host.assets = &assets;
    host.gamepad = &gamepad;
    host.audio = &audio;
    host.workers = &workers;
    g_dawn = &dawn;

    // GPU 無し環境 (CI 等) では Dawn 初期化に失敗し得るが、CPU 側の機能と
    // selftest は動かせるため続行する (Present 等は内部でゲートされる)
    if (!dawn.Initialize(hwnd, host.viewport_width, host.viewport_height)) {
        std::cerr << "warning: Dawn initialization failed - continuing without GPU" << std::endl;
    }
    gamepad.Initialize();
    audio.Initialize();

    // main Context に EventLoop を紐付け、Worker コンストラクタを設置する。
    // (V8Runtime::Initialize 時点では EventLoop/WorkerRuntime 未生成のためここで行う)
    {
        v8::Isolate::Scope iso_scope(runtime.isolate());
        v8::HandleScope hs(runtime.isolate());
        v8::Local<v8::Context> ctx = runtime.context();
        v8::Context::Scope cs(ctx);
        ctx->SetAlignedPointerInEmbedderData(kEventLoopEmbedderSlot, &event_loop);
        InstallWorker(runtime.isolate(), ctx->Global(), &host);
    }

    // 6. bootstrap.js -> アプリ本体(ESM) の順で読み込み
    {
        const std::string boot = ReadFile(bootstrap_js);
        if (boot.empty()) {
            std::cerr << "bootstrap.js not found: " << bootstrap_js << std::endl;
        } else if (!runtime.RunScript(boot, "js/bootstrap.js")) {
            std::cerr << "bootstrap.js execution failed" << std::endl;
        }

        if (selftest) {
            const std::string script = ReadFile(selftest_js);
            if (script.empty()) {
                std::cerr << "selftest.js not found: " << selftest_js << std::endl;
                return 1;
            }
            if (!runtime.RunScript(script, "js/selftest.js")) {
                std::cerr << "selftest.js execution failed" << std::endl;
                return 1;
            }
        } else {
            auto entry = assets.ResolveEntryModule();
            if (!entry) {
                std::cerr << "Application entry module not found under assets/app" << std::endl;
            } else if (!runtime.RunModule("", *entry)) {
                std::cerr << "Application module execution failed" << std::endl;
            }
        }
    }

    // 7. ゲームループ
    v8::Isolate::Scope isolate_scope(runtime.isolate());
    MSG msg = {};
    int exit_code = 0;
    while (g_running) {
        // Win32 メッセージ
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = false;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!g_running) {
            break;
        }

        const double now = event_loop.Now();

        v8::HandleScope handle_scope(runtime.isolate());
        v8::Local<v8::Context> ctx = runtime.context();
        v8::Context::Scope context_scope(ctx);

        // 入力
        gamepad.Poll(now);

        // 音声: 再生終了した source へ "ended" を配送する
        PumpAudioEvents(runtime.isolate());

        // タイマー -> マイクロタスク
        event_loop.PumpTimers();
        runtime.PumpMicrotasks();

        // メインスレッドの requestAnimationFrame (アプリのロジック/描画コマンド生成)
        event_loop.RunAnimationFrame(now);
        runtime.PumpMicrotasks();

        // Worker (レンダラ等) のメッセージ配送 + rAF。
        // レンダラ worker はここで WebGPU コマンドを発行し submit する。
        workers.Pump(now);

        // V8 プラットフォームタスク + Dawn の非同期処理
        runtime.PumpPlatformTasks();
        dawn.Tick();

        // 提示
        dawn.Present();

        // selftest 完了検知: selftest.js が globalThis.__selftestExitCode を設定したら終了
        if (selftest) {
            v8::Local<v8::Value> code;
            if (ctx->Global()->Get(ctx, v8::String::NewFromUtf8Literal(
                    runtime.isolate(), "__selftestExitCode")).ToLocal(&code) &&
                code->IsNumber()) {
                exit_code = static_cast<int>(code.As<v8::Number>()->Value());
                g_running = false;
            }
        }
    }

    // 8. 後始末 (v8::Global を持つ静的リストは V8 破棄前に必ず解放する)
    ShutdownAudioEvents();
    host.main_canvas.Reset();
    runtime.Dispose();
    V8Runtime::ShutdownProcess();
    if (xgame_initialized) {
        XGameRuntimeUninitialize();
    }
    return exit_code;
}
