#include "TextRasterizer.h"

// DirectWrite / Direct2D / WIC はコンソール (Game Core OS) に存在しない
// デスクトップ専用 API。コンソールでは false を返し、呼び出し側
// (Canvas2D の fillText/measureText) が近似値へフォールバックする
// (契約は TextRasterizer.h に明記済み)。
// 実機でのテキスト描画品質は devkit 段階の課題 (stb_truetype + フォント同梱)。
#if !NEXT2D_XBOX_CONSOLE

#include "../bindings/RasterCore.h"

#include <algorithm>
#include <cmath>

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;

namespace next2d {

namespace {

IDWriteFactory* DWrite()
{
    static ComPtr<IDWriteFactory> factory;
    if (!factory) {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
    }
    return factory.Get();
}

ID2D1Factory* D2D()
{
    static ComPtr<ID2D1Factory> factory;
    if (!factory) {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf());
    }
    return factory.Get();
}

IWICImagingFactory* WIC()
{
    static ComPtr<IWICImagingFactory> factory;
    if (!factory) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(factory.GetAddressOf()));
    }
    return factory.Get();
}

// CSS font 文字列から family を抽出 (px サイズは raster::FontPixelSize)。
std::wstring FontFamily(const std::string& font)
{
    // 末尾のカンマ区切りの先頭ファミリを採用。無ければ Segoe UI。
    const auto pos = font.find("px");
    std::string rest = (pos != std::string::npos && pos + 2 < font.size())
        ? font.substr(pos + 2) : "sans-serif";
    // trim + 先頭ファミリ
    size_t s = rest.find_first_not_of(" \t");
    if (s == std::string::npos) return L"Segoe UI";
    rest = rest.substr(s);
    const auto comma = rest.find(',');
    if (comma != std::string::npos) rest = rest.substr(0, comma);
    // クォート除去
    rest.erase(std::remove(rest.begin(), rest.end(), '\''), rest.end());
    rest.erase(std::remove(rest.begin(), rest.end(), '"'), rest.end());
    if (rest == "sans-serif" || rest.empty()) return L"Segoe UI";
    if (rest == "serif") return L"Times New Roman";
    if (rest == "monospace") return L"Consolas";
    return std::wstring(rest.begin(), rest.end());
}

ComPtr<IDWriteTextLayout> MakeLayout(const std::string& css_font, const std::wstring& wtext)
{
    if (!DWrite()) return nullptr;
    ComPtr<IDWriteTextFormat> format;
    const bool bold = css_font.find("bold") != std::string::npos;
    const bool italic = css_font.find("italic") != std::string::npos;
    DWrite()->CreateTextFormat(
        FontFamily(css_font).c_str(), nullptr,
        bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
        italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        static_cast<float>(raster::FontPixelSize(css_font)), L"en-us", &format);
    if (!format) return nullptr;
    ComPtr<IDWriteTextLayout> layout;
    DWrite()->CreateTextLayout(wtext.c_str(), static_cast<UINT32>(wtext.size()),
                               format.Get(), 100000.0f, 100000.0f, &layout);
    return layout;
}

} // namespace

bool MeasureTextWithDWrite(const std::string& css_font, const std::wstring& text,
                           TextMetricsInfo& out)
{
    ComPtr<IDWriteTextLayout> layout = MakeLayout(css_font, text);
    if (!layout) return false;

    DWRITE_TEXT_METRICS tm = {};
    layout->GetMetrics(&tm);
    out.width = tm.widthIncludingTrailingWhitespace;

    UINT32 lineCount = 0;
    layout->GetLineMetrics(nullptr, 0, &lineCount);
    if (lineCount > 0) {
        std::vector<DWRITE_LINE_METRICS> lines(lineCount);
        layout->GetLineMetrics(lines.data(), lineCount, &lineCount);
        out.ascent = lines[0].baseline;
        out.descent = lines[0].height - lines[0].baseline;
    } else {
        const double px = raster::FontPixelSize(css_font);
        out.ascent = px * 0.8;
        out.descent = px * 0.2;
    }
    return true;
}

bool RasterizeTextWithDWrite(const std::string& css_font, const std::wstring& text,
                             uint8_t r, uint8_t g, uint8_t b, TextBitmap& out)
{
    if (text.empty() || !DWrite() || !D2D() || !WIC()) return false;
    ComPtr<IDWriteTextLayout> layout = MakeLayout(css_font, text);
    if (!layout) return false;

    DWRITE_TEXT_METRICS tm = {};
    layout->GetMetrics(&tm);
    const int tw = std::max(1, static_cast<int>(std::ceil(tm.widthIncludingTrailingWhitespace)) + 2);
    const int th = std::max(1, static_cast<int>(std::ceil(tm.height)) + 2);

    // fillText の x,y はベースライン起点。レイアウトは左上起点なので baseline を返す。
    out.baseline = raster::FontPixelSize(css_font) * 0.8;
    UINT32 lineCount = 0; layout->GetLineMetrics(nullptr, 0, &lineCount);
    if (lineCount > 0) {
        std::vector<DWRITE_LINE_METRICS> lines(lineCount);
        layout->GetLineMetrics(lines.data(), lineCount, &lineCount);
        out.baseline = lines[0].baseline;
    }

    ComPtr<IWICBitmap> wic;
    if (FAILED(WIC()->CreateBitmap(tw, th, GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapCacheOnLoad, &wic))) return false;
    ComPtr<ID2D1RenderTarget> rt;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(D2D()->CreateWicBitmapRenderTarget(wic.Get(), props, &rt))) return false;
    ComPtr<ID2D1SolidColorBrush> brush;
    rt->CreateSolidColorBrush(
        D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f, 1.0f), &brush);

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0));
    rt->DrawTextLayout(D2D1::Point2F(0, 0), layout.Get(), brush.Get());
    if (FAILED(rt->EndDraw())) return false;

    // 読み戻し (PBGRA -> straight RGBA)
    ComPtr<IWICBitmapLock> lock;
    WICRect rc = {0, 0, tw, th};
    if (FAILED(wic->Lock(&rc, WICBitmapLockRead, &lock))) return false;
    UINT stride = 0, size = 0; BYTE* data = nullptr;
    lock->GetStride(&stride); lock->GetDataPointer(&size, &data);

    out.width = tw;
    out.height = th;
    out.rgba.assign(static_cast<size_t>(tw) * th * 4, 0);
    for (int yy = 0; yy < th; ++yy) {
        for (int xx = 0; xx < tw; ++xx) {
            const BYTE* px = data + yy * stride + xx * 4;
            const double a = px[3] / 255.0;
            if (a <= 0.0) continue;
            uint8_t* dst = &out.rgba[(static_cast<size_t>(yy) * tw + xx) * 4];
            dst[0] = static_cast<uint8_t>(px[2] / a);  // R (un-premultiply)
            dst[1] = static_cast<uint8_t>(px[1] / a);  // G
            dst[2] = static_cast<uint8_t>(px[0] / a);  // B
            dst[3] = px[3];                            // A
        }
    }
    return true;
}

} // namespace next2d

#else // NEXT2D_XBOX_CONSOLE

namespace next2d {

bool MeasureTextWithDWrite(const std::string&, const std::wstring&, TextMetricsInfo&)
{
    return false;   // 呼び出し側が近似値へフォールバックする
}

bool RasterizeTextWithDWrite(const std::string&, const std::wstring&,
                             uint8_t, uint8_t, uint8_t, TextBitmap&)
{
    return false;
}

} // namespace next2d

#endif // !NEXT2D_XBOX_CONSOLE
