#include "DecodeQueue.h"

#ifdef _WIN32
#include <objbase.h>
#endif

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace next2d::decodequeue {

namespace {

struct Job {
    std::function<void()> work;
    std::function<void()> complete;
};

struct Queue {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Job> pending;                       // 未処理 (プールスレッドが取る)
    std::deque<std::function<void()>> done;        // work 完了済み (Pump が実行)
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
                done.push_back(std::move(job.complete));
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
        q.pending.push_back({std::move(work), std::move(complete)});
    }
    q.cv.notify_one();
}

void Pump()
{
    Queue& q = Q();
    std::deque<std::function<void()>> ready;
    {
        std::lock_guard<std::mutex> lock(q.mutex);
        ready.swap(q.done);
    }
    for (auto& complete : ready) {
        if (complete) {
            complete();
        }
    }
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
}

} // namespace next2d::decodequeue
