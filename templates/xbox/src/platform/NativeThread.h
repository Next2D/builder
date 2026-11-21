// NativeThread: スタックの大きさを指定して作るスレッド。
//
// std::thread はスタックの大きさを指定できない。V8 の isolate を動かすスレッドには
// 1 MB を超えるスタックが要る (V8 は、isolate に入った時点のスタック位置から約 1 MB を
// 使えるものとして、JS の再帰の上限を決める)。既定のスタックがそれより小さい環境では、
// 上限に達する前にスタックがあふれる。
#pragma once

#include <cstddef>
#include <functional>

namespace next2d {

class NativeThread {
public:
    NativeThread() = default;
    ~NativeThread() { Join(); }

    NativeThread(const NativeThread&) = delete;
    NativeThread& operator=(const NativeThread&) = delete;

    // fn を新しいスレッドで実行する。作れなければ false。
    bool Start(std::function<void()> fn, size_t stack_bytes);

    // スレッドの終了を待つ (始まっていなければ何もしない)。
    void Join();

    bool started() const { return handle_ != nullptr; }

private:
    void* handle_ = nullptr;
};

} // namespace next2d
