// WebGPU バインディング共通基盤。
//
// wgpu:: (Dawn C++) ハンドルを V8 オブジェクトへラップする汎用機構と、
// JS ディスクリプタ(プレーンオブジェクト)を wgpu 構造体へ変換するヘルパーを提供する。
//
// 【重要・実装範囲について】
// WebGPU の IDL 全面(約40型・数百メソッド・全ディスクリプタ)を手書きするのは膨大なため、
// 本バインディングは Next2D の WebGPU レンダラが使用するコア経路
// (adapter/device/queue/buffer/texture/sampler/shader/bindGroup(Layout)/
//  pipelineLayout/renderPipeline/commandEncoder/renderPass/canvasContext) を実装する。
// 追加のメソッド/ディスクリプタ項目は各 Create* 付近の «EXTEND» コメントに沿って拡張する。
#pragma once

#include <webgpu/webgpu_cpp.h>
#include <v8.h>

#include "v8/V8Util.h"
#include "v8/WeakHandle.h"

#include <memory>
#include <string>

namespace next2d::webgpu {

// wgpu ハンドルの寿命を JS オブジェクトに紐づけるためのホルダ。
template <typename T>
struct Holder {
    T handle;
};

// wgpu ハンドル T を、指定 ObjectTemplate から生成したオブジェクトにラップする。
// GC 時の解放は AttachWeak (第一パスで Global を Reset する規約に準拠) に委ねる。
template <typename T>
v8::Local<v8::Object> Wrap(v8::Isolate* isolate,
                           v8::Local<v8::ObjectTemplate> tmpl,
                           T handle)
{
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    v8::Local<v8::Object> obj = tmpl->NewInstance(ctx).ToLocalChecked();
    auto* holder = new Holder<T>{std::move(handle)};
    obj->SetInternalField(0, v8::External::New(isolate, holder));
    v8util::AttachWeak(isolate, obj, holder);
    return obj;
}

// ラップされたオブジェクトから wgpu ハンドルを取り出す。
template <typename T>
T& Unwrap(v8::Local<v8::Object> obj)
{
    auto* holder = static_cast<Holder<T>*>(
        obj->GetInternalField(0).As<v8::External>()->Value());
    return holder->handle;
}

// 内部フィールド 1 個の ObjectTemplate を作る。
inline v8::Local<v8::ObjectTemplate> HandleTemplate(v8::Isolate* isolate)
{
    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(isolate);
    tmpl->SetInternalFieldCount(1);
    return tmpl;
}

// --- JS オブジェクト読み取りヘルパー ------------------------------------
inline v8::Local<v8::Value> Prop(v8::Isolate* isolate, v8::Local<v8::Object> obj,
                                 const char* key)
{
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    v8::Local<v8::Value> v;
    if (obj->Get(ctx, v8util::Str(isolate, key)).ToLocal(&v)) {
        return v;
    }
    return v8::Undefined(isolate);
}

inline bool HasProp(v8::Isolate* isolate, v8::Local<v8::Object> obj, const char* key)
{
    v8::Local<v8::Value> v = Prop(isolate, obj, key);
    return !v->IsUndefined() && !v->IsNull();
}

inline uint32_t U32(v8::Isolate* isolate, v8::Local<v8::Object> obj,
                    const char* key, uint32_t fallback = 0)
{
    v8::Local<v8::Value> v = Prop(isolate, obj, key);
    return v->IsNumber() ? static_cast<uint32_t>(v.As<v8::Number>()->Value()) : fallback;
}

inline uint64_t U64(v8::Isolate* isolate, v8::Local<v8::Object> obj,
                    const char* key, uint64_t fallback = 0)
{
    v8::Local<v8::Value> v = Prop(isolate, obj, key);
    return v->IsNumber() ? static_cast<uint64_t>(v.As<v8::Number>()->Value()) : fallback;
}

inline double F64(v8::Isolate* isolate, v8::Local<v8::Object> obj,
                  const char* key, double fallback = 0.0)
{
    v8::Local<v8::Value> v = Prop(isolate, obj, key);
    return v->IsNumber() ? v.As<v8::Number>()->Value() : fallback;
}

inline bool Bool(v8::Isolate* isolate, v8::Local<v8::Object> obj,
                 const char* key, bool fallback = false)
{
    v8::Local<v8::Value> v = Prop(isolate, obj, key);
    return v->IsBoolean() ? v.As<v8::Boolean>()->Value() : fallback;
}

inline std::string Str(v8::Isolate* isolate, v8::Local<v8::Object> obj, const char* key)
{
    return v8util::ToStdString(isolate, Prop(isolate, obj, key));
}

} // namespace next2d::webgpu
