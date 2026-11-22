// RasterCore.h (Canvas2D ソフトラスタライザ純ロジック) の単体テスト。
// V8 / Windows 不要。macOS/Linux では:
//   c++ -std=c++17 -O1 -o raster_test tests/raster_test.cpp && ./raster_test
// Windows (MSVC) では:
//   cl /std:c++17 /EHsc tests\raster_test.cpp && raster_test.exe
#include "../src/bindings/RasterCore.h"

#include <cmath>
#include <cstdio>

using namespace next2d::raster;

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { ++checks; if (!(cond)) { \
    ++failures; std::printf("not ok - %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define CHECK_NEAR(a, b, eps) do { ++checks; if (std::fabs((a) - (b)) > (eps)) { \
    ++failures; std::printf("not ok - %s:%d: %s=%f != %s=%f\n", \
        __FILE__, __LINE__, #a, static_cast<double>(a), #b, static_cast<double>(b)); } } while (0)

// (x,y) のピクセルを取得
static RGBA Px(const Surface& s, int x, int y)
{
    if (s.Blank()) {
        return { 0, 0, 0, 0 };   // まだ何も描いていない: 全面が透明
    }
    size_t i = (static_cast<size_t>(y) * s.width + x) * 4;
    return { s.pixels[i], s.pixels[i+1], s.pixels[i+2], s.pixels[i+3] };
}

static SubPath MakeRectPath(double x, double y, double w, double h, bool reverse = false)
{
    SubPath sp;
    if (!reverse) {
        sp.pts = { {x,y}, {x+w,y}, {x+w,y+h}, {x,y+h} };
    } else {
        sp.pts = { {x,y}, {x,y+h}, {x+w,y+h}, {x+w,y} };
    }
    sp.closed = true;
    return sp;
}

static void TestParseColor()
{
    RGBA c = ParseColor("#fff");
    CHECK(c.r == 255 && c.g == 255 && c.b == 255 && c.a == 255);
    c = ParseColor("#ff8000");
    CHECK(c.r == 255 && c.g == 128 && c.b == 0);
    c = ParseColor("rgb(10, 20, 30)");
    CHECK(c.r == 10 && c.g == 20 && c.b == 30 && c.a == 255);
    c = ParseColor("rgba(10, 20, 30, 0.5)");
    CHECK(c.r == 10 && c.g == 20 && c.b == 30 && c.a == 127);
    c = ParseColor("");
    CHECK(c.r == 0 && c.g == 0 && c.b == 0 && c.a == 255);
}

static void TestFontPixelSize()
{
    CHECK_NEAR(FontPixelSize("10px sans-serif"), 10.0, 1e-9);
    CHECK_NEAR(FontPixelSize("bold 24px Arial"), 24.0, 1e-9);
    CHECK_NEAR(FontPixelSize("italic 1.5px x"), 1.5, 1e-9);
    CHECK_NEAR(FontPixelSize("no-size"), 10.0, 1e-9);
}

static void TestMatrix()
{
    // translate(10,0) してから scale(2,2): Canvas と同じく後から掛けた変換が先に適用される
    Mat m;
    m = Multiply(m, {1,0,0,1,10,0});
    m = Multiply(m, {2,0,0,2,0,0});
    Point p = Apply(m, 1, 1);
    CHECK_NEAR(p.x, 12.0, 1e-9);
    CHECK_NEAR(p.y, 2.0, 1e-9);

    // 90 度回転 (a=cos,b=sin,c=-sin,d=cos)
    const double r = kPi / 2;
    Mat rot = { std::cos(r), std::sin(r), -std::sin(r), std::cos(r), 0, 0 };
    Point q = Apply(rot, 1, 0);
    CHECK_NEAR(q.x, 0.0, 1e-9);
    CHECK_NEAR(q.y, 1.0, 1e-9);
}

static void TestFillRectPath()
{
    Surface s(100, 100);
    ClipRect clip;
    std::vector<SubPath> path = { MakeRectPath(10, 10, 10, 10) };
    FillPath(s, clip, path, {255, 0, 0, 255}, 1.0);
    CHECK(Px(s, 15, 15).r == 255 && Px(s, 15, 15).a == 255);  // 内側
    CHECK(Px(s, 5, 15).a == 0);                               // 左外
    CHECK(Px(s, 25, 15).a == 0);                              // 右外
    CHECK(Px(s, 15, 25).a == 0);                              // 下外
}

static void TestFillImplicitClose()
{
    // 未クローズの三角形 (moveTo/lineTo のみ) — Canvas 仕様では fill 時に暗黙クローズ
    Surface s(100, 100);
    ClipRect clip;
    SubPath tri;
    tri.pts = { {10,10}, {50,10}, {30,40} };
    tri.closed = false;
    std::vector<SubPath> path = { tri };
    FillPath(s, clip, path, {0, 255, 0, 255}, 1.0);
    CHECK(Px(s, 30, 20).g == 255);   // 重心付近は塗られる
    CHECK(Px(s, 12, 35).a == 0);     // 三角形の外
}

static void TestNonzeroWindingDonut()
{
    // 外周 CW + 内周 CCW → 穴には塗らない (nonzero)
    Surface s(100, 100);
    ClipRect clip;
    std::vector<SubPath> path = {
        MakeRectPath(10, 10, 50, 50, false),  // 外周
        MakeRectPath(20, 20, 30, 30, true),   // 内周 (逆回り)
    };
    FillPath(s, clip, path, {0, 0, 255, 255}, 1.0);
    CHECK(Px(s, 15, 35).b == 255);   // リング部
    CHECK(Px(s, 35, 35).a == 0);     // 穴

    // isPointInPath も同じ nonzero 判定
    CHECK(PointInPath(path, 15, 35));
    CHECK(!PointInPath(path, 35, 35));
    CHECK(!PointInPath(path, 90, 90));
}

static void TestClip()
{
    Surface s(100, 100);
    ClipRect clip;
    IntersectClip(clip, 10, 10, 20, 20);
    CHECK(clip.active);

    std::vector<SubPath> path = { MakeRectPath(0, 0, 100, 100) };
    FillPath(s, clip, path, {255, 0, 0, 255}, 1.0);
    CHECK(Px(s, 15, 15).r == 255);   // クリップ内
    CHECK(Px(s, 25, 15).a == 0);     // クリップ外 (右)
    CHECK(Px(s, 5, 5).a == 0);       // クリップ外 (左上)

    // 2 回目の clip は積集合
    IntersectClip(clip, 0, 0, 15, 100);
    CHECK_NEAR(clip.x0, 10.0, 1e-9);
    CHECK_NEAR(clip.x1, 15.0, 1e-9);
    Surface s2(100, 100);
    FillPath(s2, clip, path, {255, 0, 0, 255}, 1.0);
    CHECK(Px(s2, 12, 15).r == 255);
    CHECK(Px(s2, 17, 15).a == 0);    // 積集合で除外
}

static void TestClearRect()
{
    Surface s(50, 50);
    ClipRect clip;
    std::vector<SubPath> path = { MakeRectPath(0, 0, 50, 50) };
    FillPath(s, clip, path, {255, 255, 255, 255}, 1.0);
    ClearRect(s, 10, 10, 10, 10);
    CHECK(Px(s, 15, 15).a == 0);     // クリア済み
    CHECK(Px(s, 25, 25).r == 255);   // クリア外は残る
}

static void TestStroke()
{
    Surface s(50, 50);
    ClipRect clip;
    SubPath line;
    line.pts = { {5, 10}, {40, 10} };
    line.closed = false;
    std::vector<SubPath> path = { line };
    StrokePath(s, clip, path, {255, 0, 255, 255}, 1.0);
    CHECK(Px(s, 20, 10).r == 255 && Px(s, 20, 10).b == 255);
    CHECK(Px(s, 20, 20).a == 0);
    // 未クローズなので終点→始点の辺は描かれない (stroke は暗黙クローズしない)
    CHECK(Px(s, 45, 10).a == 0);
}

static void TestBlendAlpha()
{
    Surface s(4, 4);
    ClipRect clip;
    // 下地: 白
    Blend(s, clip, 1, 1, {255, 255, 255, 255}, 1.0);
    // 50% の赤を重ねる
    Blend(s, clip, 1, 1, {255, 0, 0, 255}, 0.5);
    RGBA p = Px(s, 1, 1);
    CHECK(p.r == 255);
    CHECK(p.g >= 126 && p.g <= 128);
    CHECK(p.b >= 126 && p.b <= 128);

    // クリップ外は書き込まれない
    IntersectClip(clip, 0, 0, 1, 1);
    Blend(s, clip, 2, 2, {255, 0, 0, 255}, 1.0);
    CHECK(Px(s, 2, 2).a == 0);
}

static void TestFlattenCurves()
{
    // 2次ベジェ: 終点が p2 に一致
    std::vector<Point> out;
    FlattenQuadratic(out, {0,0}, {5,10}, {10,0});
    CHECK(!out.empty());
    CHECK_NEAR(out.back().x, 10.0, 1e-9);
    CHECK_NEAR(out.back().y, 0.0, 1e-9);

    // 3次ベジェ: 終点が p3 に一致
    out.clear();
    FlattenCubic(out, {0,0}, {0,10}, {10,10}, {10,0});
    CHECK_NEAR(out.back().x, 10.0, 1e-9);
    CHECK_NEAR(out.back().y, 0.0, 1e-9);

    // 全周円: 始点と終点が一致し半径を保つ
    out.clear();
    Mat identity;
    FlattenArc(out, identity, 50, 50, 10, 0, 2 * kPi, false);
    CHECK(out.size() == 33);
    CHECK_NEAR(out.front().x, out.back().x, 1e-6);
    CHECK_NEAR(out.front().y, out.back().y, 1e-6);
    for (auto& p : out) {
        CHECK_NEAR(std::hypot(p.x - 50, p.y - 50), 10.0, 1e-6);
    }

    // ccw で end > start の場合は 2π 巻き戻す (過去に壊れていた経路)
    out.clear();
    FlattenArc(out, identity, 0, 0, 1, 0, kPi / 2, true);
    // 終点は角度 π/2 - 2π = -3π/2 → (cos,sin) = (0, 1)
    CHECK_NEAR(out.back().x, 0.0, 1e-6);
    CHECK_NEAR(out.back().y, 1.0, 1e-6);
}

static void TestPathBounds()
{
    std::vector<SubPath> path = { MakeRectPath(5, 7, 10, 20) };
    double x0, y0, x1, y1;
    CHECK(PathBounds(path, &x0, &y0, &x1, &y1));
    CHECK_NEAR(x0, 5.0, 1e-9);
    CHECK_NEAR(y0, 7.0, 1e-9);
    CHECK_NEAR(x1, 15.0, 1e-9);
    CHECK_NEAR(y1, 27.0, 1e-9);

    std::vector<SubPath> empty;
    CHECK(!PathBounds(empty, &x0, &y0, &x1, &y1));
}

static void TestTransformedFill()
{
    // setTransform(2,0,0,2,10,10) で組んだ 5x5 矩形 → デバイス座標 20..30
    Surface s(64, 64);
    ClipRect clip;
    Mat m = {2, 0, 0, 2, 10, 10};
    SubPath sp;
    const double corners[4][2] = { {5,5}, {10,5}, {10,10}, {5,10} };
    for (auto& c : corners) sp.pts.push_back(Apply(m, c[0], c[1]));
    sp.closed = true;
    std::vector<SubPath> path = { sp };
    FillPath(s, clip, path, {255, 128, 0, 255}, 1.0);
    CHECK(Px(s, 25, 25).r == 255);
    CHECK(Px(s, 15, 15).a == 0);
    CHECK(Px(s, 33, 25).a == 0);
}

// 任意形状クリップ (ピクセルマスク)。矩形は外接矩形、非矩形はマスクで厳密クリップ。
static void TestClipMask()
{
    // 矩形パスは mask を作らない (IsAxisAlignedRect が true)。
    std::vector<SubPath> rect = { MakeRectPath(2, 2, 5, 5) };
    CHECK(IsAxisAlignedRect(rect));

    // 左上三角 (0,0),(10,0),(0,10) — 内側は x+y<10。
    std::vector<SubPath> tri(1);
    tri[0].pts = { {0,0}, {10,0}, {0,10} };
    tri[0].closed = true;
    CHECK(!IsAxisAlignedRect(tri));

    ClipRect clip;
    IntersectClipMask(clip, tri, 10, 10);
    CHECK(clip.mask != nullptr);
    CHECK(clip.mask_w == 10 && clip.mask_h == 10);

    // 全面塗りが三角形にクリップされる。
    Surface s(10, 10);
    std::vector<SubPath> full = { MakeRectPath(0, 0, 10, 10) };
    FillPath(s, clip, full, {255, 0, 0, 255}, 1.0);
    CHECK(Px(s, 1, 1).r == 255);   // 深く内側
    CHECK(Px(s, 1, 2).r == 255);
    CHECK(Px(s, 9, 9).a == 0);     // 深く外側 (x+y=18)
    CHECK(Px(s, 8, 8).a == 0);     // 外側 (x+y=16)

    // Blend も直接マスクに従う。
    Surface s2(10, 10);
    Blend(s2, clip, 1, 1, {0, 255, 0, 255}, 1.0);   // 内側 → 描かれる
    Blend(s2, clip, 9, 9, {0, 255, 0, 255}, 1.0);   // 外側 → 描かれない
    CHECK(Px(s2, 1, 1).g == 255);
    CHECK(Px(s2, 9, 9).a == 0);

    // マスク同士の積集合: 左上三角 ∩ 右下三角 は (1,1) を除外する。
    ClipRect clip2;
    IntersectClipMask(clip2, tri, 10, 10);
    std::vector<SubPath> tri2(1);
    tri2[0].pts = { {10,0}, {10,10}, {0,10} };   // 右下三角 (内側 x+y>10)
    tri2[0].closed = true;
    IntersectClipMask(clip2, tri2, 10, 10);
    Surface s3(10, 10);
    FillPath(s3, clip2, full, {0, 0, 255, 255}, 1.0);
    CHECK(Px(s3, 1, 1).a == 0);    // 左上三角内だが右下三角外 → 積集合で除外

    // 非矩形クリップでも外接矩形 (高速リジェクト) は反映される。
    CHECK(clip.active);
}

// CopyImage は、各ピクセルを Blend(..., 1.0) で合成した結果と一致する (不透明 / 透明 / 半透明、
// surface からのはみ出しを含む)。
static void TestCopyImage()
{
    const uint32_t sw = 7, sh = 5;
    std::vector<uint8_t> image(static_cast<size_t>(sw) * sh * 4);
    for (uint32_t y = 0; y < sh; ++y) {
        for (uint32_t x = 0; x < sw; ++x) {
            uint8_t* p = &image[(static_cast<size_t>(y) * sw + x) * 4];
            p[0] = static_cast<uint8_t>(x * 30 + 10);
            p[1] = static_cast<uint8_t>(y * 40 + 20);
            p[2] = static_cast<uint8_t>((x + y) * 17);
            // 列ごとに 不透明 / 透明 / 半透明 を混ぜる
            p[3] = (x % 3 == 0) ? 255 : (x % 3 == 1) ? 0 : static_cast<uint8_t>(60 + y * 30);
        }
    }

    const int offsets[][2] = { {0, 0}, {2, 1}, {-3, -2}, {6, 4}, {-7, 0}, {0, -5}, {20, 20} };
    for (const auto& offset : offsets) {
        Surface fast(8, 6), slow(8, 6);
        // 合成先を空でない状態にしておく
        fast.Ensure();
        slow.Ensure();
        for (size_t i = 0; i < fast.pixels.size(); ++i) {
            fast.pixels[i] = slow.pixels[i] = static_cast<uint8_t>(i * 7 + 3);
        }
        CopyImage(fast, image.data(), sw, sh, offset[0], offset[1]);
        const ClipRect no_clip;
        for (uint32_t y = 0; y < sh; ++y) {
            for (uint32_t x = 0; x < sw; ++x) {
                const uint8_t* p = &image[(static_cast<size_t>(y) * sw + x) * 4];
                Blend(slow, no_clip, offset[0] + static_cast<int>(x), offset[1] + static_cast<int>(y),
                      RGBA{p[0], p[1], p[2], p[3]}, 1.0);
            }
        }
        CHECK(fast.pixels == slow.pixels);
    }

    // まだ何も描いていない surface へ、同じ大きさの画像をぴったり重ねる (0 で埋めずに作る経路)
    {
        Surface fast(static_cast<int>(sw), static_cast<int>(sh)), slow(static_cast<int>(sw), static_cast<int>(sh));
        CHECK(fast.Blank());
        CopyImage(fast, image.data(), sw, sh, 0, 0);
        CHECK(!fast.Blank());
        CHECK(fast.pixels.size() == fast.ByteSize());
        const ClipRect no_clip;
        for (uint32_t y = 0; y < sh; ++y) {
            for (uint32_t x = 0; x < sw; ++x) {
                const uint8_t* p = &image[(static_cast<size_t>(y) * sw + x) * 4];
                Blend(slow, no_clip, static_cast<int>(x), static_cast<int>(y), RGBA{p[0], p[1], p[2], p[3]}, 1.0);
            }
        }
        CHECK(fast.pixels == slow.pixels);
    }

    // まだ何も描いていない surface でも、大きさや位置が合わなければ通常の経路 (結果は同じ)
    for (const auto& offset : offsets) {
        Surface fast(8, 6), slow(8, 6);
        slow.Ensure();
        CopyImage(fast, image.data(), sw, sh, offset[0], offset[1]);
        const ClipRect no_clip;
        for (uint32_t y = 0; y < sh; ++y) {
            for (uint32_t x = 0; x < sw; ++x) {
                const uint8_t* p = &image[(static_cast<size_t>(y) * sw + x) * 4];
                Blend(slow, no_clip, offset[0] + static_cast<int>(x), offset[1] + static_cast<int>(y),
                      RGBA{p[0], p[1], p[2], p[3]}, 1.0);
            }
        }
        fast.Ensure();
        CHECK(fast.pixels == slow.pixels);
    }
}

// 画像を写しただけの状態 (Surface::source): 実体にすると、透明の上に合成した結果になる
static void TestSurfaceSource()
{
    const int w = 5, h = 3;
    std::vector<uint8_t> image(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < image.size() / 4; ++i) {
        image[i * 4] = static_cast<uint8_t>(i * 13 + 5);
        image[i * 4 + 1] = static_cast<uint8_t>(i * 29 + 7);
        image[i * 4 + 2] = static_cast<uint8_t>(i * 3 + 200);
        image[i * 4 + 3] = (i % 3 == 0) ? 255 : (i % 3 == 1) ? 0 : static_cast<uint8_t>(40 + i * 11);
    }
    Surface expected(w, h);
    const ClipRect no_clip;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = &image[(static_cast<size_t>(y) * w + x) * 4];
            Blend(expected, no_clip, x, y, RGBA{p[0], p[1], p[2], p[3]}, 1.0);
        }
    }

    // そのまま読む (canvas のピクセルを作らずに読み出す経路と同じ計算)
    std::vector<uint8_t> direct(image.size(), 0xAB);
    CompositeOntoTransparent(direct.data(), image.data(), static_cast<size_t>(w) * h);
    CHECK(direct == expected.pixels);

    // 実体にする
    Surface s(w, h);
    s.source = image.data();
    CHECK(!s.Blank());
    CHECK(s.pixels.empty());
    s.Ensure();
    CHECK(s.source == nullptr);
    CHECK(s.pixels == expected.pixels);

    // 上から描くと、実体にしてから合成される
    Surface drawn(w, h), drawn_expected(w, h);
    drawn_expected.pixels = expected.pixels;
    drawn.source = image.data();
    Blend(drawn, no_clip, 1, 1, RGBA{10, 20, 30, 128}, 1.0);
    Blend(drawn_expected, no_clip, 1, 1, RGBA{10, 20, 30, 128}, 1.0);
    CHECK(drawn.pixels == drawn_expected.pixels);

    // 消すときも、実体にしてから消す
    Surface cleared(w, h), cleared_expected(w, h);
    cleared_expected.pixels = expected.pixels;
    cleared.source = image.data();
    ClearRect(cleared, 0, 0, 2, 2);
    ClearRect(cleared_expected, 0, 0, 2, 2);
    CHECK(cleared.pixels == cleared_expected.pixels);

    // 何も描いていない状態へ戻す
    Surface reset(w, h);
    reset.source = image.data();
    reset.Clear();
    CHECK(reset.Blank());
}

// 何も描いていない surface は確保しない。消しても確保しない。描いたら確保する
static void TestLazySurface()
{
    Surface s(16, 16);
    CHECK(s.Blank());
    ClearRect(s, 0, 0, 16, 16);
    CHECK(s.Blank());
    const ClipRect no_clip;
    Blend(s, no_clip, 100, 100, RGBA{255, 0, 0, 255}, 1.0);   // 範囲外: 描かない
    CHECK(s.Blank());
    Blend(s, no_clip, 3, 4, RGBA{255, 0, 0, 255}, 1.0);
    CHECK(!s.Blank());
    CHECK(s.pixels.size() == s.ByteSize());
    CHECK(s.pixels[(4 * 16 + 3) * 4] == 255);
    CHECK(s.pixels[0] == 0);
}

int main()
{
    TestCopyImage();
    TestLazySurface();
    TestSurfaceSource();
    TestParseColor();
    TestFontPixelSize();
    TestMatrix();
    TestFillRectPath();
    TestFillImplicitClose();
    TestNonzeroWindingDonut();
    TestClip();
    TestClearRect();
    TestStroke();
    TestBlendAlpha();
    TestFlattenCurves();
    TestPathBounds();
    TestTransformedFill();
    TestClipMask();

    std::printf("%s: %d checks, %d failures\n",
                failures ? "FAILED" : "ok", checks, failures);
    return failures ? 1 : 0;
}
