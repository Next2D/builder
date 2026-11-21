#include "DecodeQueue.h"

#ifdef _WIN32
#include <objbase.h>
#endif

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace next2d::decodequeue {

namespace {

std::atomic<int> g_scene_loads{0};   // AddSceneLoad / SceneLoads

struct Job {
    std::function<void()> work;
    std::function<void()> complete;
    std::thread::id owner;   // Submit を呼んだスレッド。complete はこのスレッドの Pump で実行する
};

struct Done {
    std::function<void()> complete;
    std::thread::id owner;
};

struct Queue {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Job> pending;                       // 未処理 (プールスレッドが取る)
    std::deque<Done> done;                         // work 完了済み (Pump が実行)
    std::vector<std::thread> threads;
    bool stop = false;

    void ThreadMain()
    {
#ifdef _WIN32
        // WIC / Media Foundation は COM を要する。プールスレッド毎に MTA で初期化する
        // (per-call 実装なので MTA スレッド間で共有状態は無い)。
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [this] { return stop || !pending.empty(); });
                if (stop) {
                    break;
                }
                job = std::move(pending.front());
                pending.pop_front();
            }
            if (job.work) {
                job.work();
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                done.push_back({std::move(job.complete), job.owner});
            }
        }
#ifdef _WIN32
        CoUninitialize();
#endif
    }

    void EnsureThreads()
    {
        if (!threads.empty()) {
            return;
        }
        // デコードは CPU バウンド。コア数の半分 (2〜4) で遷移時のバースト
        // (画像数十枚 + BGM) を並列処理しつつ、メイン/GC スレッドを圧迫しない。
        const unsigned hw = std::thread::hardware_concurrency();
        const unsigned count = std::clamp(hw / 2u, 2u, 4u);
        threads.reserve(count);
        for (unsigned i = 0; i < count; ++i) {
            threads.emplace_back([this] { ThreadMain(); });
        }
    }
};

Queue& Q()
{
    static Queue q;
    return q;
}

} // namespace

void Submit(std::function<void()> work, std::function<void()> complete)
{
    Queue& q = Q();
    {
        std::lock_guard<std::mutex> lock(q.mutex);
        if (q.stop) {
            return;   // Shutdown 後の Submit は破棄 (終了処理中のロード要求)
        }
        q.EnsureThreads();
        q.pending.push_back({std::move(work), std::move(complete), std::this_thread::get_id()});
    }
    q.cv.notify_one();
}

void Pump()
{
    Queue& q = Q();
    // complete は V8 に触れるので、依頼したスレッド (その isolate を動かしているスレッド) の分だけ実行する
    const std::thread::id self = std::this_thread::get_id();
    std::deque<std::function<void()>> ready;
    {
        std::lock_guard<std::mutex> lock(q.mutex);
        for (auto it = q.done.begin(); it != q.done.end();) {
            if (it->owner == self) {
                ready.push_back(std::move(it->complete));
                it = q.done.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& complete : ready) {
        if (complete) {
            complete();
        }
    }
}

void DropForThisThread()
{
    Queue& q = Q();
    const std::thread::id self = std::this_thread::get_id();
    std::deque<Job> dropped_pending;
    std::deque<Done> dropped_done;
    {
        std::lock_guard<std::mutex> lock(q.mutex);
        for (auto it = q.pending.begin(); it != q.pending.end();) {
            if (it->owner == self) {
                dropped_pending.push_back(std::move(*it));
                it = q.pending.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = q.done.begin(); it != q.done.end();) {
            if (it->owner == self) {
                dropped_done.push_back(std::move(*it));
                it = q.done.erase(it);
            } else {
                ++it;
            }
        }
    }
    // dropped_* は、ロックを放したあとにここ (呼び出したスレッド) で破棄される
}

void Shutdown()
{
    Queue& q = Q();
    {
        std::lock_guard<std::mutex> lock(q.mutex);
        q.stop = true;
    }
    q.cv.notify_all();
    for (auto& t : q.threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    q.threads.clear();
    // 未実行の work / complete を破棄する。complete は v8::Global を捕捉して
    // いるため、この関数は Isolate 破棄前に呼ばれる必要がある (main.cpp の後始末順)。
    std::lock_guard<std::mutex> lock(q.mutex);
    q.pending.clear();
    q.done.clear();
    g_scene_loads.store(0);   // 破棄した complete の分は減らされないので、ここで 0 に戻す
}

void AddSceneLoad(int delta)
{
    g_scene_loads.fetch_add(delta);
}

int SceneLoads()
{
    return std::max(0, g_scene_loads.load());
}

} // namespace next2d::decodequeue
