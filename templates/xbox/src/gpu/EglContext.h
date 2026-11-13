// EglContext: EGL で OpenGL ES 3 コンテキストを作る GlesContext 実装。
//
// EGL は Khronos の公開仕様で、ANGLE (Windows / macOS / Linux) などが提供する。
// デスクトップでの WebGL2 バインディング検証 (tests/webgl2) と、EGL を備える環境の
// ホストで使う。native_window が nullptr のときはオフスクリーン (pbuffer) になる。
#pragma once

#include "GlesContext.h"

#include <memory>
#include <string>

namespace next2d {

class EglContext final : public GlesContext {
public:
    // native_display: EGLNativeDisplayType 相当 (nullptr = EGL_DEFAULT_DISPLAY)。
    // native_window : EGLNativeWindowType 相当 (nullptr = width x height の pbuffer)。
    // 失敗時は nullptr を返し、error に理由を入れる。
    static std::unique_ptr<EglContext> Create(void* native_display, void* native_window,
                                              int width, int height, std::string* error);
    ~EglContext() override;

    bool MakeCurrent() override;
    void SwapBuffers() override;
    int width() const override { return width_; }
    int height() const override { return height_; }

    int swap_count() const { return swap_count_; }

private:
    EglContext() = default;

    void* display_ = nullptr;   // EGLDisplay
    void* surface_ = nullptr;   // EGLSurface
    void* context_ = nullptr;   // EGLContext
    int width_ = 0;
    int height_ = 0;
    int swap_count_ = 0;
};

} // namespace next2d
