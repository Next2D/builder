// CanvasRenderingContext2D の最小実装。
//
// player の実使用は「パスベースのヒット判定(setTransform/beginPath/moveTo/lineTo/arc/
// isPointInPath)」が中心で、fill/stroke はメッシュ生成やテキスト下地に使われる。
// ラスタライズ純ロジックは RasterCore.h (V8/Windows 非依存、単体テスト対象)。
// このファイルは V8 バインディングと DirectWrite/Direct2D テキスト描画の層。
#include "Bindings.h"

#include "HostContext.h"
#include "ImageSource.h"
#include "RasterCore.h"
#include "platform/TextRasterizer.h"
#include "v8/V8Util.h"
#include "v8/WeakHandle.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace next2d {

using v8util::Str;
using v8util::SetMethod;
using v8util::SetValue;
using v8util::ToStdString;

namespace {

using raster::Mat;
using raster::Point;
using raster::RGBA;
using raster::SubPath;
using raster::ParseColor;
using raster::FontPixelSize;
using raster::Apply;
using raster::Multiply;

struct State {
    Mat transform;
    RGBA fill{0,0,0,255};
    RGBA stroke{0,0,0,255};
    double line_width = 1.0;
    double global_alpha = 1.0;
    std::string font = "10px sans-serif";
    // クリップ矩形 (デバイス座標)。TextField はテキスト枠(矩形パス)を clip() で
    // 指定して文字のはみ出しを抑える。
    raster::ClipRect clip;

    // --- CRC2D の完全な描画状態 (getter/setter で往復可能にする) ---
    // ラスタライズへ実際に反映するもの(textAlign/textBaseline 等)と、
    // 状態として保持するだけのもの(shadow/composite 等の近似)を含む。
    std::string fill_style_str   = "#000000";
    std::string stroke_style_str = "#000000";
    std::string line_cap   = "butt";
    std::string line_join  = "miter";
    double      miter_limit = 10.0;
    double      line_dash_offset = 0.0;
    std::vector<double> line_dash;
    double      shadow_blur = 0.0;
    std::string shadow_color = "rgba(0, 0, 0, 0)";
    double      shadow_offset_x = 0.0;
    double      shadow_offset_y = 0.0;
    std::string text_align    = "start";
    std::string text_baseline = "alphabetic";
    std::string direction     = "inherit";
    std::string global_composite_operation = "source-over";
    bool        image_smoothing_enabled = true;
    std::string image_smoothing_quality = "low";
    std::string filter = "none";
    std::string font_kerning = "auto";
    std::string letter_spacing = "0px";
    std::string word_spacing = "0px";
    std::string text_rendering = "auto";
};

// createLinearGradient / createPattern 等が返す塗りオブジェクトの実体。
// fillStyle/strokeStyle へ代入された際に代表色 (RepresentativeColor) を取り出す。
struct PaintObject {
    enum Kind { Gradient, Pattern } kind = Gradient;
    std::vector<std::pair<double, RGBA>> stops;  // gradient
    RGBA sample{128,128,128,255};                // pattern の代表色
};

// 2D コンテキストの実体。JS オブジェクトの内部フィールドに保持する。
struct Canvas2D {
    raster::Surface surface;
    std::vector<SubPath> path;
    State state;
    std::vector<State> stack;
    // 現在のパス開始点 (arcTo/未サブパス時の moveTo 補完用)
    double cur_x = 0.0, cur_y = 0.0;

    Canvas2D(int w, int h) : surface(w, h) {}
};

// --- 内部アクセサ ---------------------------------------------------------
Canvas2D* Self(v8::Local<v8::Object> obj)
{
    return static_cast<Canvas2D*>(obj->GetInternalField(0).As<v8::External>()->Value());
}

double Arg(const v8::FunctionCallbackInfo<v8::Value>& a, int i, double fb = 0.0)
{
    return (i < a.Length() && a[i]->IsNumber()) ? a[i].As<v8::Number>()->Value() : fb;
}

void Blend(Canvas2D* c, int x, int y, const RGBA& col, double alpha)
{
    raster::Blend(c->surface, c->state.clip, x, y, col, alpha);
}

// 現在の変換を適用して点をパスに追加
void AddPoint(Canvas2D* c, double x, double y, bool new_sub)
{
    Point p = Apply(c->state.transform, x, y);
    if (new_sub || c->path.empty()) {
        c->path.push_back(SubPath{});
    }
    c->path.back().pts.push_back(p);
    c->cur_x = x;   // arcTo 等が参照する現在点 (ユーザー座標)
    c->cur_y = y;
}

// 値 (色文字列 or gradient/pattern オブジェクト) を代表 RGBA へ解決する。
RGBA ColorFromValue(v8::Isolate* iso, v8::Local<v8::Value> value)
{
    if (value->IsObject()) {
        v8::Local<v8::Object> o = value.As<v8::Object>();
        if (o->InternalFieldCount() >= 1) {
            v8::Local<v8::Value> ext = o->GetInternalField(0).As<v8::Value>();
            if (ext->IsExternal()) {
                auto* paint = static_cast<PaintObject*>(ext.As<v8::External>()->Value());
                if (paint) {
                    if (paint->kind == PaintObject::Gradient && !paint->stops.empty()) {
                        // 代表色 = 全ストップの平均 (近似)。
                        uint32_t r=0,g=0,b=0,al=0;
                        for (auto& s : paint->stops) { r+=s.second.r; g+=s.second.g; b+=s.second.b; al+=s.second.a; }
                        const uint32_t n = static_cast<uint32_t>(paint->stops.size());
                        return RGBA{ uint8_t(r/n), uint8_t(g/n), uint8_t(b/n), uint8_t(al/n) };
                    }
                    return paint->sample;
                }
            }
        }
    }
    return ParseColor(ToStdString(iso, value));
}

// --- パス構築 -------------------------------------------------------------
void BeginPath(const v8::FunctionCallbackInfo<v8::Value>& a) { Self(a.This())->path.clear(); }
void MoveTo(const v8::FunctionCallbackInfo<v8::Value>& a) { AddPoint(Self(a.This()), Arg(a,0), Arg(a,1), true); }
void LineTo(const v8::FunctionCallbackInfo<v8::Value>& a) { AddPoint(Self(a.This()), Arg(a,0), Arg(a,1), false); }
void ClosePath(const v8::FunctionCallbackInfo<v8::Value>& a) {
    Canvas2D* c = Self(a.This());
    if (!c->path.empty()) c->path.back().closed = true;
}

void BezierCurveTo(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    Canvas2D* c = Self(a.This());
    if (c->path.empty() || c->path.back().pts.empty()) AddPoint(c, 0, 0, true);
    // 直前点(変換済み)から制御点(変換して)へフラット化
    Point p0 = c->path.back().pts.back();
    Point p1 = Apply(c->state.transform, Arg(a,0), Arg(a,1));
    Point p2 = Apply(c->state.transform, Arg(a,2), Arg(a,3));
    Point p3 = Apply(c->state.transform, Arg(a,4), Arg(a,5));
    raster::FlattenCubic(c->path.back().pts, p0, p1, p2, p3);
}

void QuadraticCurveTo(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    Canvas2D* c = Self(a.This());
    if (c->path.empty() || c->path.back().pts.empty()) AddPoint(c, 0, 0, true);
    Point p0 = c->path.back().pts.back();
    Point p1 = Apply(c->state.transform, Arg(a,0), Arg(a,1));
    Point p2 = Apply(c->state.transform, Arg(a,2), Arg(a,3));
    raster::FlattenQuadratic(c->path.back().pts, p0, p1, p2);
}

void Arc(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    Canvas2D* c = Self(a.This());
    bool ccw = a.Length() > 5 && a[5]->BooleanValue(a.GetIsolate());
    // 既存サブパスが無ければ開始。あれば現在サブパスへ連結する。
    if (c->path.empty()) c->path.push_back(SubPath{});
    raster::FlattenArc(c->path.back().pts, c->state.transform,
                       Arg(a,0), Arg(a,1), Arg(a,2), Arg(a,3), Arg(a,4), ccw);
}

void Rect(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    Canvas2D* c = Self(a.This());
    double x = Arg(a,0), y = Arg(a,1), w = Arg(a,2), h = Arg(a,3);
    c->path.push_back(SubPath{});
    const double corners[4][2] = { {x,y}, {x+w,y}, {x+w,y+h}, {x,y+h} };
    for (auto& pt : corners) {
        c->path.back().pts.push_back(Apply(c->state.transform, pt[0], pt[1]));
    }
    c->path.back().closed = true;
}

// --- ラスタライズ ---------------------------------------------------------
void Fill(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    Canvas2D* c = Self(a.This());
    raster::FillPath(c->surface, c->state.clip, c->path, c->state.fill, c->state.global_alpha);
}

void Stroke(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    Canvas2D* c = Self(a.This());
    raster::StrokePath(c->surface, c->state.clip, c->path, c->state.stroke, c->state.global_alpha);
}

// isPointInPath(x, y): nonzero winding。ヒット判定の中核。
// 引数は「変換後(デバイス)座標」。player は setTransform でパスを組んだのち
// デバイス座標で判定するため、パスへは変換適用済み。
void IsPointInPath(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    // «診断» 暴走ループ検出。player のメッシュ生成 (fill/stroke) は isPointInPath を
    // 多数呼ぶが、正常なら 1 フレームで収束する。ローディングで固まる #2 が
    // 「メッシュ生成の無限ループ」なら、この総数ログがフリーズ直前に高速連発する
    // (ハートビートが止まった後も総数だけ伸び続ける)。[Hit] 診断はキャップ後に
    // 出ないため、キャップ後の暴走を捉えるにはこの総数ログが必要。
    {
        static uint64_t s_total = 0;
        if (++s_total % 50000 == 0) {
            char b[72];
            std::snprintf(b, sizeof(b), "[hittest] isPointInPath total=%llu",
                          static_cast<unsigned long long>(s_total));
            v8util::AppendErrorLog(b);
        }
    }

    Canvas2D* c = Self(a.This());
    const double px = Arg(a,0), py = Arg(a,1);
    const bool hit = raster::PointInPath(c->path, px, py);

    // 診断: ヒットテストのクエリ点・パス外接矩形・結果を記録する。
    // 「pointermove は反応するがボタン(pointerdown/up)が効かない」の切り分け用。
    //   - 点が全パス外接矩形の外 → stage.pointer の座標系ズレ (scale/offset)
    //   - q=nan → stage.pointer が NaN (起動直後 rendererScale=0 等)
    //   - 点が矩形内なのに false → winding 判定バグ
    //   - パスが空 → パス構築経路の問題
    // pointerdown/up は move とは別枠(各 40 行)で確実に残す。起動直後の move 洪水で
    // 枠を使い切って down/up が観測できない事態を防ぐ。
    {
        HostContext* host = HostContext::From(a.GetIsolate());
        const int kind = host ? host->input_kind : 0;
        const bool is_press = (kind == 2 || kind == 3);
        static int press_log = 0, move_log = 0;
        int* cnt = is_press ? &press_log : &move_log;
        const int cap = is_press ? 40 : 24;
        if (*cnt < cap) {
            ++(*cnt);
            const char* label = (kind == 2) ? "down" : (kind == 3) ? "up"
                              : (kind == 1) ? "move" : "other";
            double bx0, by0, bx1, by1;
            const bool has = raster::PathBounds(c->path, &bx0, &by0, &bx1, &by1);
            char buf[208];
            std::snprintf(buf, sizeof(buf),
                "[Hit:%s] q=(%.1f,%.1f) subpaths=%zu bbox=%s(%.1f,%.1f..%.1f,%.1f) -> %s",
                label, px, py, c->path.size(), has ? "" : "EMPTY",
                has?bx0:0, has?by0:0, has?bx1:0, has?by1:0, hit ? "HIT" : "miss");
            v8util::AppendErrorLog(buf);
        }
    }

    a.GetReturnValue().Set(v8::Boolean::New(a.GetIsolate(), hit));
}

// --- 変換 ----------------------------------------------------------------
void SetTransform(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    Self(a.This())->state.transform =
        { Arg(a,0,1), Arg(a,1), Arg(a,2), Arg(a,3,1), Arg(a,4), Arg(a,5) };
}
void Transform(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    Canvas2D* c = Self(a.This());
    c->state.transform = Multiply(c->state.transform,
        { Arg(a,0,1), Arg(a,1), Arg(a,2), Arg(a,3,1), Arg(a,4), Arg(a,5) });
}
void Translate(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    Canvas2D* c = Self(a.This());
    c->state.transform = Multiply(c->state.transform, {1,0,0,1, Arg(a,0), Arg(a,1)});
}
void Scale(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    Canvas2D* c = Self(a.This());
    c->state.transform = Multiply(c->state.transform, {Arg(a,0,1),0,0,Arg(a,1,1),0,0});
}
void Rotate(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    Canvas2D* c = Self(a.This());
    double r = Arg(a,0);
    c->state.transform = Multiply(c->state.transform,
        {std::cos(r), std::sin(r), -std::sin(r), std::cos(r), 0, 0});
}
void Save(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    Canvas2D* c = Self(a.This()); c->stack.push_back(c->state);
}
void Restore(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    Canvas2D* c = Self(a.This());
    if (!c->stack.empty()) { c->state = c->stack.back(); c->stack.pop_back(); }
}
// clip(): 現在パスの外接矩形をクリップ領域として設定する (デバイス座標)。
// 既存クリップとは積集合を取る。player の TextField 枠は矩形パスなので外接矩形で十分。
// «EXTEND» 任意形状の厳密クリップは per-pixel マスクが必要 (現状は矩形近似)。
void Clip(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    Canvas2D* c = Self(a.This());
    double minx, miny, maxx, maxy;
    if (!raster::PathBounds(c->path, &minx, &miny, &maxx, &maxy)) return;
    raster::IntersectClip(c->state.clip,
        std::floor(minx), std::floor(miny), std::ceil(maxx), std::ceil(maxy));
}

// --- 矩形 / クリア --------------------------------------------------------
void ClearRect(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    Canvas2D* c = Self(a.This());
    raster::ClearRect(c->surface, Arg(a,0), Arg(a,1), Arg(a,2), Arg(a,3));
}
void FillRect(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    Canvas2D* c = Self(a.This());
    Point p = Apply(c->state.transform, Arg(a,0), Arg(a,1));
    double w = Arg(a,2), h = Arg(a,3);
    for (int yy = static_cast<int>(p.y); yy < p.y + h; ++yy)
        for (int xx = static_cast<int>(p.x); xx < p.x + w; ++xx)
            Blend(c, xx, yy, c->state.fill, c->state.global_alpha);
}

// --- テキスト (DirectWrite / Direct2D — platform/TextRasterizer) -----------
// NOTE: 実装済みだが実機での見た目(ベースライン位置/合字/絵文字)は要検証。
static std::wstring ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

void MeasureText(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    v8::Isolate* iso = a.GetIsolate();
    Canvas2D* c = Self(a.This());
    const std::wstring wtext = ToWide(a.Length() > 0 ? ToStdString(iso, a[0]) : "");

    // DirectWrite が使えない場合は px サイズからの近似メトリクス
    TextMetricsInfo m;
    if (!MeasureTextWithDWrite(c->state.font, wtext, m)) {
        const double px = FontPixelSize(c->state.font);
        m.width = 0;
        m.ascent = px * 0.8;
        m.descent = px * 0.2;
    }
    v8::Local<v8::Object> metrics = v8::Object::New(iso);
    SetValue(iso, metrics, "width", v8::Number::New(iso, m.width));
    SetValue(iso, metrics, "actualBoundingBoxAscent", v8::Number::New(iso, m.ascent));
    SetValue(iso, metrics, "actualBoundingBoxDescent", v8::Number::New(iso, m.descent));
    SetValue(iso, metrics, "fontBoundingBoxAscent", v8::Number::New(iso, m.ascent));
    SetValue(iso, metrics, "fontBoundingBoxDescent", v8::Number::New(iso, m.descent));
    a.GetReturnValue().Set(metrics);
}

// text をラスタライズして surface へ合成する。
void RasterizeText(Canvas2D* c, const std::wstring& wtext, double x, double y, const RGBA& col)
{
    TextBitmap bmp;
    if (!RasterizeTextWithDWrite(c->state.font, wtext, col.r, col.g, col.b, bmp)) return;

    // fillText の x,y はベースライン起点。ビットマップは左上起点。
    Point origin = Apply(c->state.transform, x, y);
    // textAlign (device px 近似): start/left=左, center=中央, right/end=右。
    const std::string& al = c->state.text_align;
    if (al == "center") {
        origin.x -= bmp.width / 2.0;
    } else if (al == "right" || al == "end") {
        origin.x -= bmp.width;
    }
    // textBaseline (device px 近似)。
    const std::string& bl = c->state.text_baseline;
    if (bl == "top" || bl == "hanging") {
        // 上端起点: そのまま
    } else if (bl == "middle") {
        origin.y -= bmp.height / 2.0;
    } else if (bl == "bottom" || bl == "ideographic") {
        origin.y -= bmp.height;
    } else {
        origin.y -= bmp.baseline;   // alphabetic (既定)
    }
    for (int yy = 0; yy < bmp.height; ++yy) {
        for (int xx = 0; xx < bmp.width; ++xx) {
            const uint8_t* px = &bmp.rgba[(static_cast<size_t>(yy) * bmp.width + xx) * 4];
            if (px[3] == 0) continue;
            Blend(c, static_cast<int>(origin.x) + xx, static_cast<int>(origin.y) + yy,
                  RGBA{px[0], px[1], px[2], px[3]}, c->state.global_alpha);
        }
    }
}

void FillText(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    v8::Isolate* iso = a.GetIsolate();
    Canvas2D* c = Self(a.This());
    const std::wstring wtext = ToWide(a.Length() > 0 ? ToStdString(iso, a[0]) : "");
    RasterizeText(c, wtext, Arg(a,1), Arg(a,2), c->state.fill);
}

// strokeText: 近似としてグリフを塗りで描画するが色は strokeStyle を使う。
void StrokeText(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    v8::Isolate* iso = a.GetIsolate();
    Canvas2D* c = Self(a.This());
    const std::wstring wtext = ToWide(a.Length() > 0 ? ToStdString(iso, a[0]) : "");
    RasterizeText(c, wtext, Arg(a,1), Arg(a,2), c->state.stroke);
}

// drawImage(source, dx, dy [, dw, dh]) — 画像/canvas を surface へ合成 (最近傍)。
void DrawImage(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    v8::Isolate* iso = a.GetIsolate();
    Canvas2D* c = Self(a.This());
    const uint8_t* src = nullptr; uint32_t sw = 0, sh = 0;
    if (a.Length() < 1 || !GetImageSourcePixels(iso, a[0], &src, &sw, &sh) || !src) {
        return;
    }
    double dx = Arg(a,1), dy = Arg(a,2);
    double dw = Arg(a,3, static_cast<double>(sw)), dh = Arg(a,4, static_cast<double>(sh));
    for (int yy = 0; yy < static_cast<int>(dh); ++yy) {
        for (int xx = 0; xx < static_cast<int>(dw); ++xx) {
            uint32_t u = static_cast<uint32_t>(xx * sw / dw);
            uint32_t v = static_cast<uint32_t>(yy * sh / dh);
            const uint8_t* p = &src[(static_cast<size_t>(v) * sw + u) * 4];
            RGBA s{p[0], p[1], p[2], p[3]};
            Point d = Apply(c->state.transform, dx + xx, dy + yy);
            Blend(c, static_cast<int>(d.x), static_cast<int>(d.y), s, c->state.global_alpha);
        }
    }
}

// --- ImageData -----------------------------------------------------------
void GetImageData(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    v8::Isolate* iso = a.GetIsolate();
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    Canvas2D* c = Self(a.This());
    int x = static_cast<int>(Arg(a,0)), y = static_cast<int>(Arg(a,1));
    int w = static_cast<int>(Arg(a,2, c->surface.width));
    int h = static_cast<int>(Arg(a,3, c->surface.height));

    size_t bytes = static_cast<size_t>(w) * h * 4;
    auto store = v8::ArrayBuffer::NewBackingStore(iso, bytes);
    auto* dst = static_cast<uint8_t*>(store->Data());
    for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
            int sx = x + xx, sy = y + yy;
            size_t di = (static_cast<size_t>(yy)*w+xx)*4;
            if (sx>=0&&sy>=0&&sx<c->surface.width&&sy<c->surface.height) {
                std::memcpy(&dst[di],
                    &c->surface.pixels[(static_cast<size_t>(sy)*c->surface.width+sx)*4], 4);
            }
        }
    auto ab = v8::ArrayBuffer::New(iso, std::move(store));
    auto arr = v8::Uint8ClampedArray::New(ab, 0, bytes);
    v8::Local<v8::Object> image_data = v8::Object::New(iso);
    SetValue(iso, image_data, "width", v8::Integer::New(iso, w));
    SetValue(iso, image_data, "height", v8::Integer::New(iso, h));
    image_data->Set(ctx, Str(iso, "data"), arr).Check();
    a.GetReturnValue().Set(image_data);
}

// --- 追加ジオメトリ: strokeRect / arcTo / ellipse / roundRect ---------------
void StrokeRect(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    Canvas2D* c = Self(a.This());
    double x = Arg(a,0), y = Arg(a,1), w = Arg(a,2), h = Arg(a,3);
    std::vector<SubPath> tmp; tmp.push_back(SubPath{});
    const double cn[4][2] = { {x,y}, {x+w,y}, {x+w,y+h}, {x,y+h} };
    for (auto& pt : cn) tmp.back().pts.push_back(Apply(c->state.transform, pt[0], pt[1]));
    tmp.back().closed = true;
    raster::StrokePath(c->surface, c->state.clip, tmp, c->state.stroke, c->state.global_alpha);
}

void ArcTo(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    Canvas2D* c = Self(a.This());
    const double x1 = Arg(a,0), y1 = Arg(a,1), x2 = Arg(a,2), y2 = Arg(a,3), r = Arg(a,4);
    const double x0 = c->cur_x, y0 = c->cur_y;
    const double a1 = x0-x1, b1 = y0-y1, a2 = x2-x1, b2 = y2-y1;
    const double l1 = std::hypot(a1,b1), l2 = std::hypot(a2,b2);
    if (l1 < 1e-6 || l2 < 1e-6 || r < 1e-6) { AddPoint(c, x1, y1, c->path.empty()); return; }
    double cosang = std::clamp((a1*a2 + b1*b2) / (l1*l2), -1.0, 1.0);
    const double ang = std::acos(cosang);
    const double th = std::tan(ang / 2);
    if (th < 1e-6) { AddPoint(c, x1, y1, c->path.empty()); return; }
    const double dist = r / th;
    const double t1x = x1 + a1/l1*dist, t1y = y1 + b1/l1*dist;
    const double t2x = x1 + a2/l2*dist, t2y = y1 + b2/l2*dist;
    AddPoint(c, t1x, t1y, c->path.empty());   // 現在点→第一接点
    double bx = a1/l1 + a2/l2, by = b1/l1 + b2/l2;
    const double bl = std::hypot(bx, by);
    if (bl > 1e-6) {
        const double cenDist = r / std::sin(ang / 2);
        const double cx = x1 + bx/bl*cenDist, cy = y1 + by/bl*cenDist;
        const double sa = std::atan2(t1y-cy, t1x-cx), ea = std::atan2(t2y-cy, t2x-cx);
        const bool ccw = (a1*b2 - b1*a2) > 0;
        raster::FlattenArc(c->path.back().pts, c->state.transform, cx, cy, r, sa, ea, ccw);
    }
    c->cur_x = t2x; c->cur_y = t2y;
}

void Ellipse(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    Canvas2D* c = Self(a.This());
    const double cx = Arg(a,0), cy = Arg(a,1), rx = Arg(a,2), ry = Arg(a,3), rot = Arg(a,4);
    double s = Arg(a,5), e = Arg(a,6, 2*raster::kPi);
    const bool ccw = a.Length() > 7 && a[7]->BooleanValue(a.GetIsolate());
    if (ccw && e > s) e -= 2*raster::kPi;
    if (!ccw && e < s) e += 2*raster::kPi;
    if (c->path.empty()) c->path.push_back(SubPath{});
    const double cr = std::cos(rot), sr = std::sin(rot);
    const int steps = 48;
    for (int i = 0; i <= steps; ++i) {
        const double t = s + (e - s) * (static_cast<double>(i) / steps);
        const double ex = rx*std::cos(t), ey = ry*std::sin(t);
        c->path.back().pts.push_back(
            Apply(c->state.transform, cx + ex*cr - ey*sr, cy + ex*sr + ey*cr));
    }
}

void RoundRect(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    Canvas2D* c = Self(a.This());
    const double x = Arg(a,0), y = Arg(a,1), w = Arg(a,2), h = Arg(a,3);
    double r = 0.0;
    if (a.Length() > 4) {
        if (a[4]->IsNumber()) {
            r = a[4].As<v8::Number>()->Value();
        } else if (a[4]->IsArray()) {
            auto arr = a[4].As<v8::Array>();
            v8::Local<v8::Value> v0;
            if (arr->Length() > 0 && arr->Get(a.GetIsolate()->GetCurrentContext(), 0).ToLocal(&v0) && v0->IsNumber()) {
                r = v0.As<v8::Number>()->Value();
            }
        }
    }
    r = std::min(r, std::min(std::abs(w), std::abs(h)) / 2.0);
    c->path.push_back(SubPath{});
    auto& pts = c->path.back().pts;
    const double hp = raster::kPi / 2;
    raster::FlattenArc(pts, c->state.transform, x+w-r, y+r,   r, -hp,   0.0,   false); // 右上
    raster::FlattenArc(pts, c->state.transform, x+w-r, y+h-r, r, 0.0,   hp,    false); // 右下
    raster::FlattenArc(pts, c->state.transform, x+r,   y+h-r, r, hp,    raster::kPi, false); // 左下
    raster::FlattenArc(pts, c->state.transform, x+r,   y+r,   r, raster::kPi, hp*3, false); // 左上
    c->path.back().closed = true;
}

// --- 追加変換: resetTransform / getTransform --------------------------------
void ResetTransform(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    Self(a.This())->state.transform = Mat{1,0,0,1,0,0};
}
void GetTransform(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    v8::Isolate* iso = a.GetIsolate();
    const Mat& m = Self(a.This())->state.transform;
    v8::Local<v8::Object> o = v8::Object::New(iso);
    SetValue(iso, o, "a", v8::Number::New(iso, m.a));
    SetValue(iso, o, "b", v8::Number::New(iso, m.b));
    SetValue(iso, o, "c", v8::Number::New(iso, m.c));
    SetValue(iso, o, "d", v8::Number::New(iso, m.d));
    SetValue(iso, o, "e", v8::Number::New(iso, m.e));
    SetValue(iso, o, "f", v8::Number::New(iso, m.f));
    // DOMMatrix 互換の別名も付ける
    SetValue(iso, o, "m11", v8::Number::New(iso, m.a));
    SetValue(iso, o, "m12", v8::Number::New(iso, m.b));
    SetValue(iso, o, "m21", v8::Number::New(iso, m.c));
    SetValue(iso, o, "m22", v8::Number::New(iso, m.d));
    SetValue(iso, o, "m41", v8::Number::New(iso, m.e));
    SetValue(iso, o, "m42", v8::Number::New(iso, m.f));
    a.GetReturnValue().Set(o);
}

// --- 追加ライン: getLineDash / isPointInStroke -----------------------------
void GetLineDash(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    v8::Isolate* iso = a.GetIsolate();
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    const auto& dash = Self(a.This())->state.line_dash;
    v8::Local<v8::Array> arr = v8::Array::New(iso, static_cast<int>(dash.size()));
    for (uint32_t i = 0; i < dash.size(); ++i) {
        arr->Set(ctx, i, v8::Number::New(iso, dash[i])).Check();
    }
    a.GetReturnValue().Set(arr);
}
// isPointInStroke: 近似としてパス内包 (fill) と同じ判定を返す。
void IsPointInStroke(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    Canvas2D* c = Self(a.This());
    a.GetReturnValue().Set(v8::Boolean::New(
        a.GetIsolate(), raster::PointInPath(c->path, Arg(a,0), Arg(a,1))));
}

// --- 追加: gradient / pattern ----------------------------------------------
void AddColorStop(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    v8::Local<v8::Object> self = a.This();
    if (self->InternalFieldCount() < 1) return;
    v8::Local<v8::Value> ext = self->GetInternalField(0).As<v8::Value>();
    if (!ext->IsExternal()) return;
    auto* p = static_cast<PaintObject*>(ext.As<v8::External>()->Value());
    if (!p) return;
    p->stops.push_back({ Arg(a,0), ParseColor(ToStdString(a.GetIsolate(), a.Length()>1 ? a[1] : v8::Local<v8::Value>())) });
}

v8::Local<v8::Object> MakePaintObject(v8::Isolate* isolate, PaintObject* paint)
{
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(isolate);
    tmpl->SetInternalFieldCount(1);
    v8::Local<v8::Object> o = tmpl->NewInstance(ctx).ToLocalChecked();
    o->SetInternalField(0, v8::External::New(isolate, paint));
    v8util::AttachWeak(isolate, o, paint);
    SetMethod(isolate, o, "addColorStop", AddColorStop);
    return o;
}

void CreateLinearGradient(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    auto* p = new PaintObject();
    p->kind = PaintObject::Gradient;
    a.GetReturnValue().Set(MakePaintObject(a.GetIsolate(), p));
}
void CreateRadialGradient(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    auto* p = new PaintObject();
    p->kind = PaintObject::Gradient;
    a.GetReturnValue().Set(MakePaintObject(a.GetIsolate(), p));
}
void CreateConicGradient(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    auto* p = new PaintObject();
    p->kind = PaintObject::Gradient;
    a.GetReturnValue().Set(MakePaintObject(a.GetIsolate(), p));
}
void CreatePattern(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    v8::Isolate* iso = a.GetIsolate();
    auto* p = new PaintObject();
    p->kind = PaintObject::Pattern;
    // 画像の代表色 (中央付近の平均) をサンプルする。
    const uint8_t* src = nullptr; uint32_t sw = 0, sh = 0;
    if (a.Length() > 0 && GetImageSourcePixels(iso, a[0], &src, &sw, &sh) && src && sw && sh) {
        uint64_t r=0,g=0,b=0,al=0,n=0;
        for (uint32_t yy = 0; yy < sh; yy += std::max(1u, sh/16)) {
            for (uint32_t xx = 0; xx < sw; xx += std::max(1u, sw/16)) {
                const uint8_t* px = &src[(static_cast<size_t>(yy)*sw + xx)*4];
                r+=px[0]; g+=px[1]; b+=px[2]; al+=px[3]; ++n;
            }
        }
        if (n) p->sample = RGBA{ uint8_t(r/n), uint8_t(g/n), uint8_t(b/n), uint8_t(al/n) };
    }
    a.GetReturnValue().Set(MakePaintObject(iso, p));
}

// --- 追加: ImageData 生成 / 書き戻し ---------------------------------------
void CreateImageData(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    v8::Isolate* iso = a.GetIsolate();
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    int w = 0, h = 0;
    if (a.Length() > 0 && a[0]->IsObject() && !a[0]->IsNumber()) {
        // createImageData(imagedata): 同サイズの空データ
        v8::Local<v8::Object> o = a[0].As<v8::Object>();
        v8::Local<v8::Value> wv, hv;
        if (o->Get(ctx, Str(iso, "width")).ToLocal(&wv))  w = static_cast<int>(wv->NumberValue(ctx).FromMaybe(0));
        if (o->Get(ctx, Str(iso, "height")).ToLocal(&hv)) h = static_cast<int>(hv->NumberValue(ctx).FromMaybe(0));
    } else {
        w = std::abs(static_cast<int>(Arg(a,0)));
        h = std::abs(static_cast<int>(Arg(a,1)));
    }
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    const size_t bytes = static_cast<size_t>(w) * h * 4;
    auto store = v8::ArrayBuffer::NewBackingStore(iso, bytes);   // 0 初期化
    auto ab = v8::ArrayBuffer::New(iso, std::move(store));
    auto arr = v8::Uint8ClampedArray::New(ab, 0, bytes);
    v8::Local<v8::Object> image_data = v8::Object::New(iso);
    SetValue(iso, image_data, "width",  v8::Integer::New(iso, w));
    SetValue(iso, image_data, "height", v8::Integer::New(iso, h));
    image_data->Set(ctx, Str(iso, "data"), arr).Check();
    a.GetReturnValue().Set(image_data);
}

void PutImageData(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    v8::Isolate* iso = a.GetIsolate();
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    Canvas2D* c = Self(a.This());
    if (a.Length() < 3 || !a[0]->IsObject()) return;
    v8::Local<v8::Object> o = a[0].As<v8::Object>();
    v8::Local<v8::Value> wv, hv, dv;
    if (!o->Get(ctx, Str(iso, "width")).ToLocal(&wv) ||
        !o->Get(ctx, Str(iso, "height")).ToLocal(&hv) ||
        !o->Get(ctx, Str(iso, "data")).ToLocal(&dv) || !dv->IsTypedArray()) {
        return;
    }
    const int iw = static_cast<int>(wv->NumberValue(ctx).FromMaybe(0));
    const int ih = static_cast<int>(hv->NumberValue(ctx).FromMaybe(0));
    auto ta = dv.As<v8::TypedArray>();
    auto ab = ta->Buffer();
    const uint8_t* data = static_cast<const uint8_t*>(ab->Data()) + ta->ByteOffset();
    const int dx = static_cast<int>(Arg(a,1)), dy = static_cast<int>(Arg(a,2));
    // putImageData は変換・クリップ非適用でそのまま書き込む (Canvas 仕様)。
    for (int yy = 0; yy < ih; ++yy) {
        for (int xx = 0; xx < iw; ++xx) {
            const int sx = dx + xx, sy = dy + yy;
            if (sx < 0 || sy < 0 || sx >= c->surface.width || sy >= c->surface.height) continue;
            const size_t si = (static_cast<size_t>(yy)*iw + xx) * 4;
            const size_t di = (static_cast<size_t>(sy)*c->surface.width + sx) * 4;
            std::memcpy(&c->surface.pixels[di], &data[si], 4);
        }
    }
}

// --- 追加: reset() ----------------------------------------------------------
void Reset(const v8::FunctionCallbackInfo<v8::Value>& a)
{
    Canvas2D* c = Self(a.This());
    c->path.clear();
    c->stack.clear();
    c->state = State{};
    std::fill(c->surface.pixels.begin(), c->surface.pixels.end(), static_cast<uint8_t>(0));
}

// CanvasRenderingContext2D の全プロパティを getter/setter 付きで設置する。
// 解釈して描画へ反映するもの(fill/stroke/font/textAlign/textBaseline/lineWidth/
// globalAlpha)と、状態として往復保持するだけのもの(shadow/composite/filter 等の
// 近似)を含む。全て state に持たせるため save()/restore() でも復元される。
void InstallAccessors(v8::Isolate* isolate, v8::Local<v8::Object> obj)
{
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

    // state の string/number/bool メンバへ往復するアクセサを設置するマクロ。
#define N2D_STR_PROP(NAME, MEMBER) \
    obj->SetNativeDataProperty(ctx, Str(isolate, NAME), \
        [](v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info){ \
            info.GetReturnValue().Set(v8util::Str(info.GetIsolate(), Self(info.This())->state.MEMBER)); }, \
        [](v8::Local<v8::Name>, v8::Local<v8::Value> v, const v8::PropertyCallbackInfo<void>& info){ \
            Self(info.This())->state.MEMBER = ToStdString(info.GetIsolate(), v); }).Check()
#define N2D_NUM_PROP(NAME, MEMBER) \
    obj->SetNativeDataProperty(ctx, Str(isolate, NAME), \
        [](v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info){ \
            info.GetReturnValue().Set(v8::Number::New(info.GetIsolate(), Self(info.This())->state.MEMBER)); }, \
        [](v8::Local<v8::Name>, v8::Local<v8::Value> v, const v8::PropertyCallbackInfo<void>& info){ \
            if (v->IsNumber()) Self(info.This())->state.MEMBER = v.As<v8::Number>()->Value(); }).Check()
#define N2D_BOOL_PROP(NAME, MEMBER) \
    obj->SetNativeDataProperty(ctx, Str(isolate, NAME), \
        [](v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info){ \
            info.GetReturnValue().Set(v8::Boolean::New(info.GetIsolate(), Self(info.This())->state.MEMBER)); }, \
        [](v8::Local<v8::Name>, v8::Local<v8::Value> v, const v8::PropertyCallbackInfo<void>& info){ \
            Self(info.This())->state.MEMBER = v->BooleanValue(info.GetIsolate()); }).Check()

    // fillStyle / strokeStyle: gradient/pattern オブジェクトも受け取り代表色へ解決。
    // 重要: getter は「代入した色を #rrggbb に正規化して」返す (ブラウザ準拠)。
    // player は色文字列の数値化に読み戻しを使う (@next2d/filters の
    // $convertColorStringToNumber は `+("0x"+ctx.fillStyle.slice(1))` で解釈するため、
    // getter が "rgb(191,255,255)" 等の生文字列を返すと NaN になり、その色を使う
    // グラデーション (スライム body 等) が黒くなる)。パース済み RGBA を hex 化して保持する。
    obj->SetNativeDataProperty(ctx, Str(isolate, "fillStyle"),
        [](v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
            info.GetReturnValue().Set(v8util::Str(info.GetIsolate(), Self(info.This())->state.fill_style_str));
        },
        [](v8::Local<v8::Name>, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<void>& info) {
            Canvas2D* c = Self(info.This());
            c->state.fill = ColorFromValue(info.GetIsolate(), value);
            if (value->IsString()) {
                char hex[8];
                std::snprintf(hex, sizeof(hex), "#%02x%02x%02x",
                    static_cast<unsigned>(c->state.fill.r),
                    static_cast<unsigned>(c->state.fill.g),
                    static_cast<unsigned>(c->state.fill.b));
                c->state.fill_style_str = hex;
            }
        }).Check();
    obj->SetNativeDataProperty(ctx, Str(isolate, "strokeStyle"),
        [](v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
            info.GetReturnValue().Set(v8util::Str(info.GetIsolate(), Self(info.This())->state.stroke_style_str));
        },
        [](v8::Local<v8::Name>, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<void>& info) {
            Canvas2D* c = Self(info.This());
            c->state.stroke = ColorFromValue(info.GetIsolate(), value);
            if (value->IsString()) {
                char hex[8];
                std::snprintf(hex, sizeof(hex), "#%02x%02x%02x",
                    static_cast<unsigned>(c->state.stroke.r),
                    static_cast<unsigned>(c->state.stroke.g),
                    static_cast<unsigned>(c->state.stroke.b));
                c->state.stroke_style_str = hex;
            }
        }).Check();

    N2D_STR_PROP("font", font);
    N2D_NUM_PROP("lineWidth", line_width);
    N2D_NUM_PROP("globalAlpha", global_alpha);
    N2D_STR_PROP("lineCap", line_cap);
    N2D_STR_PROP("lineJoin", line_join);
    N2D_NUM_PROP("miterLimit", miter_limit);
    N2D_NUM_PROP("lineDashOffset", line_dash_offset);
    N2D_NUM_PROP("shadowBlur", shadow_blur);
    N2D_STR_PROP("shadowColor", shadow_color);
    N2D_NUM_PROP("shadowOffsetX", shadow_offset_x);
    N2D_NUM_PROP("shadowOffsetY", shadow_offset_y);
    N2D_STR_PROP("textAlign", text_align);
    N2D_STR_PROP("textBaseline", text_baseline);
    N2D_STR_PROP("direction", direction);
    N2D_STR_PROP("globalCompositeOperation", global_composite_operation);
    N2D_BOOL_PROP("imageSmoothingEnabled", image_smoothing_enabled);
    N2D_STR_PROP("imageSmoothingQuality", image_smoothing_quality);
    N2D_STR_PROP("filter", filter);
    N2D_STR_PROP("fontKerning", font_kerning);
    N2D_STR_PROP("letterSpacing", letter_spacing);
    N2D_STR_PROP("wordSpacing", word_spacing);
    N2D_STR_PROP("textRendering", text_rendering);

#undef N2D_STR_PROP
#undef N2D_NUM_PROP
#undef N2D_BOOL_PROP
}

} // namespace

// ImageSource.h: Canvas2D コンテキストオブジェクトから RGBA を取得する。
bool GetCanvas2DPixels(v8::Local<v8::Object> context2d,
                       const uint8_t** out_rgba, uint32_t* out_width, uint32_t* out_height)
{
    if (context2d->InternalFieldCount() < 1) {
        return false;
    }
    auto* c = static_cast<Canvas2D*>(
        context2d->GetInternalField(0).As<v8::External>()->Value());
    if (!c) return false;
    *out_rgba = c->surface.pixels.data();
    *out_width = static_cast<uint32_t>(c->surface.width);
    *out_height = static_cast<uint32_t>(c->surface.height);
    return true;
}

v8::Local<v8::Object> CreateCanvas2DContext(v8::Isolate* isolate, HostContext* /*host*/,
                                            int width, int height)
{
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(isolate);
    tmpl->SetInternalFieldCount(1);
    v8::Local<v8::Object> obj = tmpl->NewInstance(ctx).ToLocalChecked();

    auto* impl = new Canvas2D(width > 0 ? width : 1, height > 0 ? height : 1);
    obj->SetInternalField(0, v8::External::New(isolate, impl));
    v8util::AttachWeak(isolate, obj, impl);

    // パス構築
    SetMethod(isolate, obj, "beginPath", BeginPath);
    SetMethod(isolate, obj, "moveTo", MoveTo);
    SetMethod(isolate, obj, "lineTo", LineTo);
    SetMethod(isolate, obj, "closePath", ClosePath);
    SetMethod(isolate, obj, "bezierCurveTo", BezierCurveTo);
    SetMethod(isolate, obj, "quadraticCurveTo", QuadraticCurveTo);
    SetMethod(isolate, obj, "arc", Arc);
    SetMethod(isolate, obj, "arcTo", ArcTo);
    SetMethod(isolate, obj, "ellipse", Ellipse);
    SetMethod(isolate, obj, "rect", Rect);
    SetMethod(isolate, obj, "roundRect", RoundRect);
    // 塗り/線/判定
    SetMethod(isolate, obj, "fill", Fill);
    SetMethod(isolate, obj, "stroke", Stroke);
    SetMethod(isolate, obj, "clip", Clip);
    SetMethod(isolate, obj, "isPointInPath", IsPointInPath);
    SetMethod(isolate, obj, "isPointInStroke", IsPointInStroke);
    // 変換
    SetMethod(isolate, obj, "setTransform", SetTransform);
    SetMethod(isolate, obj, "transform", Transform);
    SetMethod(isolate, obj, "translate", Translate);
    SetMethod(isolate, obj, "scale", Scale);
    SetMethod(isolate, obj, "rotate", Rotate);
    SetMethod(isolate, obj, "resetTransform", ResetTransform);
    SetMethod(isolate, obj, "getTransform", GetTransform);
    // 状態スタック
    SetMethod(isolate, obj, "save", Save);
    SetMethod(isolate, obj, "restore", Restore);
    SetMethod(isolate, obj, "reset", Reset);
    // 矩形
    SetMethod(isolate, obj, "clearRect", ClearRect);
    SetMethod(isolate, obj, "fillRect", FillRect);
    SetMethod(isolate, obj, "strokeRect", StrokeRect);
    // テキスト
    SetMethod(isolate, obj, "measureText", MeasureText);
    SetMethod(isolate, obj, "fillText", FillText);
    SetMethod(isolate, obj, "strokeText", StrokeText);
    // 画像 / ImageData
    SetMethod(isolate, obj, "drawImage", DrawImage);
    SetMethod(isolate, obj, "getImageData", GetImageData);
    SetMethod(isolate, obj, "createImageData", CreateImageData);
    SetMethod(isolate, obj, "putImageData", PutImageData);
    // gradient / pattern
    SetMethod(isolate, obj, "createLinearGradient", CreateLinearGradient);
    SetMethod(isolate, obj, "createRadialGradient", CreateRadialGradient);
    SetMethod(isolate, obj, "createConicGradient", CreateConicGradient);
    SetMethod(isolate, obj, "createPattern", CreatePattern);
    // 線種
    SetMethod(isolate, obj, "getLineDash", GetLineDash);
    SetMethod(isolate, obj, "setLineDash", [](const v8::FunctionCallbackInfo<v8::Value>& a) {
        Canvas2D* c = Self(a.This());
        c->state.line_dash.clear();
        if (a.Length() > 0 && a[0]->IsArray()) {
            auto arr = a[0].As<v8::Array>();
            v8::Local<v8::Context> ctx2 = a.GetIsolate()->GetCurrentContext();
            for (uint32_t i = 0; i < arr->Length(); ++i) {
                v8::Local<v8::Value> v;
                if (arr->Get(ctx2, i).ToLocal(&v) && v->IsNumber()) {
                    c->state.line_dash.push_back(v.As<v8::Number>()->Value());
                }
            }
        }
    });
    // 参考: getContextAttributes (最小)
    SetMethod(isolate, obj, "getContextAttributes", [](const v8::FunctionCallbackInfo<v8::Value>& a) {
        v8::Isolate* iso = a.GetIsolate();
        v8::Local<v8::Object> o = v8::Object::New(iso);
        SetValue(iso, o, "alpha", v8::Boolean::New(iso, true));
        SetValue(iso, o, "desynchronized", v8::Boolean::New(iso, false));
        SetValue(iso, o, "colorSpace", v8util::Str(iso, "srgb"));
        SetValue(iso, o, "willReadFrequently", v8::Boolean::New(iso, false));
        a.GetReturnValue().Set(o);
    });

    InstallAccessors(isolate, obj);
    return obj;
}

} // namespace next2d
