// EventLoop: setTimeout / setInterval / requestAnimationFrame を実装する非同期タスクキュー。
//
// ブラウザのイベントループを単一スレッドで模倣する。main.cpp のゲームループが毎フレーム
// PumpTimers() と RunAnimationFrame() を呼び出す。V8 のマイクロタスク(Promise)は
// V8Runtime 側で各タスク実行後にチェックポイントされる。
#pragma once

#include <v8.h>
#include <chrono>
#include <cstdint>
#include <map>
#include <vector>

namespace next2d {

// Context の embedder data slot に EventLoop* を格納するインデックス。
// main / 各 worker の Context がそれぞれ自分の EventLoop を登録し、
// Timers/rAF バインディングは実行中の Context の EventLoop を参照する。
constexpr int kEventLoopEmbedderSlot = 1;

class EventLoop;

// 現在の Context に紐づく EventLoop を返す (未登録なら HostContext の既定)。
EventLoop* CurrentEventLoop(v8::Isolate* isolate);

class EventLoop {
public:
    explicit EventLoop(v8::Isolate* isolate);
    ~EventLoop();

    using Clock = std::chrono::steady_clock;

    // performance.now() 相当 (起動からの経過ミリ秒)
    double Now() const;

    // setTimeout / setInterval
    uint32_t SetTimer(v8::Local<v8::Function> callback, double delay_ms, bool repeat);
    void ClearTimer(uint32_t id);

    // requestAnimationFrame / cancelAnimationFrame
    uint32_t RequestAnimationFrame(v8::Local<v8::Function> callback);
    void CancelAnimationFrame(uint32_t id);

    // 期限の来たタイマーを実行する (毎フレーム冒頭)
    void PumpTimers();

    // 登録済み rAF コールバックをタイムスタンプ付きで実行する (毎フレーム)
    void RunAnimationFrame(double timestamp_ms);

    // 保留タスクが残っているか (終了判定用)
    bool HasPendingWork() const;

private:
    struct TimerEntry {
        v8::Global<v8::Function> callback;
        double due_ms;
        double interval_ms;
        bool repeat;
        bool cancelled;
    };

    v8::Isolate* isolate_;
    Clock::time_point start_;

    uint32_t next_timer_id_ = 1;
    std::map<uint32_t, TimerEntry> timers_;

    uint32_t next_raf_id_ = 1;
    std::vector<std::pair<uint32_t, v8::Global<v8::Function>>> raf_callbacks_;
    std::vector<uint32_t> raf_cancelled_;
};

} // namespace next2d
