#include "NativeThread.h"

#include <memory>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#endif

namespace next2d {

namespace {

#ifdef _WIN32
unsigned __stdcall ThreadEntry(void* arg)
{
    std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()>*>(arg));
    (*fn)();
    return 0;
}
#else
void* ThreadEntry(void* arg)
{
    std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()>*>(arg));
    (*fn)();
    return nullptr;
}
#endif

} // namespace

bool NativeThread::Start(std::function<void()> fn, size_t stack_bytes)
{
    if (handle_) {
        return false;
    }
    auto holder = std::make_unique<std::function<void()>>(std::move(fn));
#ifdef _WIN32
    const uintptr_t thread = _beginthreadex(
        nullptr, static_cast<unsigned>(stack_bytes), ThreadEntry, holder.get(), 0, nullptr);
    if (thread == 0) {
        return false;
    }
    handle_ = reinterpret_cast<void*>(thread);
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_bytes);
    auto* thread = new pthread_t();
    const int result = pthread_create(thread, &attr, ThreadEntry, holder.get());
    pthread_attr_destroy(&attr);
    if (result != 0) {
        delete thread;
        return false;
    }
    handle_ = thread;
#endif
    holder.release();   // 以後はスレッド側が持つ
    return true;
}

void NativeThread::Join()
{
    if (!handle_) {
        return;
    }
#ifdef _WIN32
    WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
    CloseHandle(static_cast<HANDLE>(handle_));
#else
    auto* thread = static_cast<pthread_t*>(handle_);
    pthread_join(*thread, nullptr);
    delete thread;
#endif
    handle_ = nullptr;
}

} // namespace next2d
