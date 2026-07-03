# =============================================================================
# Dawn (Google WebGPU 実装) を取得してビルドする。
# D3D12 バックエンドのみを有効化し、Xbox(GDK)/PC(GDK) で利用する。
#
# 注意:
#   - submodule はクローンしない (GIT_SUBMODULES "")。SwiftShader→LLVM まで再帰して
#     数 GB のクローン + Windows の MAX_PATH 超過 ("Filename too long") で失敗するため。
#     依存は Dawn 公式の DAWN_FETCH_DEPENDENCIES (python スクリプト) が必要分だけ取得する。
#   - DAWN_FETCH_DEPENDENCIES には python3 が PATH に必要。
#   - 初回構成はネットワークとビルド時間を要する。CI ではキャッシュ推奨。
#   - DAWN_TAG はバインディング (bindings/webgpu/*) が使う API 世代と一致させること:
#     wgpu::Limits / ShaderSourceWGSL / SurfaceSourceWindowsHWND / OptionalBool
#     (いずれも 2025 以降の命名。古いタグでは SupportedLimits 等になりビルド不可)
# =============================================================================
include(FetchContent)

set(DAWN_TAG "v20260402.171122" CACHE STRING "Dawn のチェックアウトタグ (github.com/google/dawn)")

# Dawn のビルドオプション: 必要なものだけを有効化
set(DAWN_FETCH_DEPENDENCIES     ON  CACHE BOOL "" FORCE)
set(DAWN_ENABLE_D3D12           ON  CACHE BOOL "" FORCE)
set(DAWN_ENABLE_D3D11           OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_VULKAN          OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_METAL           OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_OPENGLES        OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_DESKTOP_GL      OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_NULL            OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_SAMPLES          OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_TESTS            OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_CMD_TOOLS        OFF CACHE BOOL "" FORCE)

# webgpu_dawn: webgpu.h C-API を提供する統合ライブラリ (バインディング層はこれを叩く)。
# 2025 以降の Dawn では SHARED / STATIC / OFF の3値。STATIC で exe に静的リンクし
# 追加 DLL の同梱を不要にする (BUILD_SHARED_LIBS=OFF が前提)。
set(DAWN_BUILD_MONOLITHIC_LIBRARY STATIC CACHE STRING "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    dawn
    # GitHub ミラーの方が CI から高速・安定 (googlesource はレート制限がきつい)
    GIT_REPOSITORY https://github.com/google/dawn.git
    GIT_TAG        ${DAWN_TAG}
    GIT_SHALLOW    TRUE
    # submodule は取得しない (DAWN_FETCH_DEPENDENCIES が必要分を取得する)
    GIT_SUBMODULES ""
    GIT_SUBMODULES_RECURSE FALSE
)

FetchContent_MakeAvailable(dawn)

# webgpu_dawn ターゲットに dawn::webgpu_dawn エイリアスが無いバージョン向けの保険
if(NOT TARGET dawn::webgpu_dawn AND TARGET webgpu_dawn)
    add_library(dawn::webgpu_dawn ALIAS webgpu_dawn)
endif()
