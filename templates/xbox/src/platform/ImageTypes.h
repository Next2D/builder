// 画像デコード結果の共有型。V8 に依存しないため、単体テスト
// (tests/windows_platform_test.cpp) からも利用できる。
#pragma once

#include <cstdint>
#include <vector>

namespace next2d {

// createImageBitmap / transferToImageBitmap が返す ImageBitmap の実体。
struct DecodedImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;   // RGBA8 straight alpha
};

} // namespace next2d
