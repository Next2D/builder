// DOM EventTarget 相当の最小実装。
// addEventListener で登録した関数を __listeners[type] に保持し、C++ から DispatchEvent で発火する。
// main.cpp の WndProc が Win32 入力を PointerEvent/KeyboardEvent/WheelEvent へ変換して使う。
#pragma once

#include <v8.h>

namespace next2d {

// target に addEventListener / removeEventListener / dispatchEvent を設置する。
void InstallEventTarget(v8::Isolate* isolate, v8::Local<v8::Object> target);

// target の on<type> と __listeners[type] を発火する。event は {type, ...} を持つ JS オブジェクト。
void DispatchEvent(v8::Isolate* isolate, v8::Local<v8::Object> target,
                   v8::Local<v8::Object> event);

} // namespace next2d
