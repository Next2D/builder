# =============================================================================
# Next2D Xbox (GDK native) host
#
# V8 に Next2D の JavaScript を載せ、Dawn(WebGPU/D3D12) で描画する
# ネイティブタイトルをビルドする CMake プロジェクト。
#
# 対象トリプレット (builder が -A に渡す):
#   - Gaming.Xbox.Scarlett.x64  : Xbox Series X|S
#   - Gaming.Xbox.XboxOne.x64   : Xbox One
#   - Gaming.Desktop.x64        : PC(GDK) / ローカル検証
#
# 必須:
#   - Windows + Visual Studio 2022 (v143)
#   - Microsoft GDK (June 2024 以降)
#   - V8 の prebuilt ライブラリ (V8_ROOT で指定)
#   - Dawn (FetchContent で取得・ビルド)
# =============================================================================
cmake_minimum_required(VERSION 3.26)

project(Next2DXboxHost LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# GDK target platform (Gaming.Xbox.* / Gaming.Desktop.*) の判定
if(NOT DEFINED CMAKE_GENERATOR_PLATFORM)
    message(WARNING "CMAKE_GENERATOR_PLATFORM が未設定です。builder は -A Gaming.Xbox.Scarlett.x64 等を渡します。")
endif()

string(FIND "${CMAKE_GENERATOR_PLATFORM}" "Gaming.Xbox" _is_console)
if(_is_console GREATER_EQUAL 0)
    set(NEXT2D_IS_CONSOLE ON)
else()
    set(NEXT2D_IS_CONSOLE OFF)
endif()

# -----------------------------------------------------------------------------
# 依存: Dawn (WebGPU 実装 / D3D12 バックエンド)
# -----------------------------------------------------------------------------
include(cmake/FetchDawn.cmake)

# -----------------------------------------------------------------------------
# 依存: V8 (prebuilt)
#   V8 はソースからのビルドが重いため prebuilt を前提とする。
#   cmake -D V8_ROOT=<path> で monolith ビルド (v8_monolith.lib) を指す。
# -----------------------------------------------------------------------------
include(cmake/FindV8.cmake)

# -----------------------------------------------------------------------------
# ソース
# -----------------------------------------------------------------------------
file(GLOB_RECURSE NEXT2D_HOST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

add_executable(Next2DXboxHost WIN32 ${NEXT2D_HOST_SOURCES})

target_include_directories(Next2DXboxHost PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(Next2DXboxHost PRIVATE
    v8::v8
    dawn::webgpu_dawn
)

# WebGPU / D3D12 / GameInput / WIC / XAudio2 / GDK
target_link_libraries(Next2DXboxHost PRIVATE
    d3d12
    dxgi
    dxguid
    windowscodecs   # WIC: 画像デコード
)

if(NEXT2D_IS_CONSOLE)
    # コンソール用ライブラリ (GDK が提供)
    target_link_libraries(Next2DXboxHost PRIVATE
        xgameruntime
        gameinput
        xaudio2_9      # コンソールは XAudio2.9
    )
    target_compile_definitions(Next2DXboxHost PRIVATE NEXT2D_XBOX_CONSOLE=1)
else()
    # PC(GDK) / ローカル検証
    target_link_libraries(Next2DXboxHost PRIVATE
        xgameruntime
        gameinput
        xaudio2
    )
    target_compile_definitions(Next2DXboxHost PRIVATE NEXT2D_XBOX_CONSOLE=0)
endif()

# V8 のプラットフォーム要件
target_compile_definitions(Next2DXboxHost PRIVATE
    V8_COMPRESS_POINTERS
    V8_ENABLE_SANDBOX
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)

# -----------------------------------------------------------------------------
# ランタイム資材のステージング
#   builder が assets/app に JS/HTML/アセットを配置する。
#   js/bootstrap.js はブラウザ相当グローバルを整える起動スクリプト。
# -----------------------------------------------------------------------------
add_custom_command(TARGET Next2DXboxHost POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:Next2DXboxHost>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/js"
        "$<TARGET_FILE_DIR:Next2DXboxHost>/js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/MicrosoftGame.config"
        "$<TARGET_FILE_DIR:Next2DXboxHost>/MicrosoftGame.config"
    COMMENT "Staging Next2D assets, bootstrap JS and MicrosoftGame.config"
)
