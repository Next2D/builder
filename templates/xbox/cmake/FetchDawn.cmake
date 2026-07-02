# =============================================================================
# Dawn (Google WebGPU 実装) を取得してビルドする。
# D3D12 バックエンドのみを有効化し、Xbox(GDK)/PC(GDK) で利用する。
#
# 注意:
#   - Dawn のビルドには depot_tools 由来の一部が不要になる CMake パスを使用する。
#   - 初回構成はネットワークとビルド時間を要する。CI ではキャッシュ推奨。
#   - DAWN_TAG は動作確認済みタグに固定すること (chromium ブランチに追従)。
# =============================================================================
include(FetchContent)

set(DAWN_TAG "chromium/6478" CACHE STRING "Dawn のチェックアウトタグ")

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

# webgpu_dawn: webgpu.h C-API を提供する統合ライブラリ (バインディング層はこれを叩く)
set(DAWN_BUILD_MONOLITHIC_LIBRARY ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    dawn
    GIT_REPOSITORY https://dawn.googlesource.com/dawn
    GIT_TAG        ${DAWN_TAG}
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(dawn)

# webgpu_dawn ターゲットに dawn::webgpu_dawn エイリアスが無いバージョン向けの保険
if(NOT TARGET dawn::webgpu_dawn AND TARGET webgpu_dawn)
    add_library(dawn::webgpu_dawn ALIAS webgpu_dawn)
endif()
