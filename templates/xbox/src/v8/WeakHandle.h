// SetWeak (GC 連動の解放) の正しい運用ヘルパー。
//
// V8 の規約では、第一パスの weak callback 内で対応する v8::Global を必ず
// Reset しなければならない。違反すると GC 時に fatal
// 「Check failed: Handle not reset in first callback.」で落ちる。
// payload と Global を束ねて所有し、GC で Reset + delete の両方を行う。
#pragma once

#include <v8.h>

namespace next2d::v8util {

template <typename T>
class WeakHandle {
public:
    static void Attach(v8::Isolate* isolate, v8::Local<v8::Object> obj, T* payload)
    {
        auto* w = new WeakHandle<T>(payload);
        w->handle_.Reset(isolate, obj);
        w->handle_.SetWeak(w, &WeakHandle::OnGC, v8::WeakCallbackType::kParameter);
    }

private:
    explicit WeakHandle(T* payload) : payload_(payload) {}

    static void OnGC(const v8::WeakCallbackInfo<WeakHandle<T>>& info)
    {
        WeakHandle<T>* w = info.GetParameter();
        w->handle_.Reset();   // 第一パスでの Reset (V8 の必須要件)
        delete w->payload_;
        delete w;
    }

    v8::Global<v8::Object> handle_;
    T* payload_;
};

// オブジェクトが GC されたときに `delete payload` する弱参照を張る。
template <typename T>
inline void AttachWeak(v8::Isolate* isolate, v8::Local<v8::Object> obj, T* payload)
{
    WeakHandle<T>::Attach(isolate, obj, payload);
}

} // namespace next2d::v8util
