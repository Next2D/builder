// 各ブラウザ相当バインディングのインストール宣言。
// V8Runtime::Initialize から Context 生成後に InstallGlobalBindings が呼ばれる。
#pragma once

#include <v8.h>
#include <string>

namespace next2d {

class HostContext;

// console.* (log/info/warn/error/debug)
void InstallConsole(v8::Isolate* isolate, v8::Local<v8::Object> global);

// setTimeout / setInterval / clearTimeout / clearInterval /
// requestAnimationFrame / cancelAnimationFrame / performance.now
void InstallTimers(v8::Isolate* isolate, v8::Local<v8::Object> global, HostContext* host);

// fetch / XMLHttpRequest 最小実装 (assets/app からのローカル読み込み)
void InstallFetch(v8::Isolate* isolate, v8::Local<v8::Object> global, HostContext* host);

// Image / createImageBitmap (WIC デコード)
void InstallImage(v8::Isolate* isolate, v8::Local<v8::Object> global, HostContext* host);

// navigator.gpu (WebGPU -> Dawn)。backend==WebGPU のときのみ navigator.gpu を公開する。
void InstallWebGPU(v8::Isolate* isolate, v8::Local<v8::Object> global, HostContext* host);

// canvas.getContext('webgpu') が返す GPUCanvasContext を生成する (WebGPU.cpp)。
v8::Local<v8::Object> CreateWebGPUCanvasContext(v8::Isolate* isolate, HostContext* host);

// canvas.getContext('2d') / OffscreenCanvas.getContext('2d') を生成する (Canvas2D.cpp)。
v8::Local<v8::Object> CreateCanvas2DContext(v8::Isolate* isolate, HostContext* host,
                                            int width, int height);

// navigator.getGamepads / gamepad イベント基盤 (GameInput)
void InstallGamepad(v8::Isolate* isolate, v8::Local<v8::Object> global, HostContext* host);

// AudioContext 最小実装 (XAudio2)
void InstallAudio(v8::Isolate* isolate, v8::Local<v8::Object> global, HostContext* host);

// 再生終了した AudioBufferSourceNode へ "ended" を発火する。main.cpp のループから毎フレーム呼ぶ。
void PumpAudioEvents(v8::Isolate* isolate);

// window / document / navigator / screen 等の DOM 相当スタブと canvas 生成
void InstallDomShims(v8::Isolate* isolate, v8::Local<v8::Object> global, HostContext* host);

// HTMLCanvasElement 相当を生成する。getContext は '2d'/'webgpu'/'webgl2' を
// backend に応じてディスパッチし、transferControlToOffscreen も備える (Canvas.cpp)。
v8::Local<v8::Object> CreateCanvasElement(v8::Isolate* isolate, HostContext* host,
                                          int width, int height);

// OffscreenCanvas を生成する (Canvas.cpp)。worker/メッシュ生成/テキストで使用。
v8::Local<v8::Object> CreateOffscreenCanvas(v8::Isolate* isolate, HostContext* host,
                                            int width, int height);

// window/worker グローバルへ Worker コンストラクタ等を設置する (Worker/WorkerRuntime)。
void InstallWorker(v8::Isolate* isolate, v8::Local<v8::Object> global, HostContext* host);

// XMLHttpRequest / Blob / URL.createObjectURL / ImageData (Network.cpp)
void InstallNetwork(v8::Isolate* isolate, v8::Local<v8::Object> global, HostContext* host);

// HTMLVideoElement を生成する (Video.cpp)。document.createElement("video") から使用。
v8::Local<v8::Object> CreateVideoElement(v8::Isolate* isolate, HostContext* host);

// video 要素の現在フレーム RGBA を取得する (Video.cpp)。GetImageSourcePixels から使用。
bool GetVideoFramePixels(v8::Isolate* isolate, v8::Local<v8::Object> obj,
                         const uint8_t** out_rgba, uint32_t* out_w, uint32_t* out_h);

// blob: URL のバイト列をテキストとして取得する (Network.cpp)。
bool ResolveObjectURL(const std::string& url, std::string* out_text);

// 上記をまとめてインストールする
void InstallGlobalBindings(v8::Isolate* isolate, v8::Local<v8::Object> global, HostContext* host);

} // namespace next2d
