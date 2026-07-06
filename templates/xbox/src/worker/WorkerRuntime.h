// WorkerRuntime: Web Worker を「同一 Isolate 内の別 Context を協調スケジューリング」する
// 単一スレッドモデルで実装する。
//
// player はレンダラを Worker + transferControlToOffscreen(OffscreenCanvas) で動かし、
// メインスレッドから postMessage で描画コマンドを送る。真のマルチスレッド化は
// Dawn/GPU のスレッド安全性を要するため、まず GPU を 1 スレッドに保てる協調モデルを採る。
//
// - new Worker(url)         : 別 Context を生成しワーカースクリプトを評価
// - worker.postMessage(m,t) : main→worker のメッセージキューへ (ValueSerializer)
// - self.postMessage(m,t)   : worker→main のメッセージキューへ
// - Pump()                  : 双方向のメッセージ配送 + 各 worker の EventLoop 前進
#pragma once

#include <v8.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace next2d {

class HostContext;
class EventLoop;
class WorkerRuntime;

// 1 メッセージ = ValueSerializer 出力バイト列 + transfer された ArrayBuffer の実体。
// transfer_list の ArrayBuffer は中身を bytes に埋め込まず所有権ごと移す
// (render キューは数十 MB になるため、コピーするとフレームレートが崩壊する)。
struct WorkerMessage {
    std::vector<uint8_t> data;
    std::vector<std::shared_ptr<v8::BackingStore>> transfers;
};

class WorkerInstance {
public:
    WorkerInstance(WorkerRuntime* runtime, v8::Isolate* isolate, std::string url);
    ~WorkerInstance();

    bool Start();                 // Context 生成 + ワーカースクリプト評価
    void Terminate();

    // main→worker / worker→main の投函
    void PostToWorker(WorkerMessage message);
    void PostToMain(WorkerMessage message);

    // 配送 + タイマー/rAF 前進 (WorkerRuntime::Pump から呼ぶ)
    void Deliver(double now_ms);

    v8::Local<v8::Context> context() const;
    EventLoop* event_loop() const { return loop_.get(); }

    // main 側の Worker オブジェクト (onmessage を保持)
    v8::Global<v8::Object> main_worker_object;
    // worker 側 self.onmessage
    v8::Global<v8::Function> worker_onmessage;
    // main 側 worker.onmessage
    v8::Global<v8::Function> main_onmessage;

private:
    void DeliverToWorker(double now_ms);
    void DeliverToMain();

    WorkerRuntime* runtime_;
    v8::Isolate* isolate_;
    std::string url_;
    v8::Global<v8::Context> context_;
    std::unique_ptr<EventLoop> loop_;
    std::deque<WorkerMessage> to_worker_;
    std::deque<WorkerMessage> to_main_;
    bool terminated_ = false;
};

class WorkerRuntime {
public:
    WorkerRuntime(v8::Isolate* isolate, HostContext* host);
    ~WorkerRuntime();

    v8::Isolate* isolate() const { return isolate_; }
    HostContext* host() const { return host_; }

    // main / worker グローバルに Worker コンストラクタ等を設置する。
    void InstallOnGlobal(v8::Local<v8::Object> global);

    // new Worker(url) の実体
    v8::Local<v8::Object> CreateWorker(const std::string& url);

    // 全 worker のメッセージ配送とイベントループ前進
    void Pump(double now_ms);

    bool HasWorkers() const { return !workers_.empty(); }

    // 全 WorkerInstance を破棄する。WorkerInstance は v8::Global (context_ /
    // main_worker_object 等) を保持するため、Isolate::Dispose より前に必ず呼ぶこと
    // (呼ばずにスタック巻き戻しで破棄されると Isolate 破棄後の Global::Reset で fail-fast)。
    void Shutdown() { workers_.clear(); }

private:
    v8::Isolate* isolate_;
    HostContext* host_;
    std::vector<std::unique_ptr<WorkerInstance>> workers_;
};

// ValueSerializer/Deserializer による structured clone
// (OffscreenCanvas / ImageBitmap のホストオブジェクト対応 + ArrayBuffer transfer)。
// 失敗時は data が空の WorkerMessage を返す。transfer_list 内の ArrayBuffer は
// detach され、backing store が message.transfers へ移る。
WorkerMessage SerializeMessage(v8::Isolate* isolate, v8::Local<v8::Context> source_ctx,
                               v8::Local<v8::Value> message,
                               v8::Local<v8::Value> transfer_list);

// message を target_ctx で復元する (transfer された ArrayBuffer は同一実体を再装着)。
v8::MaybeLocal<v8::Value> DeserializeMessage(v8::Isolate* isolate,
                                             v8::Local<v8::Context> target_ctx,
                                             const WorkerMessage& message);

} // namespace next2d
