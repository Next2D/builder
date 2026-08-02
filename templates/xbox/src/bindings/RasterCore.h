// Canvas2D のソフトウェアラスタライザ純ロジック。
// V8 / Windows API に依存しないため、macOS/Linux でも単体テストできる
// (tests/raster_test.cpp)。Canvas2D.cpp はこのヘッダの薄いバインディング層。
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
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
struct Surface {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;

    Surface(int w, int h)
        : width(w), height(h), pixels(static_cast<size_t>(w) * h * 4, 0) {}
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
    size_t i = (static_cast<size_t>(y) * s.width + x) * 4;
    double a = (col.a / 255.0) * alpha;
    s.pixels[i+0] = static_cast<uint8_t>(col.r * a + s.pixels[i+0] * (1 - a));
    s.pixels[i+1] = static_cast<uint8_t>(col.g * a + s.pixels[i+1] * (1 - a));
    s.pixels[i+2] = static_cast<uint8_t>(col.b * a + s.pixels[i+2] * (1 - a));
    s.pixels[i+3] = static_cast<uint8_t>(255 * a + s.pixels[i+3] * (1 - a));
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
inline void ClearRect(Surface& s, double x, double y, double w, double h)
{
    for (int yy = static_cast<int>(y); yy < y + h; ++yy)
        for (int xx = static_cast<int>(x); xx < x + w; ++xx)
            if (xx >= 0 && yy >= 0 && xx < s.width && yy < s.height) {
                size_t i = (static_cast<size_t>(yy) * s.width + xx) * 4;
                s.pixels[i] = s.pixels[i+1] = s.pixels[i+2] = s.pixels[i+3] = 0;
            }
}

} // namespace next2d::raster
