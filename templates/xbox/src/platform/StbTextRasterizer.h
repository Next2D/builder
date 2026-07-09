// stb_truetype によるテキスト計測/ラスタライズ (全プラットフォーム共通)。
//
// コンソール (Game Core OS) には DirectWrite が無いため、TextRasterizer.cpp の
// コンソール分岐がこの実装へ委譲する (デスクトップは従来どおり DirectWrite)。
// システムフォントにも依存できないため、フォントは実行時に登録する:
//   - 埋め込み pak 内の *.ttf/*.otf (ゲーム資材。main.cpp が起動時に登録)
//   - exe 隣接の fonts/*.ttf (開発ビルド用)
// フォント未登録の場合は false を返し、呼び出し側 (Canvas2D) が近似値へ
// フォールバックする (TextRasterizer.h と同じ契約)。
//
// 制限 (DirectWrite との差): 合字/シェーピング無し (CJK は影響なし)、
// bold は登録フォントに bold バリアントがある場合のみ反映。
#pragma once

#include "TextRasterizer.h"   // TextBitmap / TextMetricsInfo

#include <cstdint>
#include <string>
#include <vector>

namespace next2d::stbtext {

// TTF/OTF バイト列をフォントとして登録する (name はファイル名 stem 等の識別子。
// "bold" を含む名前は bold バリアントとして扱う)。不正なフォントは false。
bool RegisterFont(const std::string& name, std::vector<uint8_t> data);

// 登録済みフォントが 1 つ以上あるか。
bool HasFonts();

// css_font 例: "bold 24px Arial", "16px 'Noto Sans JP', sans-serif"
bool MeasureText(const std::string& css_font, const std::wstring& text,
                 TextMetricsInfo& out);

bool RasterizeText(const std::string& css_font, const std::wstring& text,
                   uint8_t r, uint8_t g, uint8_t b, TextBitmap& out);

} // namespace next2d::stbtext
