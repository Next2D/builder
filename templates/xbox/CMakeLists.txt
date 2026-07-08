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

if(MSVC)
    # MSVC は /Zc:__cplusplus を渡さないと __cplusplus を 199711L のまま報告し
    # (/std:c++20 を指定していても)、v8config.h の C++20 チェックで停止する。
    add_compile_options(/Zc:__cplusplus)
endif()

# CRT は静的 (/MT, Debug は /MTd) に統一する。
# prebuilt V8 monolith は GN の既定で静的 CRT ビルドのため、ホスト側が既定の /MD だと
# libcpmt.lib と msvcprt.lib が衝突して LNK2005/LNK2038 でリンクに失敗する。
# ここで設定することで FetchContent の Dawn も同じ CRT でビルドされる。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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
# GDK の解決 (2通り)
#   A) インストール済み GDK: -A Gaming.Desktop.x64 / Gaming.Xbox.* でビルド。
#      VS 統合が include/lib を自動供給するため追加設定は不要。
#   B) 展開レイアウト (インストーラ不要・CI 向け): NuGet の Microsoft.GDK.Windows を
#      展開し、-A x64 + GDK_ROOT (cmake 変数 or 環境変数) で指定する。
#      GDK_ROOT は <package>/native/<edition>/windows を指す (include/, lib/x64/, bin/x64/)。
# -----------------------------------------------------------------------------
string(FIND "${CMAKE_GENERATOR_PLATFORM}" "Gaming." _is_gaming_platform)
if(_is_gaming_platform LESS 0)
    if(NOT DEFINED GDK_ROOT)
        set(GDK_ROOT "$ENV{GDK_ROOT}")
    endif()
    if(NOT GDK_ROOT OR NOT EXISTS "${GDK_ROOT}/include/XGameRuntime.h")
        message(FATAL_ERROR
            "GDK が見つかりません。次のいずれかで指定してください:\n"
            "  1) GDK をインストールして -A Gaming.Desktop.x64 でビルド\n"
            "  2) NuGet の Microsoft.GDK.Windows を展開し、-A x64 で\n"
            "     -D GDK_ROOT=<package>/native/<edition>/windows (または環境変数 GDK_ROOT) を指定")
    endif()
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

# 埋め込みアセット: builder の buildXbox が assets.pak と assets.rc を必ず生成し、
# RCDATA "N2DASSETS" として exe にリンクする (ゲーム JS/HTML は平文で隣に置かない)。
# 埋め込みは必須。assets.rc が無いのは builder を経由せず cmake を直接叩いた
# ホスト単体ビルド (ゲーム資材なし) のみで、その場合はゲームは起動しない。
set(NEXT2D_ASSETS_RC "${CMAKE_CURRENT_SOURCE_DIR}/assets.rc")
if(EXISTS "${NEXT2D_ASSETS_RC}")
    enable_language(RC)
    list(APPEND NEXT2D_HOST_SOURCES "${NEXT2D_ASSETS_RC}")
    message(STATUS "Next2D: embedding assets from ${NEXT2D_ASSETS_RC}")
else()
    message(WARNING "Next2D: assets.rc not found - host-only build (no game assets). Run @next2d/builder to embed.")
endif()

add_executable(Next2DXboxHost WIN32 ${NEXT2D_HOST_SOURCES})

target_include_directories(Next2DXboxHost PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# 展開レイアウト (GDK_ROOT) 使用時: include/lib を明示指定し、実行時 DLL を並べる
if(_is_gaming_platform LESS 0)
    target_include_directories(Next2DXboxHost PRIVATE "${GDK_ROOT}/include")
    target_link_directories(Next2DXboxHost PRIVATE "${GDK_ROOT}/lib/x64")
    add_custom_command(TARGET Next2DXboxHost POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${GDK_ROOT}/bin/x64/xgameruntime.dll"
            "$<TARGET_FILE_DIR:Next2DXboxHost>"
        COMMENT "Staging xgameruntime.dll next to the executable")
endif()

target_link_libraries(Next2DXboxHost PRIVATE
    v8::v8
    dawn::webgpu_dawn
)

if(MSVC)
    # CRT は静的 (/MT) に統一している (V8 monolith が静的 CRT 固定のため)。
    # 一方 GDK の xgameruntime.lib 等は /MD ビルドで /DEFAULTLIB:msvcprt を
    # 持ち込み LNK2005 (多重定義) を起こすので、動的 C++ ランタイムを除外して
    # 静的側 (libcpmt) に一本化する。
    target_link_options(Next2DXboxHost PRIVATE
        /NODEFAULTLIB:msvcprt.lib
        /NODEFAULTLIB:msvcprtd.lib
    )

    # Release でも PDB を生成する (クラッシュダンプ解析でスタックを名前解決するため)
    target_compile_options(Next2DXboxHost PRIVATE /Zi)
    target_link_options(Next2DXboxHost PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()

# webgpu_dawn は SHARED (DLL)。実行に必要なため exe の隣へステージする
if(TARGET webgpu_dawn)
    add_custom_command(TARGET Next2DXboxHost POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:webgpu_dawn>"
            "$<TARGET_FILE_DIR:Next2DXboxHost>"
        COMMENT "Staging webgpu_dawn.dll next to the executable")
endif()

# WebGPU / D3D12 / GameInput / WIC / XAudio2 / GDK
target_link_libraries(Next2DXboxHost PRIVATE
    d3d12
    dxgi
    dxguid
    windowscodecs   # WIC: 画像デコード
    dbghelp         # クラッシュハンドラのスタックトレース (StackWalk64/SymFromAddr)
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

# V8 のプラットフォーム要件。
# prebuilt V8 (build-v8.yml) のビルドフラグと一致させること:
#   V8_COMPRESS_POINTERS <-> v8_enable_pointer_compression=true
#   sandbox は無効 (libc++ ハードニング必須のため use_custom_libcxx=false と両立しない)
#   → V8_ENABLE_SANDBOX は定義しない
target_compile_definitions(Next2DXboxHost PRIVATE
    V8_COMPRESS_POINTERS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)

# -----------------------------------------------------------------------------
# ランタイム資材のステージング
#   ゲーム JS/HTML/アセット (assets/app) とホストスクリプト (js/bootstrap.js) は
#   builder が assets.pak にまとめて exe 内 RCDATA へ**必ず**埋め込む
#   (非埋め込みの選択肢は無い。平文 JS/HTML を配布物に残さないのが目的)。
#   したがって exe 隣へは assets/ も js/ もコピーしない。
#   MicrosoftGame.config (GDK タイトル設定) だけを exe の隣へステージングする。
# -----------------------------------------------------------------------------
add_custom_command(TARGET Next2DXboxHost POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/MicrosoftGame.config"
        "$<TARGET_FILE_DIR:Next2DXboxHost>/MicrosoftGame.config"
    COMMENT "Staging MicrosoftGame.config (assets/JS embedded in exe)"
)
