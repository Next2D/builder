// HostContext: C++ 側のサブシステムを束ね、V8 バインディングから参照できるようにする。
//
// Isolate の data slot 0 に本ポインタを格納し、任意のバインディングコールバックから
// HostContext::From(isolate) で取り出す。これにより各バインディングは EventLoop /
// DawnContext / AssetLoader / GamepadManager 等へアクセスできる。
#pragma once

#include <v8.h>

namespace next2d {

class EventLoop;
class DawnContext;
class AssetLoader;
class GamepadManager;
class AudioEngine;
class WorkerRuntime;

// Isolate data slot index (V8Runtime が SetData で登録)
constexpr uint32_t kHostContextSlot = 0;

// グラフィックスバックエンド種別。
// Xbox は WebGPU(Dawn/D3D12)。Nintendo Switch 移植時は WebGL2(GL) を選ぶ。
// player は navigator.gpu の有無で自動的に webgpu / webgl2 経路を切り替える。
enum class GraphicsBackend {
    WebGPU,
    WebGL2,
};

class HostContext {
public:
    HostContext() = default;

    EventLoop*       event_loop = nullptr;
    DawnContext*     gpu        = nullptr;
    AssetLoader*     assets     = nullptr;
    GamepadManager*  gamepad    = nullptr;
    AudioEngine*     audio      = nullptr;
    WorkerRuntime*   workers    = nullptr;

    // 使用するグラフィックスバックエンド (既定は Xbox=WebGPU)
    GraphicsBackend  backend    = GraphicsBackend::WebGPU;

    // 入力イベント配送先の主要 canvas (Canvas.cpp が最後に生成したものを記録)
    v8::Global<v8::Object> main_canvas;

    // 現在配送中の入力種別 (診断用)。0=その他, 1=pointermove, 2=pointerdown, 3=pointerup。
    // FireEvent が設定し、Canvas2D の isPointInPath 診断が種別ごとにヒット結果を記録する。
    int input_kind = 0;

    // アプリのビューポート論理サイズ (canvas.width/height の初期値)
    int viewport_width  = 1920;
    int viewport_height = 1080;
    double device_pixel_ratio = 1.0;

    static HostContext* From(v8::Isolate* isolate)
    {
        return static_cast<HostContext*>(isolate->GetData(kHostContextSlot));
    }
};

} // namespace next2d
