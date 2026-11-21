// WorkerRuntime: Web Worker の実装。動かし方は 2 通りある (HostContext::threaded_workers)。
//
// 協調 (既定): 同じ Isolate の別 Context を、メインのスレッドで順番に動かす。
//   GPU を 1 スレッドに保てるので、Dawn (WebGPU) の構成で使う。
//
// 専用スレッド: worker ごとに Isolate とスレッドを作って並行に動かす。
//   player はレンダラを Worker + transferControlToOffscreen(OffscreenCanvas) で動かし、
//   メインから postMessage で描画コマンドを送る。レンダラを別のスレッドへ出すと、ゲームの
//   処理 (メイン) と描画 (レンダラ) が別々のコアで同時に進む。GL のコンテキストは、WebGL2 の
//   コンテキストを作ったスレッド (レンダラ) が持ち、提示もそのスレッドで行う。
//
// - new Worker(url)         : ワーカースクリプトを評価 (専用スレッドではそのスレッドで)
// - worker.postMessage(m,t) : 親→worker のメッセージキューへ (ValueSerializer)
// - self.postMessage(m,t)   : worker→親 のメッセージキューへ
// - Pump()                  : 親のスレッドで、worker→親 のメッセージを配送する
//                             (協調では、親→worker の配送と worker の EventLoop の前進も行う)
//
// メッセージ (WorkerMessage) は isolate に依存しないバイト列と ArrayBuffer の実体なので、
// どちらの動かし方でも同じ形で受け渡す。
#pragma once

#include <v8.h>

#include "../platform/NativeThread.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace next2d {

class HostContext;
class EventLoop;
class WorkerRuntime;

// worker context の embedder slot。1 は EventLoop (kEventLoopEmbedderSlot) が使用。
// ESM の ResolveModule コールバックが context から WorkerInstance を引くために使う。
constexpr int kWorkerInstanceEmbedderSlot = 2;

// 1 メッセージ = ValueSerializer 出力バイト列 + transfer された ArrayBuffer の実体。
// transfer_list の ArrayBuffer は中身を bytes に埋め込まず所有権ごと移す
// (render キューは数十 MB になるため、コピーするとフレームレートが崩壊する)。
struct WorkerMessage {
    std::vector<uint8_t> data;
    std::vector<std::shared_ptr<v8::BackingStore>> transfers;
};

class WorkerInstance {
public:
    // parent_isolate: new Worker を呼んだ側の isolate (親のスレッドで動いている)
    WorkerInstance(WorkerRuntime* runtime, v8::Isolate* parent_isolate, std::string url,
                   bool is_module = false);
    // 親のスレッドから呼ぶ。専用スレッドの worker は、スレッドの終了を待つ。
    ~WorkerInstance();

    // ワーカースクリプトを読み込んで起動する (親のスレッドから呼ぶ)。
    // 協調ではその場で評価し、専用スレッドではスレッドを作ってそちらで評価する。
    bool Launch();
    void Terminate();

    // 親→worker / worker→親 の投函 (どのスレッドからでも呼べる)
    void PostToWorker(WorkerMessage message);
    void PostToMain(WorkerMessage message);

    // 親のスレッドで、worker→親 のメッセージを配送する (WorkerRuntime::Pump から呼ぶ)。
    // 協調では、その前に 親→worker の配送とタイマー/rAF の前進も行う。
    void Deliver(double now_ms);

    v8::Local<v8::Context> context() const;
    EventLoop* event_loop() const { return loop_.get(); }

    // 親側の Worker オブジェクト (onmessage を保持)。親の isolate に属する。
    v8::Global<v8::Object> main_worker_object;

private:
    bool LoadSource();            // スクリプトの本文を source_ へ読み込む (親のスレッド)
    bool Start();                 // Context 生成 + ワーカースクリプト評価 (worker を動かすスレッド)

    // worker 側の 1 回分の処理: メッセージの配送、タイマー、(run_raf なら) rAF
    void RunWorkerTurn(std::deque<WorkerMessage>& batch, double now_ms, bool run_raf);
    void DeliverToMain();

    // 専用スレッド
    void ThreadMain();
    void ThreadLoop();
    void Wake();

    // ESM worker (type: "module")。分類スクリプトは Script、モジュールは Module として評価。
    bool EvaluateModule(v8::Local<v8::Context> context, const std::string& source);
    v8::MaybeLocal<v8::Module> LoadModule(const std::string& url);
    static v8::MaybeLocal<v8::Module> ResolveModule(
        v8::Local<v8::Context> context,
        v8::Local<v8::String> specifier,
        v8::Local<v8::FixedArray> import_attributes,
        v8::Local<v8::Module> referrer);

    WorkerRuntime* runtime_;            // 親の runtime
    v8::Isolate* parent_isolate_;
    v8::Isolate* isolate_;              // worker の isolate (協調では parent_isolate_ と同じ)
    const bool threaded_;
    std::string url_;
    bool module_ = false;
    std::string source_;
    v8::Global<v8::Context> context_;
    std::unique_ptr<EventLoop> loop_;

    // メッセージキューと起床の合図。mutex_ で守る。
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<WorkerMessage> to_worker_;
    std::deque<WorkerMessage> to_main_;
    bool wake_ = false;
    std::atomic<bool> terminated_{false};

    // 計測用 (NEXT2D_HOST_PERF): 親がメッセージを投函した時刻と、worker が返事を投函するまでの時間
    std::atomic<long long> perf_posted_us_{0};   // 0 = 返事待ちのメッセージなし
    int perf_replies_ = 0;                       // 以下は worker のスレッドだけが触る
    int perf_late_replies_ = 0;
    double perf_reply_total_ms_ = 0.0;
    double perf_reply_max_ms_ = 0.0;

    // 専用スレッドの worker だけが持つもの (スレッドの中で作り、スレッドの中で破棄する)
    std::unique_ptr<HostContext> thread_host_;
    std::unique_ptr<WorkerRuntime> thread_workers_;   // 入れ子の Worker 用
    NativeThread thread_;

    // ESM 解決: url -> Module、Module identity hash -> url (referrer 特定用)。
    std::map<std::string, v8::Global<v8::Module>> modules_;
    std::map<int, std::string> module_paths_;
};

class WorkerRuntime {
public:
    WorkerRuntime(v8::Isolate* isolate, HostContext* host);
    ~WorkerRuntime();

    v8::Isolate* isolate() const { return isolate_; }
    HostContext* host() const { return host_; }

    // main / worker グローバルに Worker コンストラクタ等を設置する。
    void InstallOnGlobal(v8::Local<v8::Object> global);

    // new Worker(url, { type }) の実体
    v8::Local<v8::Object> CreateWorker(const std::string& url, bool is_module = false);

    // 全 worker のメッセージ配送 (協調ではイベントループの前進も)
    void Pump(double now_ms);

    bool HasWorkers() const { return !workers_.empty(); }

    // 専用スレッドの worker から親へメッセージが届いたときに呼ぶ関数 (親がスレッドで待って
    // いる場合に起こすため)。メインは毎フレーム Pump するので設定しない。
    void SetChildMessageNotifier(std::function<void()> notifier) { notifier_ = std::move(notifier); }
    void NotifyChildMessage() { if (notifier_) notifier_(); }

    // 全 WorkerInstance を破棄する (専用スレッドの worker は終了を待つ)。WorkerInstance は
    // v8::Global (context_ / main_worker_object 等) を保持するため、Isolate::Dispose より前に
    // 必ず呼ぶこと (呼ばずにスタック巻き戻しで破棄されると Isolate 破棄後の Global::Reset で
    // fail-fast)。
    void Shutdown() { workers_.clear(); }

private:
    v8::Isolate* isolate_;
    HostContext* host_;
    std::vector<std::unique_ptr<WorkerInstance>> workers_;
    std::function<void()> notifier_;
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
