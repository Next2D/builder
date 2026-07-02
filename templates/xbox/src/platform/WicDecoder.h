// WIC (Windows Imaging Component) による画像デコード。V8 非依存。
// Image.cpp (createImageBitmap / Image.src) と単体テストの両方から使う。
#pragma once

#include "ImageTypes.h"

#include <cstdint>
#include <vector>

namespace next2d {

// 任意フォーマット(PNG/JPEG/GIF/BMP 等)のバイト列を RGBA8 straight へデコードする。
// 呼び出し前に COM が初期化されている必要がある (CoInitializeEx)。
bool DecodeImageWithWIC(const std::vector<uint8_t>& input, DecodedImage& out);

} // namespace next2d
