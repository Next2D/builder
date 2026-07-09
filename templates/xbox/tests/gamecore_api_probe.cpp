// Game Core (Xbox コンソール) API パーティション監査プローブ。
//
// Windows SDK (19041+) は各 API 宣言を WINAPI_PARTITION_* でガードしており、
// `cl /DWINAPI_FAMILY=WINAPI_FAMILY_GAMES` でコンパイルすると GAMES パーティションに
// 属さない API は「未宣言」エラーになる。= NDA 資料なしで、公開 SDK だけを使って
// 「コンソールで使えない API」を正確に列挙できる (プリプロセッサが公式のゲート
// ロジックそのものを評価するため、テキスト解析より確実)。
//
// 使い方 (xbox-host-ci の gamecore-api-audit ジョブが実行):
//   cl /c /DWINAPI_FAMILY=WINAPI_FAMILY_GAMES tests\gamecore_api_probe.cpp
//     -> ホストがコンソールで使う API の存在検証 (エラー = 直ちに修正が必要)
//   cl /c /DWINAPI_FAMILY=WINAPI_FAMILY_GAMES /DPROBE_V8 tests\gamecore_api_probe.cpp
//     -> V8 の platform 層 (src/base/platform/*-win32) が使う API の存在検証
//        (エラー = V8 Game Core 移植時に置換が必要な TODO リスト。情報提供扱い)
//
// 注意: これは「宣言がパーティションに含まれるか」の静的検査。実機での挙動
// (リンク先 lib の有無・実行時の権限) は devkit での検証が別途必要。
#include <Windows.h>

#include <objbase.h>      // CoInitializeEx (DecodeQueue のスレッド COM 初期化)
#include <bcrypt.h>       // BCryptGenRandom (crypto.getRandomValues)
#include <xaudio2.h>      // XAudio2Create (音声出力)

#ifdef PROBE_V8
#include <psapi.h>        // GetProcessMemoryInfo (V8 OS::GetPeakMemoryUsageKb)
#include <process.h>      // _beginthreadex (V8 Thread)
#endif

// 関数アドレスを取るだけの配列。宣言が無ければコンパイルエラーになる。
#define PROBE(fn) reinterpret_cast<void*>(&fn)

// ---------------------------------------------------------------------------
// ホストがコンソールビルドでも使う API (NEXT2D_XBOX_CONSOLE ガード外のもの)。
// ここのエラーはホスト側の修正が必要 (ジョブを fail させる)。
// ---------------------------------------------------------------------------
void* const g_host_apis[] = {
    // ウィンドウ / メッセージループ (main.cpp。GDK コンソールサンプルも同構成)
    PROBE(RegisterClassExW),
    PROBE(CreateWindowExW),
    PROBE(DefWindowProcW),
    PROBE(PeekMessageW),
    PROBE(TranslateMessage),
    PROBE(DispatchMessageW),
    PROBE(PostQuitMessage),
    PROBE(LoadCursorW),
    PROBE(GetClientRect),

    // 時刻 / ペーシング (EventLoop::Now, メインループ)
    PROBE(QueryPerformanceCounter),
    PROBE(QueryPerformanceFrequency),
    PROBE(Sleep),

    // モジュール / 環境 (ExeDir, DomShims の storage パス解決)
    PROBE(GetModuleFileNameW),
    PROBE(GetModuleHandleW),
    PROBE(GetEnvironmentVariableW),
    PROBE(GetLastError),

    // COM (DecodeQueue プールスレッド。コンソールでは WIC/MF は使わないが
    // CoInitializeEx 自体は呼ぶ)
    PROBE(CoInitializeEx),
    PROBE(CoUninitialize),

    // 乱数 (crypto.getRandomValues)
    PROBE(BCryptGenRandom),

    // 音声 (XAudio2 はコンソールでは xaudio2_9)
    PROBE(XAudio2Create),

    // リソース (EmbeddedAssets の RCDATA 読み込み)
    PROBE(FindResourceW),
    PROBE(LoadResource),
    PROBE(LockResource),
    PROBE(SizeofResource),
};

#ifdef PROBE_V8
// ---------------------------------------------------------------------------
// V8 13.7 の platform 層 (src/base/platform/{platform-win32,time,mutex,
// condition-variable,semaphore}.cc) から機械抽出した Win32 API。
// ここのエラー = V8 Game Core 移植時の置換 TODO (情報提供、ジョブは fail しない)。
// ---------------------------------------------------------------------------
void* const g_v8_apis[] = {
    // 仮想メモリ (焦点: コンソールでは XMemVirtualAlloc への置換が推奨とされる)
    PROBE(VirtualAlloc),
    PROBE(VirtualFree),
    PROBE(VirtualProtect),
    PROBE(VirtualQuery),
    PROBE(DiscardVirtualMemory),
    // VirtualAlloc2 / MapViewOfFile3 / UnmapViewOfFile2 は V8 が GetProcAddress で
    // 動的解決するが、宣言の有無がパーティション対応の指標になるため含める
    PROBE(VirtualAlloc2),
    PROBE(MapViewOfFile3),
    PROBE(UnmapViewOfFile2),

    // ファイルマッピング (OS::MemoryMappedFile)
    PROBE(CreateFileMappingW),
    PROBE(CreateFileW),
    PROBE(MapViewOfFile),
    PROBE(MapViewOfFileEx),
    PROBE(UnmapViewOfFile),
    PROBE(GetFileSize),
    PROBE(GetFileType),
    PROBE(DeleteFileA),
    PROBE(GetTempPathA),
    PROBE(GetTempFileNameA),

    // スレッド / TLS / 同期
    PROBE(GetCurrentThread),
    PROBE(GetCurrentThreadId),
    PROBE(SetThreadPriority),
    PROBE(TlsAlloc),
    PROBE(TlsFree),
    PROBE(TlsGetValue),
    PROBE(TlsSetValue),
    PROBE(CreateSemaphoreA),
    PROBE(ReleaseSemaphore),
    PROBE(WaitForSingleObject),
    PROBE(CloseHandle),
    PROBE(CreateWaitableTimerExW),
    PROBE(SetWaitableTimer),

    // 時刻 (焦点: timeGetTime は winmm — デスクトップ専用の可能性が高い)
    PROBE(timeGetTime),
    PROBE(GetSystemTime),
    PROBE(GetSystemTimeAsFileTime),
    PROBE(SystemTimeToFileTime),
    PROBE(GetThreadTimes),
    PROBE(GetTimeZoneInformation),

    // プロセス / システム情報
    PROBE(GetCurrentProcess),
    PROBE(GetCurrentProcessId),
    PROBE(GetSystemInfo),
    PROBE(ExitProcess),
    PROBE(TerminateProcess),
    PROBE(GetProcessMemoryInfo),
    PROBE(SetErrorMode),
    PROBE(GetStdHandle),

    // 文字列変換 / デバッグ / モジュール
    PROBE(MultiByteToWideChar),
    PROBE(WideCharToMultiByte),
    PROBE(OutputDebugStringA),
    PROBE(DebugBreak),
    PROBE(IsDebuggerPresent),
    PROBE(LoadLibraryW),
    PROBE(GetProcAddress),
    PROBE(VerifyVersionInfoW),   // IsWindows10OrGreater (versionhelpers) の実体

    // CRT (参考: パーティション対象外のはずだが確認)
    reinterpret_cast<void*>(&_beginthreadex),
};
#endif // PROBE_V8

// リンク不要 (cl /c のみ)。配列が最適化で消えないよう参照を残す。
void* GameCoreApiProbeAnchor()
{
    void* acc = const_cast<void**>(g_host_apis)[0];
#ifdef PROBE_V8
    acc = const_cast<void**>(g_v8_apis)[0];
#endif
    return acc;
}
