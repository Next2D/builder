// Canvas2D のソフトウェアラスタライザ純ロジック。
// V8 / Windows API に依存しないため、macOS/Linux でも単体テストできる
// (tests/raster_test.cpp)。Canvas2D.cpp はこのヘッダの薄いバインディング層。
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace next2d::raster {

inline constexpr double kPi = 3.14159265358979323846;

struct Mat { double a=1,b=0,c=0,d=1,e=0,f=0; };

struct Point { double x, y; };

struct SubPath {
    std::vector<Point> pts;
    bool closed = false;
};

struct RGBA { uint8_t r=0,g=0,b=0,a=255; };

// クリップ領域 (デバイス座標)。
//   active=false          … 無制限
//   active=true, mask=null … 矩形クリップ [x0,x1) x [y0,y1) (厳密)
//   mask!=null            … 任意形状クリップ。x0..y1 は外接矩形 (高速リジェクト用)、
//                            mask は surface と同寸のカバレッジ (1=内側/0=外側)。
// mask は shared_ptr のため save()/restore() の state コピーで安価に共有される
// (clip() は既存 mask を破壊せず新しい mask を割り当てるので復元が壊れない)。
struct ClipRect {
    bool   active = false;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::shared_ptr<const std::vector<uint8_t>> mask;
    int mask_w = 0, mask_h = 0;
};

// RGBA8 のピクセルバッファ。
// 透明な (0, 0, 0, 0) の上に、RGBA8 のピクセル列を不透明度 1 で合成した結果を out へ書く
// (各ピクセルを Blend(..., 1.0) で合成した結果と同じ)。不透明なピクセルはそのまま、透明なピクセルは 0、
// 半透明なピクセルは透明との合成になる。out は初期化されていなくてよい。
inline void CompositeOntoTransparent(uint8_t* out, const uint8_t* src, size_t pixel_count)
{
    std::memcpy(out, src, pixel_count * 4);
    for (size_t i = 0; i < pixel_count; ++i) {
        uint8_t* p = out + i * 4;
        const uint8_t alpha = p[3];
        if (alpha == 255) {
            continue;
        }
        if (alpha == 0) {
            p[0] = p[1] = p[2] = 0;
            continue;
        }
        // Blend(..., 1.0) を、合成先が (0, 0, 0, 0) の場合について書いたもの
        const double a = alpha / 255.0;
        p[0] = static_cast<uint8_t>(p[0] * a + 0 * (1 - a));
        p[1] = static_cast<uint8_t>(p[1] * a + 0 * (1 - a));
        p[2] = static_cast<uint8_t>(p[2] * a + 0 * (1 - a));
        p[3] = static_cast<uint8_t>(255 * a + 0 * (1 - a));
    }
}

// canvas のビットマップ。読み込んだ画像を canvas へ写して読み出す、という使い方 (幅の代入、
// 高さの代入、描画、読み出し) で、画像 1 枚につき全ピクセルの書き込みを何度も行わないために、
// ピクセルを持たない状態を 2 つ持つ (どちらも pixels は空。幅と高さは 1 以上なので、確保済みなら
// 空にはならない):
//   - 何も描いていない (source が null): 全面が透明。大きさを変えた直後はこの状態で、確保しない。
//   - 画像を写しただけ (source が非 null): 同じ大きさの画像を、何も描いていない状態へぴったり
//     重ねた。内容は「透明の上にその画像を合成したもの」。source の指す先は、この状態の間
//     呼び出し側が生かしておく。
// 書き込む側は先に Ensure() を呼ぶ (ピクセルを確保して、内容を実体にする)。
struct Surface {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
    const uint8_t* source = nullptr;   // 写しただけの画像 (RGBA8、width x height)

    Surface(int w, int h)
        : width(w), height(h) {}

    size_t ByteSize() const
    {
        return static_cast<size_t>(width) * height * 4;
    }

    // 何も描いていない (全面が透明で、ピクセルを確保していない)
    bool Blank() const
    {
        return pixels.empty() && source == nullptr;
    }

    // ピクセルを確保して、内容を実体にする
    void Ensure()
    {
        if (!pixels.empty()) {
            return;
        }
        if (source != nullptr) {
            const size_t bytes = ByteSize();
            pixels.reserve(bytes);
            // 1 行ずつ足す (全体を 0 で埋めてから上書きしない)
            const size_t row_bytes = static_cast<size_t>(width) * 4;
            for (int y = 0; y < height; ++y) {
                const uint8_t* row = source + static_cast<size_t>(y) * row_bytes;
                pixels.insert(pixels.end(), row, row + row_bytes);
                CompositeOntoTransparent(&pixels[static_cast<size_t>(y) * row_bytes], row,
                                         static_cast<size_t>(width));
            }
            source = nullptr;
            return;
        }
        pixels.assign(ByteSize(), 0);
    }

    // 何も描いていない状態へ戻す
    void Clear()
    {
        pixels.clear();
        source = nullptr;
    }
};

inline Point Apply(const Mat& m, double x, double y)
{
    return { m.a * x + m.c * y + m.e, m.b * x + m.d * y + m.f };
}

inline Mat Multiply(const Mat& m, const Mat& n)
{
    return {
        m.a * n.a + m.c * n.b,
        m.b * n.a + m.d * n.b,
        m.a * n.c + m.c * n.d,
        m.b * n.c + m.d * n.d,
        m.a * n.e + m.c * n.f + m.e,
        m.b * n.e + m.d * n.f + m.f,
    };
}

// "bold 24px sans-serif" 等から px を抽出 (近似メトリクス用)
inline double FontPixelSize(const std::string& font)
{
    const auto pos = font.find("px");
    if (pos == std::string::npos) return 10.0;
    size_t start = pos;
    while (start > 0 && (isdigit(static_cast<unsigned char>(font[start - 1])) || font[start - 1] == '.')) --start;
    try { return std::stod(font.substr(start, pos - start)); } catch (...) { return 10.0; }
}

// #rgb / #rrggbb / rgb(...) / rgba(...) を解析する。
inline RGBA ParseColor(const std::string& s)
{
    RGBA c;
    if (s.empty()) return c;
    if (s[0] == '#') {
        auto hex = [&](int i, int n) {
            return static_cast<uint8_t>(std::stoi(s.substr(i, n), nullptr, 16) * (n == 1 ? 17 : 1));
        };
        if (s.size() == 4) { c.r = hex(1,1); c.g = hex(2,1); c.b = hex(3,1); }
        else if (s.size() >= 7) { c.r = hex(1,2); c.g = hex(3,2); c.b = hex(5,2); }
        return c;
    }
    if (s.rfind("rgb", 0) == 0) {
        double vals[4] = {0,0,0,1};
        int n = 0; size_t i = s.find('(');
        while (i != std::string::npos && n < 4) {
            i++;
            try { vals[n++] = std::stod(s.substr(i)); } catch (...) { break; }
            i = s.find(',', i);
        }
        c.r = static_cast<uint8_t>(vals[0]); c.g = static_cast<uint8_t>(vals[1]);
        c.b = static_cast<uint8_t>(vals[2]); c.a = static_cast<uint8_t>(vals[3] * 255);
    }
    return c;
}

// 1 ピクセルをアルファ合成する。クリップ矩形の外は書き込まない。
inline void Blend(Surface& s, const ClipRect& clip, int x, int y, const RGBA& col, double alpha)
{
    if (x < 0 || y < 0 || x >= s.width || y >= s.height) return;
    if (clip.active && (x < clip.x0 || x >= clip.x1 || y < clip.y0 || y >= clip.y1)) return;
    // 任意形状クリップ: マスクの外なら書き込まない。
    if (clip.mask && (x >= clip.mask_w || y >= clip.mask_h ||
                      (*clip.mask)[static_cast<size_t>(y) * clip.mask_w + x] == 0)) return;
    s.Ensure();
    size_t i = (static_cast<size_t>(y) * s.width + x) * 4;
    double a = (col.a / 255.0) * alpha;
    s.pixels[i+0] = static_cast<uint8_t>(col.r * a + s.pixels[i+0] * (1 - a));
    s.pixels[i+1] = static_cast<uint8_t>(col.g * a + s.pixels[i+1] * (1 - a));
    s.pixels[i+2] = static_cast<uint8_t>(col.b * a + s.pixels[i+2] * (1 - a));
    s.pixels[i+3] = static_cast<uint8_t>(255 * a + s.pixels[i+3] * (1 - a));
}

// RGBA8 の画像 (sw x sh) を、左上が (left, top) になるように等倍で合成する (クリップなし、不透明度 1)。
// 各ピクセルを Blend(..., 1.0) で合成した結果と同じだが、行ごとにまとめて処理する:
// 不透明なピクセルの並びはそのまま写し、透明なピクセルは飛ばし、半透明のピクセルだけ合成する。
// surface からはみ出す部分は描かない。
// まだ何も描いていない surface へ、同じ大きさの画像をぴったり重ねる場合 (読み込んだ画像を
// canvas へ写す、という最も多い使い方) は、surface を 0 で埋めてから写すのではなく、
// 画像の行をそのまま並べて作る (結果は同じ。透明なピクセルは 0、半透明なピクセルは透明との合成)。
inline void CopyImage(Surface& s, const uint8_t* src, uint32_t sw, uint32_t sh,
                      int64_t left, int64_t top)
{
    if (s.Blank() && left == 0 && top == 0 && s.width > 0 && s.height > 0 &&
        sw == static_cast<uint32_t>(s.width) && sh == static_cast<uint32_t>(s.height)) {
        s.source = src;
        s.Ensure();
        return;
    }
    s.Ensure();

    const int64_t x_begin = std::max<int64_t>(0, -left);
    const int64_t y_begin = std::max<int64_t>(0, -top);
    const int64_t x_end = std::min<int64_t>(sw, static_cast<int64_t>(s.width) - left);
    const int64_t y_end = std::min<int64_t>(sh, static_cast<int64_t>(s.height) - top);
    const ClipRect no_clip;
    for (int64_t yy = y_begin; yy < y_end; ++yy) {
        const uint8_t* row = &src[static_cast<size_t>(yy) * sw * 4];
        int64_t xx = x_begin;
        while (xx < x_end) {
            const uint8_t alpha = row[xx * 4 + 3];
            if (alpha == 255) {
                const int64_t start = xx;
                while (xx < x_end && row[xx * 4 + 3] == 255) ++xx;
                std::copy(row + start * 4, row + xx * 4,
                          &s.pixels[(static_cast<size_t>(top + yy) * s.width + (left + start)) * 4]);
            } else {
                if (alpha != 0) {
                    const uint8_t* p = row + xx * 4;
                    Blend(s, no_clip, static_cast<int>(left + xx), static_cast<int>(top + yy),
                          RGBA{p[0], p[1], p[2], p[3]}, 1.0);
                }
                ++xx;
            }
        }
    }
}

// スキャンライン塗り (nonzero winding)。
// HTML Canvas 仕様どおり、未クローズのサブパスも塗りでは暗黙にクローズする
// (isPointInPath と同じ扱い)。
inline void FillPath(Surface& s, const ClipRect& clip,
                     const std::vector<SubPath>& path, const RGBA& col, double alpha)
{
    double miny = 1e18, maxy = -1e18;
    for (auto& sp : path) for (auto& p : sp.pts) { miny = std::min(miny,p.y); maxy = std::max(maxy,p.y); }
    if (maxy < miny) return;
    int y0 = std::max(0, static_cast<int>(std::floor(miny)));
    int y1 = std::min(s.height - 1, static_cast<int>(std::ceil(maxy)));
    for (int y = y0; y <= y1; ++y) {
        double sy = y + 0.5;
        std::vector<std::pair<double,int>> xs; // x, winding dir
        for (auto& sp : path) {
            size_t n = sp.pts.size();
            if (n < 2) continue;
            for (size_t i = 0; i < n; ++i) {
                Point a = sp.pts[i];
                Point b = sp.pts[(i+1) % n];
                if ((a.y <= sy && b.y > sy) || (b.y <= sy && a.y > sy)) {
                    double t = (sy - a.y) / (b.y - a.y);
                    xs.push_back({a.x + t * (b.x - a.x), a.y < b.y ? 1 : -1});
                }
            }
        }
        std::sort(xs.begin(), xs.end());
        int wind = 0;
        for (size_t i = 0; i + 1 < xs.size(); ++i) {
            wind += xs[i].second;
            if (wind != 0) {
                int xa = std::max(0, static_cast<int>(std::floor(xs[i].first)));
                int xb = std::min(s.width - 1, static_cast<int>(std::ceil(xs[i+1].first)));
                for (int x = xa; x <= xb; ++x) Blend(s, clip, x, y, col, alpha);
            }
        }
    }
}

// 単純な線分描画 (太さは近似)。stroke は未クローズのサブパスを閉じない。
inline void StrokePath(Surface& s, const ClipRect& clip,
                       const std::vector<SubPath>& path, const RGBA& col, double alpha)
{
    for (auto& sp : path) {
        size_t n = sp.pts.size();
        size_t last = sp.closed ? n : (n > 0 ? n - 1 : 0);
        for (size_t i = 0; i < last; ++i) {
            Point a0 = sp.pts[i], b0 = sp.pts[(i+1) % n];
            int steps = static_cast<int>(std::hypot(b0.x - a0.x, b0.y - a0.y)) + 1;
            for (int st = 0; st <= steps; ++st) {
                double t = static_cast<double>(st) / steps;
                Blend(s, clip, static_cast<int>(a0.x + (b0.x-a0.x)*t),
                               static_cast<int>(a0.y + (b0.y-a0.y)*t), col, alpha);
            }
        }
    }
}

// nonzero winding の点内包判定。ヒット判定の中核。
inline bool PointInPath(const std::vector<SubPath>& path, double px, double py)
{
    int wind = 0;
    for (auto& sp : path) {
        size_t n = sp.pts.size();
        if (n < 2) continue;
        for (size_t i = 0; i < n; ++i) {
            Point A = sp.pts[i];
            Point B = sp.pts[(i+1) % n];
            if ((A.y <= py && B.y > py) || (B.y <= py && A.y > py)) {
                double t = (py - A.y) / (B.y - A.y);
                double xcross = A.x + t * (B.x - A.x);
                if (xcross > px) wind += (A.y < B.y) ? 1 : -1;
            }
        }
    }
    return wind != 0;
}

// パス全点の外接矩形。点が無ければ false。
inline bool PathBounds(const std::vector<SubPath>& path,
                       double* minx, double* miny, double* maxx, double* maxy)
{
    bool any = false;
    *minx = 1e18; *miny = 1e18; *maxx = -1e18; *maxy = -1e18;
    for (auto& sp : path) for (auto& p : sp.pts) {
        *minx = std::min(*minx, p.x); *maxx = std::max(*maxx, p.x);
        *miny = std::min(*miny, p.y); *maxy = std::max(*maxy, p.y);
        any = true;
    }
    return any;
}

// クリップ矩形へ [nx0,nx1)x[ny0,ny1) を積集合で反映する (clip() の実体)。
inline void IntersectClip(ClipRect& clip, double nx0, double ny0, double nx1, double ny1)
{
    if (clip.active) {
        clip.x0 = std::max(clip.x0, nx0);
        clip.y0 = std::max(clip.y0, ny0);
        clip.x1 = std::min(clip.x1, nx1);
        clip.y1 = std::min(clip.y1, ny1);
    } else {
        clip.active = true;
        clip.x0 = nx0; clip.y0 = ny0; clip.x1 = nx1; clip.y1 = ny1;
    }
}

// パスが単一の軸並行矩形か判定する (矩形なら外接矩形クリップで厳密に足りる)。
// beginPath→rect() は 4 or 5 点 (閉路点重複) の矩形サブパスになる。
inline bool IsAxisAlignedRect(const std::vector<SubPath>& path)
{
    if (path.size() != 1) return false;
    const std::vector<Point>& p = path[0].pts;
    // 末尾が始点と重複する 5 点表現も許容する
    size_t n = p.size();
    if (n == 5 && std::abs(p[4].x - p[0].x) < 1e-6 && std::abs(p[4].y - p[0].y) < 1e-6) {
        n = 4;
    }
    if (n != 4) return false;
    // 各辺が水平または垂直であること
    for (size_t i = 0; i < 4; ++i) {
        const Point& a = p[i];
        const Point& b = p[(i + 1) % 4];
        const bool horizontal = std::abs(a.y - b.y) < 1e-6;
        const bool vertical   = std::abs(a.x - b.x) < 1e-6;
        if (!horizontal && !vertical) return false;
    }
    return true;
}

// パスを nonzero winding でカバレッジマスク (surface 同寸, 1=内側) にラスタライズする。
// スキャンライン規約は FillPath と一致させる (塗りと同じ形にクリップされる)。
inline std::shared_ptr<std::vector<uint8_t>> RasterizePathMask(
    const std::vector<SubPath>& path, int width, int height)
{
    auto mask = std::make_shared<std::vector<uint8_t>>(
        static_cast<size_t>(width) * height, 0);
    if (width <= 0 || height <= 0) return mask;

    double miny = 1e18, maxy = -1e18;
    for (auto& sp : path) for (auto& p : sp.pts) { miny = std::min(miny,p.y); maxy = std::max(maxy,p.y); }
    if (maxy < miny) return mask;
    int y0 = std::max(0, static_cast<int>(std::floor(miny)));
    int y1 = std::min(height - 1, static_cast<int>(std::ceil(maxy)));
    for (int y = y0; y <= y1; ++y) {
        double sy = y + 0.5;
        std::vector<std::pair<double,int>> xs;
        for (auto& sp : path) {
            size_t n = sp.pts.size();
            if (n < 2) continue;
            for (size_t i = 0; i < n; ++i) {
                Point a = sp.pts[i];
                Point b = sp.pts[(i+1) % n];
                if ((a.y <= sy && b.y > sy) || (b.y <= sy && a.y > sy)) {
                    double t = (sy - a.y) / (b.y - a.y);
                    xs.push_back({a.x + t * (b.x - a.x), a.y < b.y ? 1 : -1});
                }
            }
        }
        std::sort(xs.begin(), xs.end());
        int wind = 0;
        for (size_t i = 0; i + 1 < xs.size(); ++i) {
            wind += xs[i].second;
            if (wind != 0) {
                int xa = std::max(0, static_cast<int>(std::floor(xs[i].first)));
                int xb = std::min(width - 1, static_cast<int>(std::ceil(xs[i+1].first)));
                for (int x = xa; x <= xb; ++x) {
                    (*mask)[static_cast<size_t>(y) * width + x] = 1;
                }
            }
        }
    }
    return mask;
}

// 任意形状パスをクリップへ積集合で反映する (非矩形 clip() の実体)。
// 外接矩形 (高速リジェクト) も同時に反映し、既存 mask があれば AND を取って
// 新しい mask を割り当てる (save/restore 安全)。
inline void IntersectClipMask(ClipRect& clip, const std::vector<SubPath>& path,
                              int width, int height)
{
    double minx, miny, maxx, maxy;
    if (PathBounds(path, &minx, &miny, &maxx, &maxy)) {
        IntersectClip(clip, std::floor(minx), std::floor(miny),
                      std::ceil(maxx), std::ceil(maxy));
    }
    auto next = RasterizePathMask(path, width, height);
    if (clip.mask) {
        const std::vector<uint8_t>& prev = *clip.mask;
        const size_t n = std::min(next->size(), prev.size());
        for (size_t i = 0; i < n; ++i) {
            if (prev[i] == 0) { (*next)[i] = 0; }
        }
    }
    clip.mask = next;
    clip.mask_w = width;
    clip.mask_h = height;
}

// 3次ベジェを現在サブパスへフラット化して追記する。p0 は変換済みの直前点。
inline void FlattenCubic(std::vector<Point>& out,
                         Point p0, Point p1, Point p2, Point p3, int steps = 16)
{
    for (int i = 1; i <= steps; ++i) {
        double t = static_cast<double>(i) / steps, u = 1 - t;
        out.push_back({
            u*u*u*p0.x + 3*u*u*t*p1.x + 3*u*t*t*p2.x + t*t*t*p3.x,
            u*u*u*p0.y + 3*u*u*t*p1.y + 3*u*t*t*p2.y + t*t*t*p3.y });
    }
}

// 2次ベジェを現在サブパスへフラット化して追記する。
inline void FlattenQuadratic(std::vector<Point>& out, Point p0, Point p1, Point p2, int steps = 12)
{
    for (int i = 1; i <= steps; ++i) {
        double t = static_cast<double>(i) / steps, u = 1 - t;
        out.push_back({
            u*u*p0.x + 2*u*t*p1.x + t*t*p2.x,
            u*u*p0.y + 2*u*t*p1.y + t*t*p2.y });
    }
}

// arc() を変換適用済みの点列へフラット化して追記する。ccw の巻き戻しも処理。
inline void FlattenArc(std::vector<Point>& out, const Mat& m,
                       double cx, double cy, double r,
                       double start, double end, bool ccw, int steps = 32)
{
    if (ccw && end > start) end -= 2 * kPi;
    if (!ccw && end < start) end += 2 * kPi;
    for (int i = 0; i <= steps; ++i) {
        double t = start + (end - start) * (static_cast<double>(i) / steps);
        out.push_back(Apply(m, cx + std::cos(t) * r, cy + std::sin(t) * r));
    }
}

// clearRect: 矩形を透明で塗りつぶす (クリップ非適用、Canvas 仕様どおり全消去)。
// 範囲を surface の中に切り詰めてから、行ごとにまとめて消す。1 ピクセルごとに範囲を判定して
// 4 バイトずつ書くと、キャンバス全体を消す呼び出し (player がキャンバスを使い回すたびに行う) で
// 画像 1 枚分の時間がかかる。消える範囲は、切り詰める前と同じ
// (x, y を 0 方向へ丸めた位置から、x + w, y + h 未満の整数まで)。
inline void ClearRect(Surface& s, double x, double y, double w, double h)
{
    // 有限でない値は何もしない (HTML の仕様どおり)
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h) ||
        s.width <= 0 || s.height <= 0) {
        return;
    }
    // 開始位置は 0 方向への丸め、終了位置は x + w (y + h) 以上の最小の整数
    const double x_begin = std::trunc(x), y_begin = std::trunc(y);
    const double x_end = std::ceil(x + w), y_end = std::ceil(y + h);
    const int x0 = static_cast<int>(std::clamp(x_begin, 0.0, static_cast<double>(s.width)));
    const int y0 = static_cast<int>(std::clamp(y_begin, 0.0, static_cast<double>(s.height)));
    const int x1 = static_cast<int>(std::clamp(x_end, 0.0, static_cast<double>(s.width)));
    const int y1 = static_cast<int>(std::clamp(y_end, 0.0, static_cast<double>(s.height)));
    if (x1 <= x0 || y1 <= y0 || s.Blank()) {
        return;   // 範囲が空、またはまだ何も描いていない (すでに全面が透明)
    }
    s.Ensure();
    const size_t row_bytes = static_cast<size_t>(x1 - x0) * 4;
    for (int yy = y0; yy < y1; ++yy) {
        std::memset(&s.pixels[(static_cast<size_t>(yy) * s.width + x0) * 4], 0, row_bytes);
    }
}

} // namespace next2d::raster
