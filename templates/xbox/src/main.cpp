// Next2D Xbox (GDK native) host エントリポイント。
//
// GDK ランタイム初期化 -> ウィンドウ生成 -> V8/Dawn/入力/音声のブートストラップ ->
// bootstrap.js とアプリ本体(ESM)の読み込み -> ゲームループ、という流れ。
#include <Windows.h>
#include <windowsx.h>
#include <timeapi.h>
#include <DbgHelp.h>
#include <XGameRuntime.h>

#pragma comment(lib, "winmm.lib")

#include "HostContext.h"
#include "AssetLoader.h"
#include "EmbeddedAssets.h"
#include "EventLoop.h"
#include "gpu/DawnContext.h"
#include "platform/DecodeQueue.h"
#include "platform/GamepadManager.h"
#include "platform/AudioEngine.h"
#include "worker/WorkerRuntime.h"
#include "bindings/Bindings.h"
#include "bindings/EventTarget.h"
#include "v8/V8Runtime.h"

#include "v8/V8Util.h"

#include <v8-profiler.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <cstdio>
#include <cstring>

namespace fs = std::filesystem;
using namespace next2d;

namespace {

bool g_running = true;

// --- --perf: フレーム統計 ---------------------------------------------------
// メインループの各区間の所要時間 (ms) を集計し、300 フレーム毎に 1 行で出力する。
// avg でボトルネックの区間を、max でヒッチ (スパイク) の在り処を特定する。
// GUI 実行では stderr が見えないため next2d-perf.log にも追記する。
struct PerfSection {
    double sum = 0.0;
    double max = 0.0;
    void Add(double ms)
    {
        sum += ms;
        if (ms > max) {
            max = ms;
        }
    }
};

struct PerfStats {
    static constexpr int kWindow = 300;   // 60fps で約 5 秒
    PerfSection tasks;     // 入力/音声/タイマー/マイクロタスク
    PerfSection js;        // メイン rAF (アプリロジック/描画コマンド生成)
    PerfSection worker;    // レンダラ worker (WebGPU コマンド発行 + submit)
    PerfSection tick;      // V8 プラットフォームタスク + Dawn Tick
    PerfSection present;   // Present (GPU/VSync 待ちを含む)
    PerfSection busy;      // 上記合計 (ペーシング Sleep を除く実働)
    PerfSection frame;     // フレーム間隔 (実効フレームレートの逆数)
    int frames = 0;

    void Flush()
    {
        if (frames == 0) {
            return;
        }
        const auto avg = [this](const PerfSection& s) { return s.sum / frames; };
        char line[320];
        std::snprintf(line, sizeof(line),
            "[perf] frame %.1fms (max %.1f) busy %.1fms | "
            "tasks %.2f/%.1f js %.2f/%.1f worker %.2f/%.1f "
            "tick %.2f/%.1f present %.2f/%.1f (avg/max, %d frames)",
            avg(frame), frame.max, avg(busy),
            avg(tasks), tasks.max, avg(js), js.max, avg(worker), worker.max,
            avg(tick), tick.max, avg(present), present.max, frames);
        std::cerr << line << std::endl;
        std::ofstream ofs("next2d-perf.log", std::ios::app);
        if (ofs) {
            ofs << line << std::endl;
        }
        *this = PerfStats{};
    }
};

// --profile: CPU プロファイル結果を self サンプル数の降順で next2d-cpuprofile.log へ。
// ノードツリーを再帰集計し「関数 (script:line)」毎の self 時間を出す。
constexpr unsigned kProfileIntervalUs = 500;

void DumpCpuProfileNode(const v8::CpuProfileNode* node,
                        std::map<std::string, unsigned>& self_hits)
{
    const char* name = node->GetFunctionNameStr();
    const char* script = node->GetScriptResourceNameStr();
    std::string key = (name && *name) ? name : "(anonymous)";
    if (script && *script) {
        // pak 内キーの末尾だけで十分読める
        std::string s(script);
        const auto slash = s.find_last_of('/');
        if (slash != std::string::npos) {
            s = s.substr(slash + 1);
        }
        key += " (" + s + ":" + std::to_string(node->GetLineNumber()) + ")";
    }
    self_hits[key] += node->GetHitCount();
    for (int i = 0; i < node->GetChildrenCount(); ++i) {
        DumpCpuProfileNode(node->GetChild(i), self_hits);
    }
}

void WriteCpuProfile(v8::CpuProfile* profile, unsigned interval_us, const char* reason)
{
    std::map<std::string, unsigned> self_hits;
    DumpCpuProfileNode(profile->GetTopDownRoot(), self_hits);

    unsigned total = 0;
    for (const auto& [key, hits] : self_hits) {
        total += hits;
    }
    std::vector<std::pair<std::string, unsigned>> sorted(self_hits.begin(), self_hits.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::ofstream ofs("next2d-cpuprofile.log", std::ios::app);
    const double dur_ms =
        static_cast<double>(profile->GetEndTime() - profile->GetStartTime()) / 1000.0;
    char head[224];
    std::snprintf(head, sizeof(head),
        "[cpuprofile] %s: window %.1fs, %u samples (interval %uus), top self-time:",
        reason, dur_ms / 1000.0, total, interval_us);
    std::cerr << head << std::endl;
    if (ofs) {
        ofs << head << std::endl;
    }
    int rank = 0;
    for (const auto& [key, hits] : sorted) {
        if (hits == 0 || ++rank > 50) {
            break;
        }
        char line[512];
        std::snprintf(line, sizeof(line), "  %5.1f%%  %8.1fms  %s",
            total ? 100.0 * hits / total : 0.0,
            hits * (interval_us / 1000.0), key.c_str());
        std::cerr << line << std::endl;
        if (ofs) {
            ofs << line << std::endl;
        }
    }
}

// 現在のプロファイル窓を書き出して新しい窓を開始する。
// 遷移スパイク (単発の長い rAF) 直後に呼ぶことで、スパイクを含む窓と含まない窓の
// 比較から犯人関数を分離する (セッション全体の集計では定常コストに埋もれるため)。
void DumpAndRestartProfile(v8::CpuProfiler* profiler, v8::Isolate* isolate,
                           const char* reason)
{
    v8::HandleScope hs(isolate);
    v8::CpuProfile* p = profiler->StopProfiling(
        v8::String::NewFromUtf8Literal(isolate, "next2d"));
    if (p) {
        WriteCpuProfile(p, kProfileIntervalUs, reason);
        p->Delete();
    }
    profiler->StartProfiling(v8::String::NewFromUtf8Literal(isolate, "next2d"));
}
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

    // ウィンドウが画面(作業領域)より大きいと画面外にはみ出し、提示が
    // 見切れる/非等倍でスケールされて歪む (CI の低解像度ディスプレイで
    // 1920x1080 ウィンドウ → 描画が縦長になっていた)。作業領域に収まるよう
    // クランプする。実機コンソールでは作業領域=全画面のため 1920x1080 のまま。
    RECT wa = {};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        const int maxW = wa.right - wa.left;
        const int maxH = wa.bottom - wa.top;
        if (maxW > 0 && width  > maxW) { width  = maxW; }
        if (maxH > 0 && height > maxH) { height = maxH; }
    }

    // コンソールでは全画面相当。PC(GDK) 検証ではウィンドウ。
    const DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    return CreateWindowExW(
        0, wc.lpszClassName, L"Next2D",
        style, CW_USEDEFAULT, CW_USEDEFAULT, width, height,
        nullptr, nullptr, instance, nullptr);
}

} // namespace

// 未処理例外(segfault/AV 等)発生時にコールスタックを next2d-error.log へ書き出す。
// GUI 実行では stderr が見えないため、C++ クラッシュ地点を掴む唯一の手段。
// DbgHelp の StackWalk64 で例外コンテキストからフレームを辿り、可能なら
// シンボル名 (関数+行) とモジュールを添える (PDB があれば関数/行まで解決)。
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep)
{
    std::ofstream ofs("next2d-error.log", std::ios::app);
    if (!ofs || !ep || !ep->ExceptionRecord || !ep->ContextRecord) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    ofs << "\n[CRASH] code=0x" << std::hex
        << ep->ExceptionRecord->ExceptionCode
        << " addr=" << ep->ExceptionRecord->ExceptionAddress
        << std::dec << std::endl;

    HANDLE process = GetCurrentProcess();
    HANDLE thread  = GetCurrentThread();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(process, nullptr, TRUE);

    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 frame = {};
    DWORD machine = 0;
#if defined(_M_X64)
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset    = ctx.Rip;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrStack.Offset = ctx.Rsp;
#elif defined(_M_ARM64)
    machine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset    = ctx.Pc;
    frame.AddrFrame.Offset = ctx.Fp;
    frame.AddrStack.Offset = ctx.Sp;
#endif
    frame.AddrPC.Mode    = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    char symbuf[sizeof(SYMBOL_INFO) + 256] = {};
    SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symbuf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen   = 255;

    for (int i = 0; i < 48; ++i) {
        if (!StackWalk64(machine, process, thread, &frame, &ctx,
                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            break;
        }
        const DWORD64 pc = frame.AddrPC.Offset;
        if (!pc) {
            break;
        }

        ofs << "  #" << i << " 0x" << std::hex << pc << std::dec;

        DWORD64 disp = 0;
        if (SymFromAddr(process, pc, &disp, sym)) {
            ofs << " " << sym->Name << "+0x" << std::hex << disp << std::dec;
        }

        const DWORD64 modbase = SymGetModuleBase64(process, pc);
        if (modbase) {
            char modname[MAX_PATH] = {};
            if (GetModuleFileNameA(reinterpret_cast<HMODULE>(modbase), modname, MAX_PATH)) {
                const char* base = std::strrchr(modname, '\\');
                ofs << " [" << (base ? base + 1 : modname) << "]";
            }
        }

        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD line_disp = 0;
        if (SymGetLineFromAddr64(process, pc, &line_disp, &line) && line.FileName) {
            const char* fbase = std::strrchr(line.FileName, '\\');
            ofs << " (" << (fbase ? fbase + 1 : line.FileName) << ":" << line.LineNumber << ")";
        }
        ofs << std::endl;
    }

    ofs.flush();
    SymCleanup(process);
    return EXCEPTION_EXECUTE_HANDLER;   // プロセスを終了させる (無限再入回避)
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmd_line, int)
{
    // 最初にクラッシュハンドラを設置し、以降の segfault 等でスタックを記録する。
    SetUnhandledExceptionFilter(CrashHandler);

    // Sleep() の分解能を 1ms に上げる (フレームペーシング用。既定 15.6ms では
    // 60Hz を刻めない)
    timeBeginPeriod(1);

    // --selftest: アプリの代わりに js/selftest.js を実行し、全バインディングを
    // 実機/PC(GDK) 上で検証して終了する (テスト完了で自動終了)。
    const bool selftest = cmd_line && wcsstr(cmd_line, L"--selftest") != nullptr;

    // --perf: フレーム統計を 300 フレーム (約 5 秒) 毎に stderr と next2d-perf.log
    // へ出力する。どの区間 (JS / レンダラ worker / GPU / Present) にフレーム時間を
    // 使っているかを数値化し、以降の最適化 (worker 並列化等) の要否判断に使う。
    const bool perf = cmd_line && wcsstr(cmd_line, L"--perf") != nullptr;

    // --profile: V8 の sampling CPU プロファイラを起動から終了まで回し、
    // 終了時に self 時間の上位関数を next2d-cpuprofile.log へ出力する。
    // jitless でも動作し、GC は "(garbage collector)" として現れる。
    // --perf で見つかった js 区間の遷移スパイク (~1.2s) の中身を特定する用途。
    const bool profile = cmd_line && wcsstr(cmd_line, L"--profile") != nullptr;

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

    // 埋め込みアセット (exe 内 RCDATA "N2DASSETS") を初期化する。存在すれば
    // assets/app 一式と js/bootstrap.js を exe 内から読み、隣接平文 JS は不要になる。
    // 無ければ全読み込みはファイルへフォールバックする (開発ビルド)。
    if (InitEmbeddedAssets()) {
        SetEmbeddedAssetsRoot(assets_app.string());
        std::cerr << "[Assets] using embedded pak" << std::endl;
    }

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

    // ウィンドウは要求サイズより小さく作られ得る (作業領域クランプ / DPI /
    // CI の低解像度ディスプレイ)。surface・canvas・screen は全て viewport_* から
    // 派生するため、ここで「実クライアント矩形」を採用して一致させる。これを
    // 怠ると surface(1920x1080) を実ウィンドウ(例 1024x768)へ非等倍提示して
    // 画面が縦長に歪む。V8 バインディング設置(screen/innerWidth/dpr)と
    // Dawn 初期化の前に確定させる必要がある。
    {
        RECT rc = {};
        if (GetClientRect(hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
            host.viewport_width  = rc.right;
            host.viewport_height = rc.bottom;
        }
        std::cerr << "[Win] client rect: " << host.viewport_width
                  << "x" << host.viewport_height << std::endl;
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

    // --profile: 起動直後からサンプリング開始 (0.5ms 間隔)
    v8::CpuProfiler* cpu_profiler = nullptr;
    if (profile) {
        v8::Isolate::Scope iso_scope(runtime.isolate());
        v8::HandleScope hs(runtime.isolate());
        cpu_profiler = v8::CpuProfiler::New(runtime.isolate());
        cpu_profiler->SetSamplingInterval(kProfileIntervalUs);
        cpu_profiler->StartProfiling(
            v8::String::NewFromUtf8Literal(runtime.isolate(), "next2d"));
        std::cerr << "[cpuprofile] profiling started" << std::endl;
    }

    // 6. bootstrap.js -> アプリ本体(ESM) の順で読み込み
    {
        // 埋め込み pak を優先し、無ければ隣接ファイルから読む host スクリプトローダ。
        const auto load_host_script = [&](const std::string& key, const fs::path& file) {
            if (const auto* embedded = GetEmbeddedAsset(key)) {
                return std::string(embedded->begin(), embedded->end());
            }
            return ReadFile(file);
        };

        const std::string boot = load_host_script("js/bootstrap.js", bootstrap_js);
        if (boot.empty()) {
            std::cerr << "bootstrap.js not found: " << bootstrap_js << std::endl;
        } else if (!runtime.RunScript(boot, "js/bootstrap.js")) {
            std::cerr << "bootstrap.js execution failed" << std::endl;
        }

        if (selftest) {
            const std::string script = load_host_script("js/selftest.js", selftest_js);
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
    MSG msg = {};
    int exit_code = 0;
    PerfStats perf_stats;
    double perf_prev_frame = 0.0;
    // Isolate::Scope はループ内に限定する。Enter されたままの isolate を
    // Dispose すると V8 の fatal (Disposing the isolate that is entered) になる。
    {
    v8::Isolate::Scope isolate_scope(runtime.isolate());
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

        // --perf: 直前の計測点からの経過を各区間へ記録する軽量マーカー
        double perf_cursor = now;
        const auto perf_mark = [&](PerfSection& section) {
            if (!perf) {
                return;
            }
            const double t = event_loop.Now();
            section.Add(t - perf_cursor);
            perf_cursor = t;
        };

        v8::HandleScope handle_scope(runtime.isolate());
        v8::Local<v8::Context> ctx = runtime.context();
        v8::Context::Scope context_scope(ctx);

        // 入力
        gamepad.Poll(now);

        // 音声: 再生終了した source へ "ended" を配送する
        PumpAudioEvents(runtime.isolate());

        // バックグラウンドデコード完了の反映 (画像 load イベント / 音声 Promise)
        decodequeue::Pump();

        // タイマー -> マイクロタスク
        event_loop.PumpTimers();
        runtime.PumpMicrotasks();
        perf_mark(perf_stats.tasks);

        // メインスレッドの requestAnimationFrame (アプリのロジック/描画コマンド生成)
        const double profile_js_start = cpu_profiler ? event_loop.Now() : 0.0;
        event_loop.RunAnimationFrame(now);
        runtime.PumpMicrotasks();
        perf_mark(perf_stats.js);

        // --profile: 遷移スパイク (単発の長い rAF) を検出したら窓を切り出す
        if (cpu_profiler) {
            const double js_ms = event_loop.Now() - profile_js_start;
            if (js_ms > 300.0) {
                char reason[64];
                std::snprintf(reason, sizeof(reason), "spike (js %.0fms)", js_ms);
                DumpAndRestartProfile(cpu_profiler, runtime.isolate(), reason);
            }
        }

        // Worker (レンダラ等) のメッセージ配送 + rAF。
        // レンダラ worker はここで WebGPU コマンドを発行し submit する。
        workers.Pump(now);
        perf_mark(perf_stats.worker);

        // V8 プラットフォームタスク + Dawn の非同期処理
        runtime.PumpPlatformTasks();
        dawn.Tick();
        perf_mark(perf_stats.tick);

        // 提示
        dawn.Present();
        perf_mark(perf_stats.present);

        if (perf) {
            perf_stats.busy.Add(perf_cursor - now);
            if (perf_prev_frame > 0.0) {
                perf_stats.frame.Add(now - perf_prev_frame);
            }
            perf_prev_frame = now;
            if (++perf_stats.frames >= PerfStats::kWindow) {
                perf_stats.Flush();
            }
        }

        // フレームペーシング: ブラウザの rAF 同様 ~60Hz に揃える。
        // Present はスキップ時にブロックしないため、これが無いとループが
        // 数万回転/秒で空回りし、rAF 前提のゲーム/Tween のタイミングが崩れる。
        {
            const double frame_ms = event_loop.Now() - now;
            if (frame_ms < 15.0) {
                Sleep(static_cast<DWORD>(15.0 - frame_ms));
            }
        }

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

    }

    // --profile: 終了時に集計して出力する (Isolate 破棄前に必ず行う)
    if (cpu_profiler) {
        v8::Isolate::Scope iso_scope(runtime.isolate());
        v8::HandleScope hs(runtime.isolate());
        v8::CpuProfile* p = cpu_profiler->StopProfiling(
            v8::String::NewFromUtf8Literal(runtime.isolate(), "next2d"));
        if (p) {
            WriteCpuProfile(p, kProfileIntervalUs, "final");
            p->Delete();
        }
        cpu_profiler->Dispose();
        cpu_profiler = nullptr;
    }

    // 8. 後始末: v8::Global を保持するものは Isolate 破棄前に必ず明示解放する。
    //    (スタック変数はスコープ終了 = runtime.Dispose() の後に巻き戻されるため、
    //     デストラクタ任せにすると破棄済み Isolate への Global::Reset で fail-fast する)
    decodequeue::Shutdown(); // デコードスレッド join + 保留 complete (v8::Global) の破棄
    workers.Shutdown();      // WorkerInstance の Global<Context> / EventLoop
    event_loop.Shutdown();   // main の setTimeout/rAF コールバック (Global<Function>)
    ShutdownAudioEvents();
    ShutdownWebGPU();
    host.main_canvas.Reset();
    runtime.Dispose();
    V8Runtime::ShutdownProcess();
    if (xgame_initialized) {
        XGameRuntimeUninitialize();
    }
    return exit_code;
}
