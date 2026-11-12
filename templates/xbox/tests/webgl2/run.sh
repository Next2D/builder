#!/usr/bin/env bash
# WebGL2 バインディング (src/bindings/webgl/WebGL2.cpp) を実 GPU 上で実行検証する。
#
# ホスト本体のビルド (V8 モノリス・実機) なしで、Node 内蔵の V8 と ANGLE の
# OpenGL ES 3 を使ってバインディングをそのまま動かし、描画結果を読み戻して確認する。
#
# 使い方:
#   tests/webgl2/run.sh [ANGLE_DIR]
# ANGLE_DIR は libEGL / libGLESv2 を含むディレクトリ。省略時は環境変数 ANGLE_DIR、
# それも無ければ Electron / Chromium 系アプリに同梱の ANGLE を探す (macOS)。
set -euo pipefail
cd "$(dirname "$0")/../.."

ANGLE_DIR="${1:-${ANGLE_DIR:-}}"
if [ -z "${ANGLE_DIR}" ]; then
    while IFS= read -r lib; do
        ANGLE_DIR="$(dirname "$lib")"
        break
    done < <(find /Applications -maxdepth 8 -name "libGLESv2.dylib" 2>/dev/null)
fi
if [ -z "${ANGLE_DIR}" ] || [ ! -f "${ANGLE_DIR}/libGLESv2.dylib" ]; then
    echo "ANGLE (libEGL.dylib / libGLESv2.dylib) not found. Pass ANGLE_DIR." >&2
    exit 2
fi
echo "ANGLE: ${ANGLE_DIR}"

NODE_INC="$(dirname "$(dirname "$(node -p process.execPath)")")/include/node"
OUT="${TMPDIR:-/tmp}/next2d_webgl2_test"
mkdir -p "${OUT}"

c++ -std=c++20 -O1 -g -Wall -Wextra -shared -undefined dynamic_lookup -fPIC \
    -DNEXT2D_ENABLE_WEBGL2=1 \
    -Isrc -I. -Ithird_party/khronos -isystem "${NODE_INC}" \
    tests/webgl2/addon.cpp src/bindings/webgl/WebGL2.cpp src/gpu/EglContext.cpp \
    "${ANGLE_DIR}/libEGL.dylib" "${ANGLE_DIR}/libGLESv2.dylib" \
    -o "${OUT}/webgl2_test.node"

# 同梱 ANGLE の install name は相対パス (./libEGL.dylib, ./libGLESv2.dylib) で、libEGL も実行時に
# ./libGLESv2.dylib を読む。そのため ANGLE のディレクトリをカレントにして実行する。
TEST_JS="$(pwd)/tests/webgl2/test.cjs"
(cd "${ANGLE_DIR}" && NEXT2D_WEBGL2_ADDON="${OUT}/webgl2_test.node" node "${TEST_JS}")

# 結合テスト: Next2D player の renderer worker (WebGL バックエンド) をバインディング上で動かす。
# player のソース (依存インストール済み) が必要。PLAYER_DIR 未指定なら builder の隣を探し、無ければ省略。
PLAYER_DIR="${PLAYER_DIR:-$(cd ../.. && pwd)/../player}"
if [ ! -x "${PLAYER_DIR}/node_modules/.bin/rollup" ]; then
    echo "skip: player integration test (set PLAYER_DIR to a Next2D player checkout with dependencies installed)"
    exit 0
fi
PLAYER_DIR="$(cd "${PLAYER_DIR}" && pwd)"
echo "player: ${PLAYER_DIR}"
(cd "${PLAYER_DIR}" && ./node_modules/.bin/rollup packages/renderer/src/index.ts \
    -f iife -n RendererWorker --inlineDynamicImports --silent \
    -p '@rollup/plugin-node-resolve={browser:true,preferBuiltins:false}' \
    -p @rollup/plugin-commonjs \
    -p '@rollup/plugin-typescript={tsconfig:"./tsconfig.json",compilerOptions:{outDir:undefined,declaration:false}}' \
    -o "${OUT}/renderer.worker.js")
PLAYER_JS="$(pwd)/tests/webgl2/player.cjs"
(cd "${ANGLE_DIR}" && NEXT2D_WEBGL2_ADDON="${OUT}/webgl2_test.node" \
    NEXT2D_RENDERER_BUNDLE="${OUT}/renderer.worker.js" node "${PLAYER_JS}")
