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

#include <Windows.h>

#include <algorithm>
#include <cstdio>
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
};

// 2D コンテキストの実体。JS オブジェクトの内部フィールドに保持する。
struct Canvas2D {
    raster::Surface surface;
    std::vector<SubPath> path;
    State state;
    std::vector<State> stack;

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
    Canvas2D* c = Self(a.This());
    a.GetReturnValue().Set(v8::Boolean::New(
        a.GetIsolate(), raster::PointInPath(c->path, Arg(a,0), Arg(a,1))));
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

    // fillText の x,y はベースライン起点。ビットマップは左上起点なので baseline を引く。
    Point origin = Apply(c->state.transform, x, y - bmp.baseline);
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

// GC 時に Canvas2D 実体を解放
void ReleaseCanvas2D(const v8::WeakCallbackInfo<Canvas2D>& info) { delete info.GetParameter(); }

// fillStyle / strokeStyle / lineWidth / globalAlpha / font アクセサ
void InstallAccessors(v8::Isolate* isolate, v8::Local<v8::Object> obj)
{
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    auto set_str = [&](const char* name, void(*setter)(Canvas2D*, const std::string&)) {
        obj->SetNativeDataProperty(ctx, Str(isolate, name),
            nullptr,
            [](v8::Local<v8::Name>, v8::Local<v8::Value> value,
               const v8::PropertyCallbackInfo<void>& info) {
                // setter は External(data) 経由で受け取る
                auto* fn = reinterpret_cast<void(*)(Canvas2D*, const std::string&)>(
                    info.Data().As<v8::External>()->Value());
                fn(Self(info.This()), ToStdString(info.GetIsolate(), value));
            },
            v8::External::New(isolate, reinterpret_cast<void*>(setter))).Check();
    };
    // fillStyle は player が色文字列の正規化に「代入後の読み出し」を使うため getter も持たせる。
    obj->SetNativeDataProperty(ctx, Str(isolate, "fillStyle"),
        [](v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
            RGBA c = Self(info.This())->state.fill;
            char buf[8];
            std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", c.r, c.g, c.b);
            info.GetReturnValue().Set(v8util::Str(info.GetIsolate(), buf));
        },
        [](v8::Local<v8::Name>, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<void>& info) {
            Self(info.This())->state.fill = ParseColor(ToStdString(info.GetIsolate(), value));
        }).Check();
    set_str("strokeStyle", [](Canvas2D* c, const std::string& s){ c->state.stroke = ParseColor(s); });
    set_str("font",        [](Canvas2D* c, const std::string& s){ c->state.font = s; });

    obj->SetNativeDataProperty(ctx, Str(isolate, "lineWidth"), nullptr,
        [](v8::Local<v8::Name>, v8::Local<v8::Value> v, const v8::PropertyCallbackInfo<void>& info){
            if (v->IsNumber()) Self(info.This())->state.line_width = v.As<v8::Number>()->Value();
        }).Check();
    obj->SetNativeDataProperty(ctx, Str(isolate, "globalAlpha"), nullptr,
        [](v8::Local<v8::Name>, v8::Local<v8::Value> v, const v8::PropertyCallbackInfo<void>& info){
            if (v->IsNumber()) Self(info.This())->state.global_alpha = v.As<v8::Number>()->Value();
        }).Check();
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
    auto* handle = new v8::Global<v8::Object>(isolate, obj);
    handle->SetWeak(impl, ReleaseCanvas2D, v8::WeakCallbackType::kParameter);

    SetMethod(isolate, obj, "beginPath", BeginPath);
    SetMethod(isolate, obj, "moveTo", MoveTo);
    SetMethod(isolate, obj, "lineTo", LineTo);
    SetMethod(isolate, obj, "closePath", ClosePath);
    SetMethod(isolate, obj, "bezierCurveTo", BezierCurveTo);
    SetMethod(isolate, obj, "quadraticCurveTo", QuadraticCurveTo);
    SetMethod(isolate, obj, "arc", Arc);
    SetMethod(isolate, obj, "rect", Rect);
    SetMethod(isolate, obj, "fill", Fill);
    SetMethod(isolate, obj, "stroke", Stroke);
    SetMethod(isolate, obj, "clip", Clip);
    SetMethod(isolate, obj, "isPointInPath", IsPointInPath);
    SetMethod(isolate, obj, "setTransform", SetTransform);
    SetMethod(isolate, obj, "transform", Transform);
    SetMethod(isolate, obj, "translate", Translate);
    SetMethod(isolate, obj, "scale", Scale);
    SetMethod(isolate, obj, "rotate", Rotate);
    SetMethod(isolate, obj, "save", Save);
    SetMethod(isolate, obj, "restore", Restore);
    SetMethod(isolate, obj, "clearRect", ClearRect);
    SetMethod(isolate, obj, "fillRect", FillRect);
    SetMethod(isolate, obj, "measureText", MeasureText);
    SetMethod(isolate, obj, "fillText", FillText);
    SetMethod(isolate, obj, "strokeText", StrokeText);
    SetMethod(isolate, obj, "drawImage", DrawImage);
    SetMethod(isolate, obj, "getImageData", GetImageData);
    SetMethod(isolate, obj, "setLineDash", [](const v8::FunctionCallbackInfo<v8::Value>&) {});

    InstallAccessors(isolate, obj);
    return obj;
}

} // namespace next2d
