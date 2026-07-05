# =============================================================================
# V8 (prebuilt monolith) を探して v8::v8 ターゲットを定義する。
#
# V8 は GN/depot_tools でのソースビルドが非常に重いため、prebuilt を前提とする。
# 以下のいずれかで monolith を用意すること:
#
#   1. 自前ビルド (推奨・再現性高)
#        fetch v8 && cd v8
#        gn gen out/x64.release --args='
#            v8_monolithic=true
#            v8_use_external_startup_data=false
#            is_component_build=false
#            use_custom_libcxx=false
#            icu_use_data_file=false
#            treat_warnings_as_errors=false
#            v8_jitless=true
#            v8_enable_turbofan=false
#            v8_enable_webassembly=false
#            target_cpu="x64"
#            is_debug=false
#            v8_enable_pointer_compression=true
#            v8_enable_sandbox=false'
#        ninja -C out/x64.release v8_monolith
#
#   2. 配布物 (v8_monolith.lib + include/) を取得
#
# cmake 構成時に -D V8_ROOT=<path> を渡す。
#   <V8_ROOT>/include/v8.h
#   <V8_ROOT>/lib/v8_monolith.lib
# =============================================================================
if(NOT DEFINED V8_ROOT)
    set(V8_ROOT "$ENV{V8_ROOT}")
endif()

if(NOT V8_ROOT OR NOT EXISTS "${V8_ROOT}")
    message(FATAL_ERROR
        "V8_ROOT が未設定/不正です。prebuilt V8 monolith のパスを指定してください:\n"
        "  npx @next2d/builder --platform xbox --env prd --v8-root C:/path/to/v8\n"
        "  (CMake 直叩きの場合: cmake -D V8_ROOT=C:/path/to/v8 ...)\n"
        "詳細は README.md の『V8 の用意』を参照。")
endif()

# find_path/find_library の結果は CMakeCache に永続化されるため、
# V8_ROOT の変更 (バージョン更新等) や参照先の消滅に追随しない。
# 古い/無効な検出結果はここで破棄して再探索させる。
if(DEFINED CACHE{NEXT2D_V8_ROOT_LAST} AND NOT "$CACHE{NEXT2D_V8_ROOT_LAST}" STREQUAL "${V8_ROOT}")
    unset(V8_INCLUDE_DIR CACHE)
    unset(V8_MONOLITH_LIB CACHE)
endif()
if(V8_INCLUDE_DIR AND NOT EXISTS "${V8_INCLUDE_DIR}/v8.h")
    unset(V8_INCLUDE_DIR CACHE)
endif()
if(V8_MONOLITH_LIB AND NOT EXISTS "${V8_MONOLITH_LIB}")
    unset(V8_MONOLITH_LIB CACHE)
endif()
set(NEXT2D_V8_ROOT_LAST "${V8_ROOT}" CACHE INTERNAL "直近の構成で使った V8_ROOT")

find_path(V8_INCLUDE_DIR
    NAMES v8.h
    PATHS "${V8_ROOT}/include"
    NO_DEFAULT_PATH
)

find_library(V8_MONOLITH_LIB
    NAMES v8_monolith
    PATHS "${V8_ROOT}/lib" "${V8_ROOT}/out/x64.release/obj"
    NO_DEFAULT_PATH
)

if(NOT V8_INCLUDE_DIR OR NOT V8_MONOLITH_LIB)
    message(FATAL_ERROR "V8 の include / v8_monolith.lib が見つかりません (V8_ROOT=${V8_ROOT})")
endif()

add_library(v8::v8 STATIC IMPORTED)
set_target_properties(v8::v8 PROPERTIES
    IMPORTED_LOCATION "${V8_MONOLITH_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${V8_INCLUDE_DIR}"
)

# V8 monolith が要求する Windows ライブラリ
target_link_libraries(v8::v8 INTERFACE winmm dbghelp)

message(STATUS "V8 found: ${V8_MONOLITH_LIB}")
