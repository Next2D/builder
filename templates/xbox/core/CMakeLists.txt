# =============================================================================
# Next2D host core library
#
# ホストの共通部分 (V8 + ブラウザ相当バインディング + WebGL2 → OpenGL ES 3) を
# 1 つの共有ライブラリにまとめる。ウィンドウ / GL コンテキスト / 入力はライブラリの
# 外 (埋め込み側) が持ち、C の窓口 (src/core/next2d_host_core.h) でやり取りする。
#
#   - 描画は WebGL2 のみ (Dawn / WebGPU は含めない)
#   - GL は関数テーブルモード (埋め込み側が関数アドレスを解決する)
#   - ゲームパッドは外部入力モード (埋め込み側が状態を書き込む)
#   - GDK には依存しない
#
# 構成例:
#   cmake -S core -B build-core -A x64 -D V8_ROOT=<prebuilt V8>
#   cmake --build build-core --config Release
# =============================================================================
cmake_minimum_required(VERSION 3.26)

project(Next2DHostCore LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NEXT2D_HOST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

if(MSVC)
    add_compile_options(/Zc:__cplusplus /utf-8)
    # prebuilt V8 monolith が静的 CRT のため、コアも静的 CRT に揃える。
    # 境界は C の窓口だけなので、埋め込み側の CRT は何でもよい。
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

list(APPEND CMAKE_MODULE_PATH "${NEXT2D_HOST_ROOT}/cmake")
include("${NEXT2D_HOST_ROOT}/cmake/FindV8.cmake")

file(GLOB_RECURSE NEXT2D_CORE_SOURCES
    "${NEXT2D_HOST_ROOT}/src/*.cpp"
    "${NEXT2D_HOST_ROOT}/src/*.h"
)
# 実行ファイル専用 / WebGPU(Dawn) / EGL 直結の実装は含めない
list(FILTER NEXT2D_CORE_SOURCES EXCLUDE REGEX
    "/src/main\\.cpp$|/src/bindings/webgpu/|/src/gpu/DawnContext\\.(cpp|h)$|/src/gpu/EglContext\\.(cpp|h)$")

add_library(next2d_host_core SHARED ${NEXT2D_CORE_SOURCES})

target_include_directories(next2d_host_core PRIVATE
    "${NEXT2D_HOST_ROOT}/src"
    "${NEXT2D_HOST_ROOT}"                      # third_party/ (stb_image, dr_mp3 等)
    "${NEXT2D_HOST_ROOT}/third_party/khronos"  # GLES3/gl3.h (型と定数のみ使用)
)

target_compile_definitions(next2d_host_core PRIVATE
    NEXT2D_HOST_CORE_BUILD
    NEXT2D_ENABLE_WEBGL2=1
    NEXT2D_ENABLE_WEBGPU=0
    NEXT2D_GL_FUNCTION_TABLE=1
    NEXT2D_GAMEPAD_EXTERNAL=1
    # OS 固有のデコーダ / テキスト描画 / 動画を使わない可搬な経路を選ぶ
    # (画像 = stb_image、音声 = dr_mp3 / dr_wav / stb_vorbis、文字 = stb_truetype)。
    NEXT2D_XBOX_CONSOLE=1
    V8_COMPRESS_POINTERS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)

target_link_libraries(next2d_host_core PRIVATE v8::v8)

if(WIN32)
    target_link_libraries(next2d_host_core PRIVATE xaudio2 ole32)
endif()

if(MSVC)
    target_compile_options(next2d_host_core PRIVATE /Zi /bigobj)
    target_link_options(next2d_host_core PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()
