#include "StbTextRasterizer.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace next2d::stbtext {

namespace {

struct Font {
    std::string name_lower;      // 登録名 (小文字)
    bool bold = false;           // 名前に "bold" を含むか
    std::vector<uint8_t> data;   // TTF/OTF 本体 (fontinfo が参照し続けるため保持)
    stbtt_fontinfo info = {};
};

// 登録フォント。ゲーム起動時に main が登録し、以後は読み取りのみ
// (worker も協調シングルスレッドなので排他は不要)。
std::vector<Font>& Fonts()
{
    static std::vector<Font> fonts;
    return fonts;
}

std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// CSS font 文字列から px サイズ / bold / ファミリ名リストを取り出す。
// 例: "bold 24px 'Noto Sans JP', sans-serif"
struct CssFont {
    double px = 16.0;
    bool bold = false;
    std::vector<std::string> families;   // 小文字・引用符除去済み
};

CssFont ParseCssFont(const std::string& css)
{
    CssFont out;
    const std::string lower = ToLower(css);

    if (lower.find("bold") != std::string::npos) {
        out.bold = true;
    } else {
        // 数値ウェイト (600 以上を bold 扱い)。"NNNpx" と混同しないよう
        // 独立トークンのみ見る。
        size_t pos = 0;
        while (pos < lower.size()) {
            const size_t start = lower.find_first_not_of(" \t", pos);
            if (start == std::string::npos) break;
            size_t end = lower.find_first_of(" \t", start);
            if (end == std::string::npos) end = lower.size();
            const std::string token = lower.substr(start, end - start);
            char* endp = nullptr;
            const long w = std::strtol(token.c_str(), &endp, 10);
            if (endp && *endp == '\0' && w >= 600 && w <= 1000) {
                out.bold = true;
            }
            pos = end;
        }
    }

    // "NNpx" (小数許容)
    const size_t px_pos = lower.find("px");
    if (px_pos != std::string::npos) {
        size_t num_start = px_pos;
        while (num_start > 0 &&
               (std::isdigit(static_cast<unsigned char>(lower[num_start - 1])) ||
                lower[num_start - 1] == '.')) {
            --num_start;
        }
        if (num_start < px_pos) {
            out.px = std::strtod(lower.substr(num_start, px_pos - num_start).c_str(), nullptr);
        }
        if (out.px <= 0) {
            out.px = 16.0;
        }

        // px の後ろがファミリリスト (カンマ区切り、引用符除去)
        size_t fam_start = lower.find_first_of(" \t", px_pos);
        if (fam_start != std::string::npos) {
            std::string families = lower.substr(fam_start + 1);
            size_t pos = 0;
            while (pos <= families.size()) {
                size_t comma = families.find(',', pos);
                if (comma == std::string::npos) comma = families.size();
                std::string fam = families.substr(pos, comma - pos);
                // trim + 引用符除去
                const auto is_trim = [](char c) {
                    return c == ' ' || c == '\t' || c == '\'' || c == '"';
                };
                while (!fam.empty() && is_trim(fam.front())) fam.erase(fam.begin());
                while (!fam.empty() && is_trim(fam.back()))  fam.pop_back();
                if (!fam.empty()) {
                    out.families.push_back(fam);
                }
                pos = comma + 1;
            }
        }
    }
    return out;
}

// wstring -> Unicode コードポイント列。
// Windows は wchar_t=UTF-16 (サロゲートペア復号)、他は UTF-32。
std::vector<uint32_t> ToCodepoints(const std::wstring& text)
{
    std::vector<uint32_t> cps;
    cps.reserve(text.size());
    if (sizeof(wchar_t) == 2) {
        for (size_t i = 0; i < text.size(); ++i) {
            const uint32_t c = static_cast<uint16_t>(text[i]);
            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.size()) {
                const uint32_t lo = static_cast<uint16_t>(text[i + 1]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cps.push_back(0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00));
                    ++i;
                    continue;
                }
            }
            cps.push_back(c);
        }
    } else {
        for (wchar_t c : text) {
            cps.push_back(static_cast<uint32_t>(c));
        }
    }
    return cps;
}

// css のファミリ指定と bold から最適な登録フォントを選ぶ。
// 優先: ファミリ一致 + bold 一致 > ファミリ一致 > bold 一致 > 先頭。
const Font* SelectFont(const CssFont& css)
{
    const auto& fonts = Fonts();
    if (fonts.empty()) {
        return nullptr;
    }
    const Font* family_match = nullptr;
    for (const auto& fam : css.families) {
        for (const auto& f : fonts) {
            if (f.name_lower.find(fam) == std::string::npos &&
                fam.find(f.name_lower) == std::string::npos) {
                continue;
            }
            if (f.bold == css.bold) {
                return &f;   // ファミリ+bold 完全一致
            }
            if (!family_match) {
                family_match = &f;
            }
        }
    }
    if (family_match) {
        return family_match;
    }
    for (const auto& f : fonts) {
        if (f.bold == css.bold) {
            return &f;
        }
    }
    return &fonts.front();
}

// 計測とラスタライズで共有するレイアウト計算。
struct Layout {
    const Font* font = nullptr;
    float scale = 0;
    int ascent = 0, descent = 0;   // フォント単位
    double width = 0;              // px
    std::vector<uint32_t> cps;
};

bool ComputeLayout(const std::string& css_font, const std::wstring& text, Layout& lo)
{
    const CssFont css = ParseCssFont(css_font);
    lo.font = SelectFont(css);
    if (!lo.font) {
        return false;
    }
    lo.scale = stbtt_ScaleForPixelHeight(&lo.font->info, static_cast<float>(css.px));
    int line_gap = 0;
    stbtt_GetFontVMetrics(&lo.font->info, &lo.ascent, &lo.descent, &line_gap);
    lo.cps = ToCodepoints(text);

    double x = 0;
    for (size_t i = 0; i < lo.cps.size(); ++i) {
        int advance = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&lo.font->info, static_cast<int>(lo.cps[i]), &advance, &lsb);
        x += advance * lo.scale;
        if (i + 1 < lo.cps.size()) {
            x += stbtt_GetCodepointKernAdvance(
                     &lo.font->info,
                     static_cast<int>(lo.cps[i]),
                     static_cast<int>(lo.cps[i + 1])) * lo.scale;
        }
    }
    lo.width = x;
    return true;
}

} // namespace

bool RegisterFont(const std::string& name, std::vector<uint8_t> data)
{
    if (data.empty()) {
        return false;
    }
    Font font;
    font.name_lower = ToLower(name);
    font.bold = font.name_lower.find("bold") != std::string::npos;
    font.data = std::move(data);
    const int offset = stbtt_GetFontOffsetForIndex(font.data.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&font.info, font.data.data(), offset)) {
        return false;
    }
    Fonts().push_back(std::move(font));
    return true;
}

bool HasFonts()
{
    return !Fonts().empty();
}

bool MeasureText(const std::string& css_font, const std::wstring& text,
                 TextMetricsInfo& out)
{
    Layout lo;
    if (!ComputeLayout(css_font, text, lo)) {
        return false;
    }
    out.width   = lo.width;
    out.ascent  = lo.ascent * lo.scale;
    out.descent = -lo.descent * lo.scale;   // stbtt の descent は負値
    return true;
}

bool RasterizeText(const std::string& css_font, const std::wstring& text,
                   uint8_t r, uint8_t g, uint8_t b, TextBitmap& out)
{
    if (text.empty()) {
        return false;   // DirectWrite 実装と同じ契約 (空文字は false)
    }
    Layout lo;
    if (!ComputeLayout(css_font, text, lo)) {
        return false;
    }

    const double ascent_px  = lo.ascent * lo.scale;
    const double descent_px = -lo.descent * lo.scale;
    const int width  = std::max(1, static_cast<int>(std::ceil(lo.width)) + 2);
    const int height = std::max(1, static_cast<int>(std::ceil(ascent_px + descent_px)) + 2);
    const int baseline = static_cast<int>(std::ceil(ascent_px)) + 1;

    // 8bit カバレッジへ全グリフを合成してから RGBA 化する
    std::vector<uint8_t> coverage(static_cast<size_t>(width) * height, 0);
    double pen_x = 1.0;
    for (size_t i = 0; i < lo.cps.size(); ++i) {
        const int cp = static_cast<int>(lo.cps[i]);
        int advance = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&lo.font->info, cp, &advance, &lsb);

        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        const float shift_x =
            static_cast<float>(pen_x + lsb * lo.scale - std::floor(pen_x + lsb * lo.scale));
        stbtt_GetCodepointBitmapBoxSubpixel(
            &lo.font->info, cp, lo.scale, lo.scale, shift_x, 0, &x0, &y0, &x1, &y1);

        const int gw = x1 - x0;
        const int gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            std::vector<uint8_t> glyph(static_cast<size_t>(gw) * gh);
            stbtt_MakeCodepointBitmapSubpixel(
                &lo.font->info, glyph.data(), gw, gh, gw,
                lo.scale, lo.scale, shift_x, 0, cp);

            const int dst_x = static_cast<int>(std::floor(pen_x + lsb * lo.scale));
            const int dst_y = baseline + y0;
            for (int yy = 0; yy < gh; ++yy) {
                const int py = dst_y + yy;
                if (py < 0 || py >= height) continue;
                for (int xx = 0; xx < gw; ++xx) {
                    const int px = dst_x + xx;
                    if (px < 0 || px >= width) continue;
                    uint8_t& dst = coverage[static_cast<size_t>(py) * width + px];
                    dst = std::max(dst, glyph[static_cast<size_t>(yy) * gw + xx]);
                }
            }
        }

        pen_x += advance * lo.scale;
        if (i + 1 < lo.cps.size()) {
            pen_x += stbtt_GetCodepointKernAdvance(
                         &lo.font->info, cp, static_cast<int>(lo.cps[i + 1])) * lo.scale;
        }
    }

    out.width = width;
    out.height = height;
    out.baseline = baseline;
    out.rgba.assign(static_cast<size_t>(width) * height * 4, 0);
    for (size_t i = 0; i < coverage.size(); ++i) {
        const uint8_t a = coverage[i];
        if (!a) continue;
        uint8_t* dst = &out.rgba[i * 4];
        dst[0] = r;
        dst[1] = g;
        dst[2] = b;
        dst[3] = a;   // straight alpha (TextBitmap の契約)
    }
    return true;
}

} // namespace next2d::stbtext
