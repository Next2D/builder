#include "EglContext.h"

#include <EGL/egl.h>

#include <cstdint>
#include <cstdio>

namespace next2d {

namespace {

std::string EglError(const char* what)
{
    char code[16];
    std::snprintf(code, sizeof(code), "0x%04X", static_cast<unsigned>(eglGetError()));
    return std::string(what) + " failed (EGL error " + code + ")";
}

} // namespace

std::unique_ptr<EglContext> EglContext::Create(void* native_display, void* native_window,
                                               int width, int height, std::string* error)
{
    auto fail = [error](const std::string& message) {
        if (error) {
            *error = message;
        }
        return std::unique_ptr<EglContext>();
    };

    // EGLNative*Type はプラットフォームによりポインタにも整数にもなるため、
    // intptr_t を経由した C スタイルキャストで両方に対応する。
    EGLDisplay display = eglGetDisplay(
        native_display ? (EGLNativeDisplayType)(reinterpret_cast<intptr_t>(native_display))
                       : EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        return fail(EglError("eglGetDisplay"));
    }
    if (!eglInitialize(display, nullptr, nullptr)) {
        return fail(EglError("eglInitialize"));
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        return fail(EglError("eglBindAPI"));
    }

    // player は {stencil: true, depth: false, antialias: false, premultipliedAlpha: true} を要求する。
    // マスク (クリップ) がデフォルトフレームバッファのステンシルを使うため stencil 8bit は必須。
    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, native_window ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(display, config_attribs, &config, 1, &count) || count < 1) {
        return fail(EglError("eglChooseConfig"));
    }

    EGLSurface surface = EGL_NO_SURFACE;
    if (native_window) {
        surface = eglCreateWindowSurface(
            display, config, (EGLNativeWindowType)(reinterpret_cast<intptr_t>(native_window)), nullptr);
    } else {
        const EGLint pbuffer_attribs[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
        surface = eglCreatePbufferSurface(display, config, pbuffer_attribs);
    }
    if (surface == EGL_NO_SURFACE) {
        return fail(EglError(native_window ? "eglCreateWindowSurface" : "eglCreatePbufferSurface"));
    }

    const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
    if (context == EGL_NO_CONTEXT) {
        const std::string message = EglError("eglCreateContext");
        eglDestroySurface(display, surface);
        return fail(message);
    }

    std::unique_ptr<EglContext> out(new EglContext());
    out->display_ = display;
    out->surface_ = surface;
    out->context_ = context;

    // 実際のサーフェスサイズを採用する (ウィンドウの場合は要求値と異なり得る)。
    EGLint w = width, h = height;
    eglQuerySurface(display, surface, EGL_WIDTH, &w);
    eglQuerySurface(display, surface, EGL_HEIGHT, &h);
    out->width_ = w;
    out->height_ = h;
    return out;
}

EglContext::~EglContext()
{
    if (display_) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context_) {
            eglDestroyContext(display_, context_);
        }
        if (surface_) {
            eglDestroySurface(display_, surface_);
        }
    }
}

bool EglContext::MakeCurrent()
{
    return eglMakeCurrent(display_, surface_, surface_, context_) == EGL_TRUE;
}

void EglContext::SwapBuffers()
{
    eglSwapBuffers(display_, surface_);
    ++swap_count_;
    needs_present = false;
}

} // namespace next2d
