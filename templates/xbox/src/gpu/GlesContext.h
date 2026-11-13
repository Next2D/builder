// GlesContext: OpenGL ES 3 コンテキストとデフォルトフレームバッファの抽象。
//
// WebGL2 バインディング (bindings/webgl/WebGL2.cpp) は OpenGL ES 3 の関数だけを呼び、
// コンテキスト生成・提示 (swap) はこのインターフェース越しに行う。これにより
// バインディング本体はプラットフォーム非依存に保たれ、実装側だけを差し替えられる
// (デスクトップ検証は EGL + ANGLE、他プラットフォームは各自の実装を用意する)。
//
// スレッドモデル: ホストは単一スレッド (worker も同一 Isolate の協調実行) なので、
// コンテキストは起動時に 1 度 MakeCurrent したら current のまま使う。
#pragma once

#include <cstdint>

namespace next2d {

class GlesContext {
public:
    virtual ~GlesContext() = default;

    // このスレッドでコンテキストを current にする。成功時 true。
    virtual bool MakeCurrent() = 0;

    // デフォルトフレームバッファを画面へ提示する。
    virtual void SwapBuffers() = 0;

    // デフォルトフレームバッファのピクセルサイズ (drawingBufferWidth/Height)。
    virtual int width() const = 0;
    virtual int height() const = 0;

    // デフォルトフレームバッファの GL 名。bindFramebuffer(target, null) の束縛先。
    // 大半の環境は 0 だが、0 以外を画面用 FBO とする環境のために差し替え可能にしている。
    virtual uint32_t default_framebuffer() const { return 0; }

    // デフォルトフレームバッファへ描画が行われたら WebGL2 バインディングが立てる。
    // メインループはこれを見て SwapBuffers し、フラグを下ろす
    // (描画の無いフレームで swap すると未定義内容が提示されるため)。
    bool needs_present = false;
};

} // namespace next2d
