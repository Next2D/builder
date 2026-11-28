// 各ブラウザ相当バインディングのインストール宣言。
// V8Runtime::Initialize から Context 生成後に InstallGlobalBindings が呼ばれる。
#pragma once

#include <v8.h>
#include <string>

namespace next2d {

class HostContext;

// navigator.language / navigator.languages に出す言語タグを設定する (next2d_host_set_language)。
// 空文字列なら既定 ("en-US")。InstallGlobalBindings より前に呼ぶこと。
void SetNavigatorLanguage(const std::string& language);

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

// canvas.getContext('webgl2') が返す WebGL2RenderingContext を生成する (webgl/WebGL2.cpp)。
// OpenGL ES 3 へ 1 対 1 で変換する。host->gles が無い場合は空の Local を返す。
// NEXT2D_ENABLE_WEBGL2 を定義したビルドでのみリンクされる。
v8::Local<v8::Object> CreateWebGL2Context(v8::Isolate* isolate, HostContext* host);

// worker を専用スレッドで動かす構成用: 呼び出したスレッドが WebGL2 のコンテキストを作った
// (GL のコンテキストを current にした) かどうか。
bool WebGL2CurrentOnThisThread();

// 開発用: 環境変数 NEXT2D_HOST_CAPTURE_DIR の指定時、提示する直前の画面を BMP で書き出す。
// GL のコンテキストを持つスレッドから、提示 (SwapBuffers) の直前に呼ぶ。
class GlesContext;
void CaptureWebGL2Frame(GlesContext* gles);

// 呼び出したスレッドが GL のコンテキストを current にしていれば外す (スレッドの終了時に呼ぶ)。
void ReleaseWebGL2Thread(HostContext* host);

// 環境変数 NEXT2D_HOST_PERF の指定時: 前回の呼び出し以降に WebGL2 のメソッドにかかった時間の
// 要約 (合計と、時間のかかった上位のメソッド) を返し、集計を 0 に戻す。未指定時は空文字列。
std::string TakeWebGL2PerfSummary();

// canvas.getContext('2d') / OffscreenCanvas.getContext('2d') を生成する (Canvas2D.cpp)。
v8::Local<v8::Object> CreateCanvas2DContext(v8::Isolate* isolate, HostContext* host,
                                            int width, int height);

// 2D コンテキストのビットマップを指定サイズで作り直し、描画状態を既定へ戻す (Canvas2D.cpp)。
// canvas.width / canvas.height の代入時に Canvas.cpp が呼ぶ。
void ResizeCanvas2DContext(v8::Local<v8::Object> context2d, int width, int height);

// navigator.getGamepads / gamepad イベント基盤 (GameInput)
void InstallGamepad(v8::Isolate* isolate, v8::Local<v8::Object> global, HostContext* host);

// AudioContext 最小実装 (XAudio2)
void InstallAudio(v8::Isolate* isolate, v8::Local<v8::Object> global, HostContext* host);

// 再生終了した AudioBufferSourceNode へ "ended" を発火する。main.cpp のループから毎フレーム呼ぶ。
void PumpAudioEvents(v8::Isolate* isolate);

// V8 破棄前に呼ぶ。音声の再生追跡リスト (v8::Global) を解放する。
void ShutdownAudioEvents();

// V8 破棄前に呼ぶ。WebGPU テンプレートの v8::Global (static) を解放する。
void ShutdownWebGPU();

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

// TextEncoder/TextDecoder/queueMicrotask 等の JS ポリフィルを評価する (Polyfills.cpp)。
// bootstrap.js はメインのみのため、worker を含む全コンテキストにはこちらで注入する。
void InstallPolyfills(v8::Isolate* isolate, v8::Local<v8::Context> context);

// 上記をまとめてインストールする
// 図形の経路とメッシュのネイティブ実装 (ShapeAccelerator.cpp)。グローバルの __next2dShape に置く。
void InstallShapeAccelerator(v8::Isolate* isolate, v8::Local<v8::Object> global);

void InstallGlobalBindings(v8::Isolate* isolate, v8::Local<v8::Object> global, HostContext* host);

} // namespace next2d
