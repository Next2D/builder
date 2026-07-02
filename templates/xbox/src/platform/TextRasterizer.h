// DirectWrite / Direct2D によるテキストラスタライズ。V8 非依存。
// Canvas2D.cpp (fillText/strokeText/measureText) と単体テストの両方から使う。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace next2d {

// テキストのラスタライズ結果 (straight RGBA)。
struct TextBitmap {
    int width = 0;
    int height = 0;
    double baseline = 0;             // 先頭行のベースライン位置 (上端から)
    std::vector<uint8_t> rgba;       // width*height*4
};

// テキストのメトリクス。
struct TextMetricsInfo {
    double width = 0;
    double ascent = 0;
    double descent = 0;
};

// CSS font 文字列 ("bold 24px Arial" 等) と UTF-16 テキストからメトリクスを取得する。
// DirectWrite が使えない場合は false (呼び出し側で近似値へフォールバック)。
bool MeasureTextWithDWrite(const std::string& css_font, const std::wstring& text,
                           TextMetricsInfo& out);

// テキストを指定色でラスタライズする。alpha はグリフ形状から得る。
bool RasterizeTextWithDWrite(const std::string& css_font, const std::wstring& text,
                             uint8_t r, uint8_t g, uint8_t b, TextBitmap& out);

} // namespace next2d
