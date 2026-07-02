#include "EventLoop.h"

#include "HostContext.h"
#include "v8/V8Util.h"

#include <algorithm>

namespace next2d {

EventLoop* CurrentEventLoop(v8::Isolate* isolate)
{
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    if (!ctx.IsEmpty() &&
        ctx->GetNumberOfEmbedderDataFields() > kEventLoopEmbedderSlot) {
        void* p = ctx->GetAlignedPointerFromEmbedderData(kEventLoopEmbedderSlot);
        if (p) {
            return static_cast<EventLoop*>(p);
        }
    }
    HostContext* host = HostContext::From(isolate);
    return host ? host->event_loop : nullptr;
}

EventLoop::EventLoop(v8::Isolate* isolate)
    : isolate_(isolate), start_(Clock::now())
{
}

EventLoop::~EventLoop() = default;

double EventLoop::Now() const
{
    const auto elapsed = Clock::now() - start_;
    return std::chrono::duration<double, std::milli>(elapsed).count();
}

uint32_t EventLoop::SetTimer(v8::Local<v8::Function> callback, double delay_ms, bool repeat)
{
    if (delay_ms < 0.0) {
        delay_ms = 0.0;
    }

    const uint32_t id = next_timer_id_++;
    TimerEntry entry;
    entry.callback.Reset(isolate_, callback);
    entry.due_ms = Now() + delay_ms;
    entry.interval_ms = delay_ms;
    entry.repeat = repeat;
    entry.cancelled = false;
    timers_.emplace(id, std::move(entry));
    return id;
}

void EventLoop::ClearTimer(uint32_t id)
{
    auto it = timers_.find(id);
    if (it != timers_.end()) {
        // 実行ループ中の削除に備え cancelled フラグで無効化
        it->second.cancelled = true;
    }
}

uint32_t EventLoop::RequestAnimationFrame(v8::Local<v8::Function> callback)
{
    const uint32_t id = next_raf_id_++;
    v8::Global<v8::Function> global;
    global.Reset(isolate_, callback);
    raf_callbacks_.emplace_back(id, std::move(global));
    return id;
}

void EventLoop::CancelAnimationFrame(uint32_t id)
{
    raf_cancelled_.push_back(id);
}

void EventLoop::PumpTimers()
{
    const double now = Now();

    v8::HandleScope handle_scope(isolate_);
    v8::Local<v8::Context> ctx = isolate_->GetCurrentContext();
    v8::Local<v8::Object> global = ctx->Global();

    // 期限到来分を収集 (map の走査中の変更を避けるため id を先に集める)
    std::vector<uint32_t> due;
    for (auto& [id, entry] : timers_) {
        if (!entry.cancelled && entry.due_ms <= now) {
            due.push_back(id);
        }
    }

    for (uint32_t id : due) {
        auto it = timers_.find(id);
        if (it == timers_.end() || it->second.cancelled) {
            continue;
        }

        v8::Local<v8::Function> fn = it->second.callback.Get(isolate_);

        if (it->second.repeat) {
            it->second.due_ms = now + it->second.interval_ms;
        }

        v8::TryCatch try_catch(isolate_);
        (void) fn->Call(ctx, global, 0, nullptr);
        // 例外はコンソールに委譲 (V8Runtime のメッセージハンドラが拾う)

        if (!it->second.repeat) {
            // 単発は実行後に破棄
            auto erase_it = timers_.find(id);
            if (erase_it != timers_.end()) {
                timers_.erase(erase_it);
            }
        }
    }

    // cancelled を掃除
    for (auto it = timers_.begin(); it != timers_.end();) {
        if (it->second.cancelled) {
            it = timers_.erase(it);
        } else {
            ++it;
        }
    }
}

void EventLoop::RunAnimationFrame(double timestamp_ms)
{
    if (raf_callbacks_.empty()) {
        return;
    }

    // ブラウザ同様、今フレームで実行するのは「現時点で登録済み」のコールバックのみ。
    // コールバック内で再登録された rAF は次フレームに回す。
    auto current = std::move(raf_callbacks_);
    raf_callbacks_.clear();

    v8::HandleScope handle_scope(isolate_);
    v8::Local<v8::Context> ctx = isolate_->GetCurrentContext();
    v8::Local<v8::Object> global = ctx->Global();
    v8::Local<v8::Value> arg = v8::Number::New(isolate_, timestamp_ms);

    for (auto& [id, cb] : current) {
        if (std::find(raf_cancelled_.begin(), raf_cancelled_.end(), id) != raf_cancelled_.end()) {
            continue;
        }
        v8::Local<v8::Function> fn = cb.Get(isolate_);
        v8::TryCatch try_catch(isolate_);
        v8::Local<v8::Value> args[1] = { arg };
        (void) fn->Call(ctx, global, 1, args);
    }

    raf_cancelled_.clear();
}

bool EventLoop::HasPendingWork() const
{
    return !timers_.empty() || !raf_callbacks_.empty();
}

} // namespace next2d
