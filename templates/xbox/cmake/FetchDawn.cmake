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
# 2025 以降の Dawn では SHARED / STATIC / OFF の3値。
# SHARED (DLL) を選ぶ理由: V8 monolith と Dawn は双方が abseil を静的に内蔵しており、
# STATIC だと raw_hash_set 等の同名シンボルが LNK2005 で衝突する (しかも absl の
# バージョンが異なるため /FORCE:MULTIPLE は実行時破壊のリスクがある)。
# DLL にすれば公開されるのは WebGPU C API のみで、absl は DLL 内部に閉じる。
set(DAWN_BUILD_MONOLITHIC_LIBRARY SHARED CACHE STRING "" FORCE)
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

# -----------------------------------------------------------------------------
# CRT を静的 (/MT) へ強制統一する。
# CMAKE_MSVC_RUNTIME_LIBRARY 変数だけでは Dawn の third_party (fetch_dawn_dependencies
# が取得する abseil/protobuf 等) の一部に伝播せず /MD が混ざり、静的 CRT 固定の
# prebuilt V8 とのリンクで LNK2005/LNK2019 になる。全ターゲットへ再帰的に
# プロパティを直接設定して確実に揃える。
# -----------------------------------------------------------------------------
function(next2d_force_static_crt dir)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _targets)
        get_target_property(_type ${_t} TYPE)
        if(NOT _type STREQUAL "INTERFACE_LIBRARY")
            set_property(TARGET ${_t} PROPERTY
                MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
    endforeach()
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_sd IN LISTS _subdirs)
        next2d_force_static_crt("${_sd}")
    endforeach()
endfunction()

if(MSVC)
    next2d_force_static_crt("${dawn_SOURCE_DIR}")
endif()
