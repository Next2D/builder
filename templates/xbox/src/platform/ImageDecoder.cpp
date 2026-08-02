#include "ImageDecoder.h"

// stb_image: public domain のヘッダオンリー画像デコーダ (third_party/)。
// メモリ入力のみ使用するため stdio を切る。
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "third_party/stb_image.h"

#if defined(_WIN32) && !NEXT2D_XBOX_CONSOLE
#include "WicDecoder.h"
#endif

#include <cstring>

namespace next2d {

bool DecodeImage(const std::vector<uint8_t>& input, DecodedImage& out)
{
    if (input.empty()) {
        return false;
    }

    // 1) stb_image (PNG/JPEG/GIF/BMP/TGA/PSD)。コンソールでも動く共通経路。
    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load_from_memory(
        input.data(), static_cast<int>(input.size()), &w, &h, &comp, 4);
    if (pixels) {
        out.width  = static_cast<uint32_t>(w);
        out.height = static_cast<uint32_t>(h);
        out.rgba.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
        stbi_image_free(pixels);
        return true;
    }

#if defined(_WIN32) && !NEXT2D_XBOX_CONSOLE
    // 2) デスクトップのみ: WIC フォールバック (TIFF/HEIF 等 stb 非対応形式の保険)。
    //    コンソールには WIC が無いためこの経路はコンパイルされない。
    return DecodeImageWithWIC(input, out);
#else
    return false;
#endif
}

} // namespace next2d
