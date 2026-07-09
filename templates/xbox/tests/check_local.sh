#!/usr/bin/env bash
# macOS / Linux で Windows 実機なしに実行できる検証をまとめて回す。
#   1. RasterCore 単体テスト (実行)
#   2. RasterCore 単体テスト (ASan/UBSan)
#   3. V8 依存ソースの構文チェック (実 V8 ヘッダに対して。Windows API 非依存の12ファイル)
#
# 使い方:
#   tests/check_local.sh [V8_INCLUDE_DIR]
# V8_INCLUDE_DIR 省略時は ./.v8_headers/include へ sparse clone する。
set -euo pipefail
cd "$(dirname "$0")/.."

V8_TAG="13.7.152.19"
V8INC="${1:-}"

echo "== 1/3 RasterCore unit tests =="
c++ -std=c++17 -Wall -Wextra -O1 -o /tmp/next2d_raster_test tests/raster_test.cpp
/tmp/next2d_raster_test

echo "== 2/3 RasterCore unit tests (ASan/UBSan) =="
c++ -std=c++17 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -o /tmp/next2d_raster_test_asan tests/raster_test.cpp
/tmp/next2d_raster_test_asan

echo "== 3/3 V8-dependent sources syntax check (V8 ${V8_TAG}) =="
if [ -z "${V8INC}" ]; then
    if [ ! -d ".v8_headers/include" ]; then
        git clone --depth 1 --branch "${V8_TAG}" --filter=blob:none --sparse \
            https://github.com/v8/v8.git .v8_headers
        (cd .v8_headers && git sparse-checkout set include)
    fi
    V8INC=".v8_headers/include"
fi

# Windows API に依存しない V8 バインディング (Windows 依存分は CI の windows-latest で検証)
FILES=(
    src/EventLoop.cpp
    src/AssetLoader.cpp
    src/v8/V8Runtime.cpp
    src/worker/WorkerRuntime.cpp
    src/bindings/Console.cpp
    src/bindings/Timers.cpp
    src/bindings/EventTarget.cpp
    src/bindings/Polyfills.cpp
    src/bindings/Network.cpp
    src/bindings/Fetch.cpp
    src/bindings/Canvas.cpp
    src/bindings/Audio.cpp
    src/bindings/Gamepad.cpp
    src/platform/DecodeQueue.cpp
    src/platform/AudioDecoder.cpp
    src/platform/ImageDecoder.cpp
    src/platform/StbTextRasterizer.cpp
)
for f in "${FILES[@]}"; do
    c++ -fsyntax-only -std=c++20 -Isrc -I. -isystem "${V8INC}" \
        -DV8_COMPRESS_POINTERS "$f"
    echo "OK   $f"
done

echo "all checks passed"
