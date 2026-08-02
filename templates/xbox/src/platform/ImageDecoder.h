// 画像デコード (エンコード済みバイト列 -> RGBA8)。
//
// 第一候補は stb_image (PNG/JPEG/GIF/BMP、全プラットフォーム共通)。
// コンソール (Game Core OS) には WIC が存在しないため、WIC 依存を第一経路から
// 外すことで PC 検証と実機の挙動を一致させる。デスクトップのみ、stb が扱えない
// 形式 (TIFF/HEIF 等) への保険として WIC フォールバックを残す。
#pragma once

#include "ImageTypes.h"

#include <cstdint>
#include <vector>

namespace next2d {

bool DecodeImage(const std::vector<uint8_t>& input, DecodedImage& out);

} // namespace next2d
