// ShapeAcceleratorCore: 図形の「経路の組み立て」「塗りのメッシュの生成」「線の外周の計算」の計算本体。
// V8 に依存しないので、単体テスト (tests/shape_accelerator_test.cpp) からも使える。
// 計算の順序と、32 ビットへ丸める位置は、Next2D Player の JavaScript と合わせてある。
// player は描画の方式 (WebGL / WebGPU) ごとに経路の規則とメッシュの形式が違うので、
// 実装も 2 つある: next2d::shape (WebGL) と next2d::shape::gpu (WebGPU)。
// 説明は ShapeAccelerator.cpp を参照。
#pragma once

// 積和をまとめて 1 命令で計算する最適化 (FMA) を止める。丸めの回数が変わると、player の
// JavaScript での計算と結果が一致しなくなる。
#if defined(_MSC_VER)
#pragma fp_contract(off)
#elif defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
// GCC は上の標準の指定を無視するので、最適化の設定として止める (このヘッダの関数だけに効かせる。
// 末尾の pop_options と対)
#pragma GCC push_options
#pragma GCC optimize("fp-contract=off")
#define NEXT2D_SHAPE_GCC_OPTIONS_PUSHED 1
#endif

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace next2d::shape {

// 描画命令の番号 (player の GraphicsCommand と同じ)
constexpr int kMoveTo = 0;
constexpr int kCurveTo = 1;
constexpr int kLineTo = 2;
constexpr int kCubic = 3;
constexpr int kArc = 4;
constexpr int kClosePath = 12;

// メッシュの頂点 1 つの要素数と、三角形 1 つの要素数
constexpr size_t kVertexSize = 17;
constexpr size_t kTriangleSize = kVertexSize * 3;

// 3 次ベジェ曲線を 2 次ベジェ曲線 1 本で近似してよい誤差の上限と、分割数を決める定数
constexpr double kCubicSingleSegmentLimit = 1.08;
constexpr double kCubicTolerance = 0.05;
constexpr int kMaxSubdivisions = 8;

// 経路: (x, y, 制御点なら 1) の並び
struct PathState {
    std::vector<double> current;
    std::vector<std::vector<double>> vertices;
};

inline void PushPoint(PathState& s, double x, double y, double control)
{
    s.current.push_back(x);
    s.current.push_back(y);
    s.current.push_back(control);
}

inline bool EqualsToLastPoint(const PathState& s, double x, double y)
{
    const size_t length = s.current.size();
    return x == s.current[length - 3] && y == s.current[length - 2];
}

inline void PushCurrentPathToVertices(PathState& s)
{
    if (s.current.size() < 3) {
        s.current.clear();
        return;
    }
    s.vertices.push_back(s.current);
    s.current.clear();
}

inline void MoveTo(PathState& s, double x, double y)
{
    if (s.current.empty()) {
        PushPoint(s, x, y, 0.0);
        return;
    }
    if (EqualsToLastPoint(s, x, y)) {
        return;
    }
    PushCurrentPathToVertices(s);
    PushPoint(s, x, y, 0.0);
}

inline void LineTo(PathState& s, double x, double y)
{
    if (s.current.empty()) {
        MoveTo(s, x, y);
    }
    if (EqualsToLastPoint(s, x, y)) {
        return;
    }
    PushPoint(s, x, y, 0.0);
}

inline void QuadraticCurveTo(PathState& s, double cx, double cy, double x, double y)
{
    if (s.current.empty()) {
        MoveTo(s, 0.0, 0.0);
    }
    if (EqualsToLastPoint(s, x, y)) {
        return;
    }
    PushPoint(s, cx, cy, 1.0);
    PushPoint(s, x, y, 0.0);
}

inline void ClosePath(PathState& s)
{
    if (s.current.size() < 7) {
        return;
    }
    const double x = s.current[0];
    const double y = s.current[1];
    if (EqualsToLastPoint(s, x, y)) {
        return;
    }
    PushPoint(s, x, y, 0.0);
}

// 3 次ベジェ曲線を t で 2 つに分ける。結果は 32 ビットへ丸めて持つ (player は Float32Array に書く)
inline void SplitCubicAt(double p0x, double p0y, double p1x, double p1y,
                  double p2x, double p2y, double p3x, double p3y,
                  double t, float* left, float* right)
{
    const double mt = 1 - t;
    const double q0x = mt * p0x + t * p1x;
    const double q0y = mt * p0y + t * p1y;
    const double q1x = mt * p1x + t * p2x;
    const double q1y = mt * p1y + t * p2y;
    const double q2x = mt * p2x + t * p3x;
    const double q2y = mt * p2y + t * p3y;
    const double r0x = mt * q0x + t * q1x;
    const double r0y = mt * q0y + t * q1y;
    const double r1x = mt * q1x + t * q2x;
    const double r1y = mt * q1y + t * q2y;
    const double sx = mt * r0x + t * r1x;
    const double sy = mt * r0y + t * r1y;
    left[0] = static_cast<float>(p0x);  left[1] = static_cast<float>(p0y);
    left[2] = static_cast<float>(q0x);  left[3] = static_cast<float>(q0y);
    left[4] = static_cast<float>(r0x);  left[5] = static_cast<float>(r0y);
    left[6] = static_cast<float>(sx);   left[7] = static_cast<float>(sy);
    right[0] = static_cast<float>(sx);  right[1] = static_cast<float>(sy);
    right[2] = static_cast<float>(r1x); right[3] = static_cast<float>(r1y);
    right[4] = static_cast<float>(q2x); right[5] = static_cast<float>(q2y);
    right[6] = static_cast<float>(p3x); right[7] = static_cast<float>(p3y);
}

// 3 次ベジェ曲線を 2 次ベジェ曲線 1 本で近似する (制御点と終点を 32 ビットで返す)
inline void CubicToQuad(double p0x, double p0y, double p1x, double p1y,
                        double p2x, double p2y, double p3x, double p3y, float* out)
{
    out[0] = static_cast<float>((3 * p1x + 3 * p2x - p0x - p3x) * 0.25);
    out[1] = static_cast<float>((3 * p1y + 3 * p2y - p0y - p3y) * 0.25);
    out[2] = static_cast<float>(p3x);
    out[3] = static_cast<float>(p3y);
}

inline int CubicSegmentCount(double p0x, double p0y, double p1x, double p1y,
                      double p2x, double p2y, double p3x, double p3y, double pixel_scale)
{
    const double dx = p3x - 3 * p2x + 3 * p1x - p0x;
    const double dy = p3y - 3 * p2y + 3 * p1y - p0y;
    const double error = 0.048112522432468816 * std::sqrt(dx * dx + dy * dy) * pixel_scale;
    int count = 1;
    while (count < kMaxSubdivisions && error > kCubicTolerance * count * count * count) {
        ++count;
    }
    return count;
}

inline void BezierCurveTo(PathState& s, double cx1, double cy1, double cx2, double cy2,
                   double x, double y, double scale_squared)
{
    if (s.current.empty()) {
        MoveTo(s, 0.0, 0.0);
    }
    const size_t length = s.current.size();
    const double from_x = s.current[length - 3];
    const double from_y = s.current[length - 2];
    if (x == from_x && y == from_y) {
        return;
    }

    // 2 次ベジェ曲線 1 本で足りる (画面上の誤差が小さい) なら、そのまま追加する
    const double dx = x - 3 * cx2 + 3 * cx1 - from_x;
    const double dy = y - 3 * cy2 + 3 * cy1 - from_y;
    if ((dx * dx + dy * dy) * scale_squared <= kCubicSingleSegmentLimit) {
        PushPoint(s, (3 * cx1 + 3 * cx2 - from_x - x) * 0.25,
                     (3 * cy1 + 3 * cy2 - from_y - y) * 0.25, 1.0);
        PushPoint(s, x, y, 0.0);
        return;
    }

    // 画面上の誤差に応じた本数の 2 次ベジェ曲線に分ける
    const double pixel_scale = std::sqrt(scale_squared);
    const int count = CubicSegmentCount(from_x, from_y, cx1, cy1, cx2, cy2, x, y, pixel_scale);
    float quads[kMaxSubdivisions * 4];
    if (count == 1) {
        CubicToQuad(from_x, from_y, cx1, cy1, cx2, cy2, x, y, quads);
    } else {
        float buffer_a[8];
        float buffer_b[8];
        float left[8];
        float* rest = buffer_a;
        float* next = buffer_b;
        rest[0] = static_cast<float>(from_x); rest[1] = static_cast<float>(from_y);
        rest[2] = static_cast<float>(cx1);    rest[3] = static_cast<float>(cy1);
        rest[4] = static_cast<float>(cx2);    rest[5] = static_cast<float>(cy2);
        rest[6] = static_cast<float>(x);      rest[7] = static_cast<float>(y);
        int offset = 0;
        for (int idx = 0; idx < count - 1; ++idx) {
            SplitCubicAt(rest[0], rest[1], rest[2], rest[3], rest[4], rest[5], rest[6], rest[7],
                         1.0 / (count - idx), left, next);
            CubicToQuad(left[0], left[1], left[2], left[3], left[4], left[5], left[6], left[7],
                        quads + offset);
            offset += 4;
            float* swap = rest;
            rest = next;
            next = swap;
        }
        CubicToQuad(rest[0], rest[1], rest[2], rest[3], rest[4], rest[5], rest[6], rest[7],
                    quads + offset);
    }
    for (int idx = 0; idx < count * 4; idx += 4) {
        QuadraticCurveTo(s, quads[idx], quads[idx + 1], quads[idx + 2], quads[idx + 3]);
    }
}

inline void Arc(PathState& s, double x, double y, double radius, double scale_squared)
{
    const double r = radius;
    const double k = radius * 0.5522847498307936;
    BezierCurveTo(s, x + r, y + k, x + k, y + r, x, y + r, scale_squared);
    BezierCurveTo(s, x - k, y + r, x - r, y + k, x - r, y, scale_squared);
    BezierCurveTo(s, x - r, y - k, x - k, y - r, x, y - r, scale_squared);
    BezierCurveTo(s, x + k, y - r, x + r, y - k, x + r, y, scale_squared);
}

// 塗り (または線) に使う経路だけを残す: 書きかけの経路を確定し、点の少ない経路を捨てる
inline void FinalizePaths(PathState& s, size_t min_elements)
{
    if (s.current.size() >= min_elements) {
        s.vertices.push_back(s.current);
    }
    s.current.clear();
    size_t kept = 0;
    for (size_t idx = 0; idx < s.vertices.size(); ++idx) {
        if (s.vertices[idx].size() >= min_elements) {
            if (kept != idx) {
                s.vertices[kept] = std::move(s.vertices[idx]);
            }
            ++kept;
        }
    }
    s.vertices.resize(kept);
}

// commands[index] から続く経路の命令をまとめて処理し、経路でない命令の位置 (または終端) を返す
inline size_t FeedPath(PathState& s, const float* commands, size_t length, size_t index, double scale_squared)
{
    while (index < length) {
        const int command = static_cast<int>(commands[index]);
        const float* p = commands + index + 1;
        const size_t rest = length - index - 1;
        if (command == kMoveTo && rest >= 2) {
            MoveTo(s, p[0], p[1]);
            index += 3;
        } else if (command == kLineTo && rest >= 2) {
            LineTo(s, p[0], p[1]);
            index += 3;
        } else if (command == kCurveTo && rest >= 4) {
            QuadraticCurveTo(s, p[0], p[1], p[2], p[3]);
            index += 5;
        } else if (command == kCubic && rest >= 6) {
            BezierCurveTo(s, p[0], p[1], p[2], p[3], p[4], p[5], scale_squared);
            index += 7;
        } else if (command == kArc && rest >= 3) {
            Arc(s, p[0], p[1], p[2], scale_squared);
            index += 4;
        } else if (command == kClosePath) {
            ClosePath(s);
            index += 1;
        } else {
            break;   // 経路でない命令 (または途中で切れた命令): 呼び出し側が処理する
        }
    }
    return index;
}

// いまの経路を塗りのメッシュにしたときの要素数 (書きかけの経路を確定してから数える)
inline size_t FillSize(PathState& s)
{
    FinalizePaths(s, 9);
    size_t size = 0;
    for (const std::vector<double>& path : s.vertices) {
        size += (path.size() / 3 - 2) * kTriangleSize;
    }
    return size;
}

// メッシュを target[offset..] へ書き、頂点数を返す。経路は空になる
inline size_t FillMesh(PathState& s, float* target, size_t target_length, size_t offset,
                       float a, float b, float c, float d, float tx, float ty,
                       float red, float green, float blue, float alpha)
{
    FinalizePaths(s, 9);

    // 色と行列は、塗り 1 回の中ではどの頂点も同じ
    float triangle[kTriangleSize] = {};
    for (size_t vertex = 0; vertex < kTriangleSize; vertex += kVertexSize) {
        triangle[vertex + 4] = red;
        triangle[vertex + 5] = green;
        triangle[vertex + 6] = blue;
        triangle[vertex + 7] = alpha;
        triangle[vertex + 8] = a;
        triangle[vertex + 9] = b;
        triangle[vertex + 11] = c;
        triangle[vertex + 12] = d;
        triangle[vertex + 14] = tx;
        triangle[vertex + 15] = ty;
    }

    size_t index = 0;   // 書いた頂点数
    for (const std::vector<double>& path : s.vertices) {
        const double* v = path.data();
        const size_t length = path.size() - 5;
        for (size_t idx = 3; idx < length; idx += 3) {
            const size_t position = offset + index * kVertexSize;
            if (position + kTriangleSize > target_length) {
                break;   // 呼び出し側が fillSize の分を確保していれば起きない
            }
            float* out = target + position;
            std::memcpy(out, triangle, sizeof(triangle));
            if (v[idx + 2] != 0.0) {
                // 曲線 (前の点、制御点、次の点)
                out[0] = static_cast<float>(v[idx - 3]);
                out[1] = static_cast<float>(v[idx - 2]);
                out[2] = 0.0f;
                out[3] = 0.0f;
                out[17] = static_cast<float>(v[idx]);
                out[18] = static_cast<float>(v[idx + 1]);
                out[19] = 0.5f;
                out[20] = 0.0f;
                out[34] = static_cast<float>(v[idx + 3]);
                out[35] = static_cast<float>(v[idx + 4]);
                out[36] = 1.0f;
                out[37] = 1.0f;
            } else {
                // 扇形の三角形 (先頭の点、この点、次の点。次が制御点ならその先)
                const size_t next = v[idx + 5] != 0.0 ? idx + 6 : idx + 3;
                if (next + 1 >= path.size()) {
                    break;   // 終点の無い制御点で終わる経路 (通常は作られない)
                }
                out[0] = static_cast<float>(v[0]);
                out[1] = static_cast<float>(v[1]);
                out[2] = 0.5f;
                out[3] = 0.5f;
                out[17] = static_cast<float>(v[idx]);
                out[18] = static_cast<float>(v[idx + 1]);
                out[19] = 0.5f;
                out[20] = 0.5f;
                out[34] = static_cast<float>(v[next]);
                out[35] = static_cast<float>(v[next + 1]);
                out[36] = 0.5f;
                out[37] = 0.5f;
            }
            index += 3;
        }
    }
    s.vertices.clear();
    return index;
}

// ---- 描画データのハッシュ値 ----------------------------------------------------------
// player は、図形を初めて描くときに、その描画データ (Float32Array) 全体からハッシュ値を作って
// キャッシュの鍵にする。player の JavaScript (display の DisplayObjectGenerateHashService) と
// 同じ値を返す: 1 要素 (32 ビット) ごとに 1 回掛けて上位を下位へ折り返し、最後に 24 ビットへ縮める。
inline uint32_t HashFloat32(const float* data, size_t length)
{
    uint32_t hash = 2166136261u ^ static_cast<uint32_t>(length);
    for (size_t idx = 0; idx < length; ++idx) {
        uint32_t bits;
        std::memcpy(&bits, data + idx, sizeof(bits));
        hash = (hash ^ bits) * 0x9E3779B1u;
        hash ^= hash >> 15;
    }
    hash = (hash ^ (hash >> 16)) * 0x85EBCA6Bu;
    hash ^= hash >> 13;
    return ((hash >> 8) ^ (hash & 0xffu)) & 0xffffffu;
}

// ---- 線 (WebGL 版) -----------------------------------------------------------------
// player は線を「線の外周をなぞる経路 (区間ごとの四角形、つなぎ目、端)」に変換してから、塗りと
// 同じ方法でメッシュにする。ここでは経路を外周の経路に置き換えるところまでを行い、メッシュは
// FillMesh で作る。計算の順序は player の JavaScript (Mesh/usecase/MeshGenerateStrokeOutlineUseCase
// と、そこから呼ばれる処理) と同じ。
// 丸いつなぎ目・丸い端の三角関数 (atan2 / cos / sin) と、曲線の分割数を決める acos は、C++ の
// 標準ライブラリの実装を使う。JavaScript エンジンの実装とは最後の桁が違うことがあるが、メッシュは
// 32 ビットへ丸めて持つので、ほぼすべての頂点が同じ値になる。

using Path = std::vector<double>;

constexpr int kStrokeSplitCount = 5;
constexpr int kStrokeSegmentCount = 1 << kStrokeSplitCount;
constexpr double kStrokeTolerance = 0.05;
constexpr double kStrokeEpsilon = 0.0001;
constexpr double kStrokePi = 3.141592653589793;   // JavaScript の Math.PI

// JavaScript の Math.hypot(x, y) と同じ計算 (大きい方で割ってから 2 乗を足す。V8 の実装と同じ順序)
inline double JsHypot(double x, double y)
{
    const bool nan = x != x || y != y;
    const double ax = x != x ? 0.0 : std::fabs(x);
    const double ay = y != y ? 0.0 : std::fabs(y);
    const double max = ax > ay ? ax : ay;
    if (max == std::numeric_limits<double>::infinity()) {
        return max;
    }
    if (nan) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (max == 0) {
        return 0.0;
    }
    double sum = 0.0;
    double compensation = 0.0;
    const double values[2] = { ax, ay };
    for (int idx = 0; idx < 2; ++idx) {
        const double n = values[idx] / max;
        const double summand = n * n - compensation;
        const double preliminary = sum + summand;
        compensation = (preliminary - sum) - summand;
        sum = preliminary;
    }
    return std::sqrt(sum) * max;
}

inline void PushPathPoint(Path& path, double x, double y, double control)
{
    path.push_back(x);
    path.push_back(y);
    path.push_back(control);
}

// 直線の区間を、太さの分だけ両側へずらした四角形にする
inline void StrokeLineRectangle(double sx, double sy, double ex, double ey, double thickness, Path& out)
{
    const double vx = ex - sx;
    const double vy = ey - sy;
    const double magnitude = std::sqrt(vx * vx + vy * vy);
    const double nx = -(vy / magnitude) * thickness;
    const double ny = vx / magnitude * thickness;
    PushPathPoint(out, sx + nx, sy + ny, 0.0);
    PushPathPoint(out, ex + nx, ey + ny, 0.0);
    PushPathPoint(out, ex - nx, ey - ny, 0.0);
    PushPathPoint(out, sx - nx, sy - ny, 0.0);
    PushPathPoint(out, sx + nx, sy + ny, 0.0);
}

// 2 次ベジェ曲線上の t の位置を、法線の方向へ offset だけずらした点
inline void StrokeOffsetPoint(double t, double x0, double y0, double x1, double y1,
                              double x2, double y2, double offset, double* out)
{
    const double u = 1 - t;
    const double position_x = u * u * x0 + 2 * u * t * x1 + t * t * x2;
    const double position_y = u * u * y0 + 2 * u * t * y1 + t * t * y2;
    const double tangent_x = 2 * u * (x1 - x0) + 2 * t * (x2 - x1);
    const double tangent_y = 2 * u * (y1 - y0) + 2 * t * (y2 - y1);
    const double normal_x = -tangent_y;
    const double normal_y = tangent_x;
    const double length = std::sqrt(normal_x * normal_x + normal_y * normal_y);
    if (length == 0) {
        out[0] = position_x + 0 * offset;
        out[1] = position_y + 0 * offset;
    } else {
        out[0] = position_x + normal_x / length * offset;
        out[1] = position_y + normal_y / length * offset;
    }
}

// 曲線の区間を半分に分ける回数 (画面上の誤差が kStrokeTolerance ピクセル以下になる最小の回数)
inline int StrokeSplitCount(double x0, double y0, double x1, double y1, double x2, double y2,
                            double thickness, double pixel_scale)
{
    if (!(pixel_scale > 0)) {
        return kStrokeSplitCount;
    }
    const double ax = x1 - x0;
    const double ay = y1 - y0;
    const double bx = x2 - x1;
    const double by = y2 - y1;
    const double length_a = std::sqrt(ax * ax + ay * ay);
    const double length_b = std::sqrt(bx * bx + by * by);
    if (length_a == 0 || length_b == 0) {
        return kStrokeSplitCount;
    }
    double cosine = (ax * bx + ay * by) / (length_a * length_b);
    if (cosine > 1) {
        cosine = 1;
    } else if (cosine < -1) {
        cosine = -1;
    }
    const double theta = std::acos(cosine);
    if (theta > 2) {
        return kStrokeSplitCount;
    }
    const double dx = x0 - 2 * x1 + x2;
    const double dy = y0 - 2 * y1 + y2;
    const double sagitta = std::sqrt(dx * dx + dy * dy) / 4
        + std::fabs(thickness) * theta * theta / 8;
    double error = sagitta * 0.75 * pixel_scale;
    int count = 0;
    while (error > kStrokeTolerance && count < kStrokeSplitCount) {
        error *= 0.25;
        ++count;
    }
    return count;
}

// 曲線の区間を、太さの分だけ両側へずらした輪郭にする (左側を順に、右側を逆順にたどる)。
// join_previous: 右側の各区間の始点に、その区間の t = 0 の点ではなく、前の区間の t = 1 の点を使う
// (WebGPU 版の規則。同じ位置だが、計算に使う区間が違うので最後の桁が変わりうる)
inline void StrokeCurveRectangle(double sx, double sy, double cx, double cy, double ex, double ey,
                                 double thickness, double pixel_scale, Path& out,
                                 bool join_previous = false)
{
    const int split_count = StrokeSplitCount(sx, sy, cx, cy, ex, ey, thickness, pixel_scale);

    double buffer_a[kStrokeSegmentCount * 6];
    double buffer_b[kStrokeSegmentCount * 6];
    double right[kStrokeSegmentCount * 6];
    double* segments = buffer_a;
    double* next = buffer_b;
    segments[0] = sx; segments[1] = sy;
    segments[2] = cx; segments[3] = cy;
    segments[4] = ex; segments[5] = ey;

    int count = 1;
    for (int split = 0; split < split_count; ++split) {
        for (int idx = 0; idx < count; ++idx) {
            const double* p = segments + idx * 6;
            const double x0 = p[0], y0 = p[1];
            const double x1 = p[2], y1 = p[3];
            const double x2 = p[4], y2 = p[5];
            const double m0x = x0 + (x1 - x0) * 0.5;
            const double m0y = y0 + (y1 - y0) * 0.5;
            const double m1x = x1 + (x2 - x1) * 0.5;
            const double m1y = y1 + (y2 - y1) * 0.5;
            const double m01x = m0x + (m1x - m0x) * 0.5;
            const double m01y = m0y + (m1y - m0y) * 0.5;
            double* q = next + idx * 12;
            q[0] = x0;   q[1] = y0;
            q[2] = m0x;  q[3] = m0y;
            q[4] = m01x; q[5] = m01y;
            q[6] = m01x; q[7] = m01y;
            q[8] = m1x;  q[9] = m1y;
            q[10] = x2;  q[11] = y2;
        }
        double* swap = segments;
        segments = next;
        next = swap;
        count *= 2;
    }

    double point[2];
    StrokeOffsetPoint(0, segments[0], segments[1], segments[2], segments[3],
                      segments[4], segments[5], +thickness, point);
    PushPathPoint(out, point[0], point[1], 0.0);

    for (int idx = 0; idx < count; ++idx) {
        const int source = idx * 6;
        const double* p = segments + source;
        const double x0 = p[0], y0 = p[1];
        const double x1 = p[2], y1 = p[3];
        const double x2 = p[4], y2 = p[5];

        double control[2];
        StrokeOffsetPoint(0.5, x0, y0, x1, y1, x2, y2, +thickness, control);
        StrokeOffsetPoint(1, x0, y0, x1, y1, x2, y2, +thickness, point);
        PushPathPoint(out, control[0], control[1], 1.0);
        PushPathPoint(out, point[0], point[1], 0.0);

        if (!join_previous || idx == 0) {
            StrokeOffsetPoint(0, x0, y0, x1, y1, x2, y2, -thickness, right + source);
        }
        StrokeOffsetPoint(0.5, x0, y0, x1, y1, x2, y2, -thickness, right + source + 2);
        if (join_previous || idx == count - 1) {
            StrokeOffsetPoint(1, x0, y0, x1, y1, x2, y2, -thickness, right + source + 4);
        }
    }

    const int last = (count - 1) * 6;
    PushPathPoint(out, right[last + 4], right[last + 5], 0.0);
    for (int idx = count - 1; idx >= 0; --idx) {
        const int source = idx * 6;
        PushPathPoint(out, right[source + 2], right[source + 3], 1.0);
        if (join_previous && idx > 0) {
            PushPathPoint(out, right[source - 2], right[source - 1], 0.0);
        } else {
            PushPathPoint(out, right[source], right[source + 1], 0.0);
        }
    }
}

// 輪郭の点 (制御点を除く) のうち、(x, y) からの距離が r のものを (x, y) の並びで集める
inline void StrokeFindOverlapping(double x, double y, double r, const Path& path, std::vector<double>& out)
{
    out.clear();
    for (size_t idx = 0; idx + 2 < path.size(); idx += 3) {
        if (path[idx + 2] != 0.0) {
            continue;
        }
        const double dx = path[idx];
        const double dy = path[idx + 1];
        const double distance = std::sqrt((dx - x) * (dx - x) + (dy - y) * (dy - y));
        if (std::fabs(distance - r) > kStrokeEpsilon) {
            continue;
        }
        out.push_back(dx);
        out.push_back(dy);
    }
}

// points のうち、輪郭 rectangle の外にある最初の点を返す (すべて内側なら false)。
// player は canvas の isPointInPath で判定するので、実行環境の canvas の実装 (RasterCore.h の
// FlattenQuadratic と PointInPath) と同じ計算をする: 曲線は 12 本の直線に分け、巻き数で判定する。
inline bool StrokeFirstPointOutside(const std::vector<double>& points, const Path& rectangle,
                                    std::vector<double>& polygon, double* out)
{
    polygon.clear();
    const size_t size = rectangle.size();
    auto value = [&](size_t index) { return index < size ? rectangle[index] : 0.0; };
    polygon.push_back(value(0));
    polygon.push_back(value(1));
    for (size_t idx = 3; idx < size; idx += 3) {
        if (value(idx + 2) != 0.0) {
            const double p0x = polygon[polygon.size() - 2];
            const double p0y = polygon[polygon.size() - 1];
            const double p1x = value(idx);
            const double p1y = value(idx + 1);
            const double p2x = value(idx + 3);
            const double p2y = value(idx + 4);
            for (int step = 1; step <= 12; ++step) {
                const double t = static_cast<double>(step) / 12;
                const double u = 1 - t;
                polygon.push_back(u * u * p0x + 2 * u * t * p1x + t * t * p2x);
                polygon.push_back(u * u * p0y + 2 * u * t * p1y + t * t * p2y);
            }
            idx += 3;
        } else {
            polygon.push_back(value(idx));
            polygon.push_back(value(idx + 1));
        }
    }

    const size_t count = polygon.size() / 2;
    for (size_t idx = 0; idx + 1 < points.size(); idx += 2) {
        const double px = points[idx];
        const double py = points[idx + 1];
        int wind = 0;
        if (count >= 2) {
            for (size_t i = 0; i < count; ++i) {
                const size_t j = (i + 1) % count;
                const double ax = polygon[i * 2], ay = polygon[i * 2 + 1];
                const double bx = polygon[j * 2], by = polygon[j * 2 + 1];
                if ((ay <= py && by > py) || (by <= py && ay > py)) {
                    const double t = (py - ay) / (by - ay);
                    const double cross = ax + t * (bx - ax);
                    if (cross > px) {
                        wind += ay < by ? 1 : -1;
                    }
                }
            }
        }
        if (wind != 0) {
            continue;
        }
        out[0] = px;
        out[1] = py;
        return true;
    }
    return false;
}

// JavaScript の a[i] === b[j] (どちらも範囲の外なら等しい)
inline bool StrokeSameValue(const std::vector<double>& a, size_t i, const std::vector<double>& b, size_t j)
{
    const bool has_a = i < a.size();
    const bool has_b = j < b.size();
    if (!has_a || !has_b) {
        return has_a == has_b;
    }
    return a[i] == b[j];
}

// つなぎ目の計算で使い回す作業領域
struct StrokeWork {
    std::vector<double> points_a;
    std::vector<double> points_b;
    std::vector<double> polygon;
    // WebGPU 版の規則でつなぎ目を作る (曲線どうしでも作る。丸いつなぎ目を角度で省かない)
    bool webgpu_rules = false;
};

// つなぎ目を作る 2 つの輪郭から、互いの外側にある点を 1 つずつ探す。つなぎ目が要らなければ false
inline bool StrokeJoinPoints(double x, double y, double r, const std::vector<Path>& rectangles,
                             bool is_last, bool skip_parallel, StrokeWork& work,
                             double* point_a, double* point_b)
{
    const size_t index_a = is_last ? 0 : rectangles.size() - 1;
    const size_t index_b = is_last ? rectangles.size() - 1 : rectangles.size() - 2;
    const Path& rect_a = rectangles[index_a];
    const Path& rect_b = rectangles[index_b];

    // 曲線の輪郭どうしはなめらかにつながっているので、つなぎ目は要らない (WebGL 版の規則)
    if (!work.webgpu_rules && rect_a.size() > 15 && rect_b.size() > 15) {
        return false;
    }

    StrokeFindOverlapping(x, y, r, rect_a, work.points_a);
    StrokeFindOverlapping(x, y, r, rect_b, work.points_b);

    // 平行なら要らない
    if (skip_parallel
        && ((StrokeSameValue(work.points_a, 0, work.points_b, 0)
                && StrokeSameValue(work.points_a, 1, work.points_b, 1))
            || (StrokeSameValue(work.points_a, 0, work.points_b, 2)
                && StrokeSameValue(work.points_a, 1, work.points_b, 3)))
    ) {
        return false;
    }

    if (!StrokeFirstPointOutside(work.points_a, rect_b, work.polygon, point_a)) {
        return false;
    }
    if (!StrokeFirstPointOutside(work.points_b, rect_a, work.polygon, point_b)) {
        return false;
    }
    return true;
}

// つなぎ目の経路を、最後の輪郭の手前へ入れる
inline void StrokeInsertJoin(std::vector<Path>& rectangles, Path&& join)
{
    rectangles.insert(rectangles.end() - 1, std::move(join));
}

inline void StrokeBevelJoin(double x, double y, double r, std::vector<Path>& rectangles,
                            bool is_last, StrokeWork& work)
{
    double a[2];
    double b[2];
    if (!StrokeJoinPoints(x, y, r, rectangles, is_last, true, work, a, b)) {
        return;
    }
    Path join;
    PushPathPoint(join, x, y, 0.0);
    PushPathPoint(join, a[0], a[1], 0.0);
    PushPathPoint(join, b[0], b[1], 0.0);
    PushPathPoint(join, x, y, 0.0);
    StrokeInsertJoin(rectangles, std::move(join));
}

inline void StrokeMiterJoin(double sx, double sy, double ex, double ey, double px, double py,
                            double r, std::vector<Path>& rectangles, bool is_last, StrokeWork& work)
{
    double a[2];
    double b[2];
    if (!StrokeJoinPoints(sx, sy, r, rectangles, is_last, true, work, a, b)) {
        return;
    }

    // 外側の 2 点を、それぞれの線の方向へ延ばした交点を求める
    const double a_vx = ex - sx;
    const double a_vy = ey - sy;
    const double length_a = JsHypot(a_vx, a_vy);
    const double d1x = a_vx / length_a;
    const double d1y = a_vy / length_a;

    const double b_vx = px - sx;
    const double b_vy = py - sy;
    const double length_b = JsHypot(b_vx, b_vy);
    const double d2x = b_vx / length_b;
    const double d2y = b_vy / length_b;

    Path join;
    const double denom = d1x * d2y - d1y * d2x;
    if (denom == 0) {
        PushPathPoint(join, sx, sy, 0.0);
        PushPathPoint(join, a[0], a[1], 0.0);
        PushPathPoint(join, b[0], b[1], 0.0);
        StrokeInsertJoin(rectangles, std::move(join));
        return;
    }

    const double t = ((b[0] - a[0]) * d2y - (b[1] - a[1]) * d2x) / denom;
    const double ix = a[0] + t * d1x;
    const double iy = a[1] + t * d1y;
    PushPathPoint(join, sx, sy, 0.0);
    PushPathPoint(join, a[0], a[1], 0.0);
    PushPathPoint(join, ix, iy, 0.0);
    PushPathPoint(join, sx, sy, 0.0);
    PushPathPoint(join, b[0], b[1], 0.0);
    PushPathPoint(join, ix, iy, 0.0);
    StrokeInsertJoin(rectangles, std::move(join));
}

inline void StrokeRoundJoin(double x, double y, double r, std::vector<Path>& rectangles,
                            bool is_last, StrokeWork& work)
{
    double a[2];
    double b[2];
    if (!StrokeJoinPoints(x, y, r, rectangles, is_last, false, work, a, b)) {
        return;
    }

    const double angle_a = std::atan2(a[1] - y, a[0] - x);
    const double angle_b = std::atan2(b[1] - y, b[0] - x);

    // 角度の差を -180 度から 180 度の範囲にする
    double angle_diff = angle_b - angle_a;
    if (angle_diff > kStrokePi) {
        angle_diff -= 2 * kStrokePi;
    } else if (angle_diff < -kStrokePi) {
        angle_diff += 2 * kStrokePi;
    }

    // 差が小さいとき、180 度に近いときは作らない (WebGL 版の規則)
    const double abs_diff = std::fabs(angle_diff);
    if (!work.webgpu_rules && (abs_diff < 0.1 || abs_diff > kStrokePi - 0.1)) {
        return;
    }

    const int segment = 8;
    const double step = angle_diff / segment;
    Path join;
    PushPathPoint(join, x, y, 0.0);
    for (int idx = 0; idx <= segment; ++idx) {
        const double angle = angle_a + idx * step;
        PushPathPoint(join, x + r * std::cos(angle), y + r * std::sin(angle), 0.0);
    }
    StrokeInsertJoin(rectangles, std::move(join));
}

// 経路の端の点と、その隣の点 (端の向きを決める)
inline void StrokeCapEnds(const Path& v, int end, double* sx, double* sy, double* ex, double* ey)
{
    const size_t size = v.size();
    if (end == 0) {
        *sx = v[0];        *sy = v[1];
        *ex = v[3];        *ey = v[4];
    } else {
        *sx = v[size - 3]; *sy = v[size - 2];
        *ex = v[size - 6]; *ey = v[size - 5];
    }
}

inline void StrokeRoundCap(const Path& v, double r, std::vector<Path>& rectangles)
{
    for (int end = 0; end < 2; ++end) {
        double sx, sy, ex, ey;
        StrokeCapEnds(v, end, &sx, &sy, &ex, &ey);

        const double angle_to_end = std::atan2(ey - sy, ex - sx);
        const double start_angle = angle_to_end + kStrokePi / 2;
        const double end_angle = angle_to_end - kStrokePi / 2;

        const int segments = 16;
        const double step = (end_angle - start_angle) / segments;
        Path points;
        for (int idx = 0; idx <= segments; ++idx) {
            const double t = end_angle + step * idx;
            PushPathPoint(points, sx + r * std::cos(t), sy + r * std::sin(t), 0.0);
        }
        if (end == 0) {
            rectangles.insert(rectangles.begin(), std::move(points));
        } else {
            rectangles.push_back(std::move(points));
        }
    }
}

inline void StrokeSquareCap(const Path& v, double r, std::vector<Path>& rectangles)
{
    for (int end = 0; end < 2; ++end) {
        double sx, sy, ex, ey;
        StrokeCapEnds(v, end, &sx, &sy, &ex, &ey);

        const double dx = -(ex - sx);
        const double dy = -(ey - sy);
        const double length = std::sqrt(dx * dx + dy * dy);
        if (length == 0) {
            continue;
        }
        const double ux = dx / length;
        const double uy = dy / length;
        const double cxn = sx + r * ux;
        const double cyn = sy + r * uy;
        const double vx = -uy;
        const double vy = ux;

        Path points;
        PushPathPoint(points, cxn + r * ux + r * vx, cyn + r * uy + r * vy, 0.0);
        PushPathPoint(points, cxn + r * ux - r * vx, cyn + r * uy - r * vy, 0.0);
        PushPathPoint(points, cxn - r * ux - r * vx, cyn - r * uy - r * vy, 0.0);
        PushPathPoint(points, cxn + r * ux + r * vx, cyn + r * uy + r * vy, 0.0);
        PushPathPoint(points, cxn - r * ux - r * vx, cyn - r * uy - r * vy, 0.0);
        PushPathPoint(points, cxn - r * ux + r * vx, cyn - r * uy + r * vy, 0.0);
        if (end == 0) {
            rectangles.insert(rectangles.begin(), std::move(points));
        } else {
            rectangles.push_back(std::move(points));
        }
    }
}

// 経路 1 本の線の外周を、塗りと同じ形式の経路の並びにして out の末尾へ足す。
// joints: 0 = bevel / 1 = miter / 2 = round、caps: 1 = round / 2 = square (ほかは何も足さない)
inline void StrokeOutline(const Path& v, double thickness, int joints, int caps, double pixel_scale,
                          StrokeWork& work, std::vector<Path>& out)
{
    const size_t size = v.size();
    if (size < 6) {
        return;
    }

    double start_x = v[0];
    double start_y = v[1];
    double control_x = 0.0;
    double control_y = 0.0;

    std::vector<Path> rectangles;
    for (size_t idx = 3; idx + 2 < size; idx += 3) {
        const double x = v[idx];
        const double y = v[idx + 1];
        if (v[idx + 2] != 0.0) {
            control_x = x;
            control_y = y;
            continue;
        }

        rectangles.emplace_back();
        if (v[idx - 1] != 0.0) {
            StrokeCurveRectangle(start_x, start_y, control_x, control_y, x, y,
                                 thickness, pixel_scale, rectangles.back());
        } else {
            StrokeLineRectangle(start_x, start_y, x, y, thickness, rectangles.back());
        }

        if (rectangles.size() > 1) {
            if (joints == 0) {
                StrokeBevelJoin(start_x, start_y, thickness, rectangles, false, work);
            } else if (joints == 1) {
                StrokeMiterJoin(start_x, start_y, x, y, v[idx - 6], v[idx - 5],
                                thickness, rectangles, false, work);
            } else if (joints == 2) {
                StrokeRoundJoin(start_x, start_y, thickness, rectangles, false, work);
            }
        }

        start_x = x;
        start_y = y;
    }

    // 始点と終点がつながっていれば、そこにもつなぎ目を作る。つながっていなければ端を作る
    const double closed_start_x = v[0];
    const double closed_start_y = v[1];
    const double closed_end_x = v[size - 3];
    const double closed_end_y = v[size - 2];
    const bool is_closed = std::fabs(closed_start_x - closed_end_x) < kStrokeEpsilon
        && std::fabs(closed_start_y - closed_end_y) < kStrokeEpsilon
        && rectangles.size() > 1;

    if (is_closed) {
        if (joints == 0) {
            StrokeBevelJoin(closed_start_x, closed_start_y, thickness, rectangles, true, work);
        } else if (joints == 1) {
            StrokeMiterJoin(closed_start_x, closed_start_y, v[3], v[4], v[size - 6], v[size - 5],
                            thickness, rectangles, true, work);
        } else if (joints == 2) {
            StrokeRoundJoin(closed_start_x, closed_start_y, thickness, rectangles, true, work);
        }
    } else if (caps == 1) {
        StrokeRoundCap(v, thickness, rectangles);
    } else if (caps == 2) {
        StrokeSquareCap(v, thickness, rectangles);
    }

    for (Path& rectangle : rectangles) {
        out.push_back(std::move(rectangle));
    }
}

// いまの経路を線の外周の経路に置き換え、それを塗りのメッシュにしたときの要素数を返す
// (0 なら描く線が無い)。続けて FillMesh を呼ぶと、線のメッシュが得られる。
// thickness は線の太さの半分。pixel_scale は座標 1 単位が画面上で何ピクセルになるか。
inline size_t StrokeSize(PathState& s, double thickness, int joints, int caps, double pixel_scale)
{
    FinalizePaths(s, 6);

    StrokeWork work;
    std::vector<Path> outlines;
    for (const Path& path : s.vertices) {
        StrokeOutline(path, thickness, joints, caps, pixel_scale, work, outlines);
    }
    s.vertices = std::move(outlines);

    size_t size = 0;
    for (const Path& path : s.vertices) {
        size += (path.size() / 3 - 2) * kTriangleSize;
    }
    return size;
}

// ---- WebGPU 版の player と同じ規則------------------------------------------------
// WebGL 版との主な違い:
//   - 経路は 64 ビットのまま持ち、3 次ベジェ曲線の分割の途中でも 32 ビットへ丸めない
//   - moveTo は常に新しい経路を始める。同じ点かどうかは「いまの位置」と比べる
//   - 円弧は、画面上の大きさによらない方法 (直線からの離れ具合) で分割する
//   - メッシュの頂点は 4 要素 (位置 2、曲線用の係数 2)。色と行列は別に渡す
//   - 塗りは経路を消さない (同じ経路を何度でも取り出せる)
namespace gpu {

constexpr size_t kVertexSize = 4;
constexpr size_t kTriangleSize = kVertexSize * 3;
constexpr double kFlatnessThreshold = 0.25;
constexpr int kMaxRecursionDepth = 8;

struct PathState {
    std::vector<double> current;
    std::vector<std::vector<double>> vertices;
    double current_x = 0.0;
    double current_y = 0.0;
    double start_x = 0.0;
    double start_y = 0.0;
};

inline void Reset(PathState& s)
{
    s.current.clear();
    s.vertices.clear();
    s.current_x = 0.0;
    s.current_y = 0.0;
    s.start_x = 0.0;
    s.start_y = 0.0;
}

inline void Push(PathState& s, double x, double y, double control)
{
    s.current.push_back(x);
    s.current.push_back(y);
    s.current.push_back(control);
}

inline void MoveTo(PathState& s, double x, double y)
{
    if (s.current.size() >= 6) {
        s.vertices.push_back(s.current);
    }
    s.current.clear();
    Push(s, x, y, 0.0);
    s.current_x = x;
    s.current_y = y;
    s.start_x = x;
    s.start_y = y;
}

inline void LineTo(PathState& s, double x, double y)
{
    if (x == s.current_x && y == s.current_y) {
        return;
    }
    Push(s, x, y, 0.0);
    s.current_x = x;
    s.current_y = y;
}

inline void QuadraticCurveTo(PathState& s, double cx, double cy, double x, double y)
{
    Push(s, cx, cy, 1.0);
    Push(s, x, y, 0.0);
    s.current_x = x;
    s.current_y = y;
}

// JavaScript の Math.max と同じ (どちらかが NaN なら NaN)
inline double JsMax(double a, double b)
{
    if (a != a || b != b) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return a > b ? a : b;
}

// 制御点が、始点と終点を結ぶ直線からどれだけ離れているか (距離の 2 乗の大きい方)
inline double Flatness(double p0x, double p0y, double p1x, double p1y,
                       double p2x, double p2y, double p3x, double p3y)
{
    const double dx = p3x - p0x;
    const double dy = p3y - p0y;
    const double length_squared = dx * dx + dy * dy;
    if (length_squared < 1e-10) {
        const double d1 = (p1x - p0x) * (p1x - p0x) + (p1y - p0y) * (p1y - p0y);
        const double d2 = (p2x - p0x) * (p2x - p0x) + (p2y - p0y) * (p2y - p0y);
        return JsMax(d1, d2);
    }
    const double cross1 = (p1x - p0x) * dy - (p1y - p0y) * dx;
    const double d1 = cross1 * cross1 / length_squared;
    const double cross2 = (p2x - p0x) * dy - (p2y - p0y) * dx;
    const double d2 = cross2 * cross2 / length_squared;
    return JsMax(d1, d2);
}

// 直線に近くなるまで半分に分け、それぞれを 2 次ベジェ曲線 1 本にする
inline void Subdivide(PathState& s, double p0x, double p0y, double p1x, double p1y,
                      double p2x, double p2y, double p3x, double p3y, int depth)
{
    const double flatness = Flatness(p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y);
    if (flatness <= kFlatnessThreshold || depth >= kMaxRecursionDepth) {
        Push(s, (p1x + p2x) * 0.5, (p1y + p2y) * 0.5, 1.0);
        Push(s, p3x, p3y, 0.0);
        return;
    }
    const double p01x = (p0x + p1x) * 0.5,  p01y = (p0y + p1y) * 0.5;
    const double p12x = (p1x + p2x) * 0.5,  p12y = (p1y + p2y) * 0.5;
    const double p23x = (p2x + p3x) * 0.5,  p23y = (p2y + p3y) * 0.5;
    const double p012x = (p01x + p12x) * 0.5, p012y = (p01y + p12y) * 0.5;
    const double p123x = (p12x + p23x) * 0.5, p123y = (p12y + p23y) * 0.5;
    const double mx = (p012x + p123x) * 0.5,  my = (p012y + p123y) * 0.5;
    Subdivide(s, p0x, p0y, p01x, p01y, p012x, p012y, mx, my, depth + 1);
    Subdivide(s, mx, my, p123x, p123y, p23x, p23y, p3x, p3y, depth + 1);
}

// pixel_scale: 曲線の座標 1 単位が画面上で何ピクセルになるか。0 以下なら Subdivide で分ける
inline void BezierCurveTo(PathState& s, double cx1, double cy1, double cx2, double cy2,
                          double x, double y, double pixel_scale)
{
    if (pixel_scale > 0) {
        const double from_x = s.current_x;
        const double from_y = s.current_y;
        const double dx = x - 3 * cx2 + 3 * cx1 - from_x;
        const double dy = y - 3 * cy2 + 3 * cy1 - from_y;
        if ((dx * dx + dy * dy) * pixel_scale * pixel_scale <= kCubicSingleSegmentLimit) {
            Push(s, (3 * cx1 + 3 * cx2 - from_x - x) * 0.25,
                    (3 * cy1 + 3 * cy2 - from_y - y) * 0.25, 1.0);
            Push(s, x, y, 0.0);
            s.current_x = x;
            s.current_y = y;
            return;
        }

        // 残りの曲線の先頭 1 / (残りの本数) を切り出すことを繰り返す
        const int count = CubicSegmentCount(from_x, from_y, cx1, cy1, cx2, cy2, x, y, pixel_scale);
        double x0 = from_x, y0 = from_y;
        double x1 = cx1, y1 = cy1;
        double x2 = cx2, y2 = cy2;
        for (int idx = 0; idx < count - 1; ++idx) {
            const double t = 1.0 / (count - idx);
            const double mt = 1 - t;
            const double q0x = mt * x0 + t * x1;
            const double q0y = mt * y0 + t * y1;
            const double q1x = mt * x1 + t * x2;
            const double q1y = mt * y1 + t * y2;
            const double q2x = mt * x2 + t * x;
            const double q2y = mt * y2 + t * y;
            const double r0x = mt * q0x + t * q1x;
            const double r0y = mt * q0y + t * q1y;
            const double r1x = mt * q1x + t * q2x;
            const double r1y = mt * q1y + t * q2y;
            const double sx = mt * r0x + t * r1x;
            const double sy = mt * r0y + t * r1y;
            Push(s, (3 * q0x + 3 * r0x - x0 - sx) * 0.25, (3 * q0y + 3 * r0y - y0 - sy) * 0.25, 1.0);
            Push(s, sx, sy, 0.0);
            x0 = sx;  y0 = sy;
            x1 = r1x; y1 = r1y;
            x2 = q2x; y2 = q2y;
        }
        Push(s, (3 * x1 + 3 * x2 - x0 - x) * 0.25, (3 * y1 + 3 * y2 - y0 - y) * 0.25, 1.0);
        Push(s, x, y, 0.0);
    } else {
        Subdivide(s, s.current_x, s.current_y, cx1, cy1, cx2, cy2, x, y, 0);
    }
    s.current_x = x;
    s.current_y = y;
}

inline void Arc(PathState& s, double x, double y, double radius)
{
    const double r = radius;
    const double k = radius * 0.5522847498307936;
    BezierCurveTo(s, x + r, y + k, x + k, y + r, x, y + r, 0.0);
    BezierCurveTo(s, x - k, y + r, x - r, y + k, x - r, y, 0.0);
    BezierCurveTo(s, x - r, y - k, x - k, y - r, x, y - r, 0.0);
    BezierCurveTo(s, x + k, y - r, x + r, y - k, x + r, y, 0.0);

    // 閉じた円の終点を、経路の始点と同じ値にする
    const size_t length = s.current.size();
    if (length >= 3) {
        s.current[length - 3] = s.start_x;
        s.current[length - 2] = s.start_y;
    }
    s.current_x = s.start_x;
    s.current_y = s.start_y;
}

inline void ClosePath(PathState& s)
{
    if (s.current.size() >= 3) {
        if (s.current_x != s.start_x || s.current_y != s.start_y) {
            Push(s, s.start_x, s.start_y, 0.0);
        }
    }
}

// commands[index] から続く経路の命令をまとめて処理し、経路でない命令の位置 (または終端) を返す
inline size_t FeedPath(PathState& s, const float* commands, size_t length, size_t index, double pixel_scale)
{
    while (index < length) {
        const int command = static_cast<int>(commands[index]);
        const float* p = commands + index + 1;
        const size_t rest = length - index - 1;
        if (command == kMoveTo && rest >= 2) {
            MoveTo(s, p[0], p[1]);
            index += 3;
        } else if (command == kLineTo && rest >= 2) {
            LineTo(s, p[0], p[1]);
            index += 3;
        } else if (command == kCurveTo && rest >= 4) {
            QuadraticCurveTo(s, p[0], p[1], p[2], p[3]);
            index += 5;
        } else if (command == kCubic && rest >= 6) {
            BezierCurveTo(s, p[0], p[1], p[2], p[3], p[4], p[5], pixel_scale);
            index += 7;
        } else if (command == kArc && rest >= 3) {
            Arc(s, p[0], p[1], p[2]);
            index += 4;
        } else if (command == kClosePath) {
            ClosePath(s);
            index += 1;
        } else {
            break;
        }
    }
    return index;
}

// 経路 1 本からできる三角形の数
inline size_t TriangleCount(const std::vector<double>& path)
{
    return path.size() >= 9 ? path.size() / 3 - 2 : 0;
}

// 書きかけの経路を、塗り (9 要素以上) または線 (6 要素以上) に使うかどうか
inline bool UsesCurrent(const PathState& s, bool stroke)
{
    return s.current.size() >= (stroke ? 6u : 9u);
}

// 塗りに使う経路が 1 本でもあるか (三角形ができるかどうかは問わない)
inline bool HasFillPath(const PathState& s)
{
    return !s.vertices.empty() || UsesCurrent(s, false);
}

// いまの経路を塗りのメッシュにしたときの要素数
inline size_t FillSize(const PathState& s)
{
    size_t triangles = 0;
    for (const std::vector<double>& path : s.vertices) {
        triangles += TriangleCount(path);
    }
    if (UsesCurrent(s, false)) {
        triangles += TriangleCount(s.current);
    }
    return triangles * kTriangleSize;
}

// flat: 曲線用の係数をすべて 0.5 にする (グラデーションや画像で描く線のメッシュの形式)
inline size_t FillPath(const std::vector<double>& path, float* target, size_t target_length, size_t index,
                       bool flat = false)
{
    const double* v = path.data();
    const size_t size = path.size();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (size_t idx = 3; idx + 5 < size; idx += 3) {
        const size_t position = index * kVertexSize;
        if (position + kTriangleSize > target_length) {
            break;   // 呼び出し側が fillSize の分を確保していれば起きない
        }
        float* out = target + position;
        if (v[idx + 2] != 0.0) {
            // 曲線 (前の点、制御点、次の点)
            out[0] = static_cast<float>(v[idx - 3]);
            out[1] = static_cast<float>(v[idx - 2]);
            out[2] = flat ? 0.5f : 0.0f;
            out[3] = flat ? 0.5f : 0.0f;
            out[4] = static_cast<float>(v[idx]);
            out[5] = static_cast<float>(v[idx + 1]);
            out[6] = 0.5f;
            out[7] = flat ? 0.5f : 0.0f;
            out[8] = static_cast<float>(v[idx + 3]);
            out[9] = static_cast<float>(v[idx + 4]);
            out[10] = flat ? 0.5f : 1.0f;
            out[11] = flat ? 0.5f : 1.0f;
        } else {
            // 扇形の三角形 (先頭の点、この点、次の点。次が制御点ならその先)
            const size_t next = v[idx + 5] != 0.0 ? idx + 6 : idx + 3;
            out[0] = static_cast<float>(v[0]);
            out[1] = static_cast<float>(v[1]);
            out[2] = 0.5f;
            out[3] = 0.5f;
            out[4] = static_cast<float>(v[idx]);
            out[5] = static_cast<float>(v[idx + 1]);
            out[6] = 0.5f;
            out[7] = 0.5f;
            // 終点の無い制御点で終わる経路 (通常は作られない) では、JavaScript と同じく値が無い
            out[8] = next + 1 < size ? static_cast<float>(v[next]) : nan;
            out[9] = next + 1 < size ? static_cast<float>(v[next + 1]) : nan;
            out[10] = 0.5f;
            out[11] = 0.5f;
        }
        index += 3;
    }
    return index;
}

// メッシュを target の先頭から書き、頂点数を返す。経路はそのまま残る
inline size_t FillMesh(const PathState& s, float* target, size_t target_length, bool flat = false)
{
    size_t index = 0;
    for (const std::vector<double>& path : s.vertices) {
        index = FillPath(path, target, target_length, index, flat);
    }
    if (UsesCurrent(s, false)) {
        index = FillPath(s.current, target, target_length, index, flat);
    }
    return index;
}

// ---- 線 (WebGPU 版) ----------------------------------------------------------------
// WebGL 版と同じ考え方 (線の外周をなぞる経路に変換して、塗りと同じ方法でメッシュにする) だが、
// player の WebGPU 版 (Mesh/usecase/MeshStrokeGenerateUseCase) は次の点が違う:
//   - 長さ 0 の直線は、太さ 0 の四角形になる (WebGL 版は値が無い)
//   - 曲線の輪郭の右側は、各区間の始点に前の区間の終点を使う
//   - 曲線どうしのつなぎ目も作る。丸いつなぎ目を角度で省かない
//   - 丸い端は 8 分割の扇形、四角い端は 5 点の四角形

inline void StrokeLineRectangle(double sx, double sy, double ex, double ey, double thickness, Path& out)
{
    const double vx = ex - sx;
    const double vy = ey - sy;
    const double magnitude = std::sqrt(vx * vx + vy * vy);
    double nx = 0.0;
    double ny = 0.0;
    if (magnitude != 0) {
        nx = -(vy / magnitude) * thickness;
        ny = vx / magnitude * thickness;
    }
    PushPathPoint(out, sx + nx, sy + ny, 0.0);
    PushPathPoint(out, ex + nx, ey + ny, 0.0);
    PushPathPoint(out, ex - nx, ey - ny, 0.0);
    PushPathPoint(out, sx - nx, sy - ny, 0.0);
    PushPathPoint(out, sx + nx, sy + ny, 0.0);
}

// 端の点から外へ向かう半円 (端の点を中心にした、8 分割の扇形)
inline void StrokeRoundCapPath(double x, double y, double from_x, double from_y, double r, Path& out)
{
    const double base = std::atan2(y - from_y, x - from_x);
    const int segment = 8;
    PushPathPoint(out, x, y, 0.0);
    for (int idx = 0; idx <= segment; ++idx) {
        const double angle = base - kStrokePi / 2 + idx * kStrokePi / segment;
        PushPathPoint(out, x + r * std::cos(angle), y + r * std::sin(angle), 0.0);
    }
}

inline void StrokeRoundCap(const Path& v, double r, std::vector<Path>& rectangles)
{
    const size_t size = v.size();
    Path start;
    StrokeRoundCapPath(v[0], v[1], v[3], v[4], r, start);
    rectangles.insert(rectangles.begin(), std::move(start));

    Path end;
    StrokeRoundCapPath(v[size - 3], v[size - 2], v[size - 6], v[size - 5], r, end);
    rectangles.push_back(std::move(end));
}

// 端の点から外へ、太さの分だけ延ばした四角形。端の向きが決まらなければ false
inline bool StrokeSquareCapPath(double x, double y, double from_x, double from_y, double r, Path& out)
{
    const double dx = x - from_x;
    const double dy = y - from_y;
    const double length = JsHypot(dx, dy);
    if (!(length > 0)) {
        return false;
    }
    const double nx = dx / length;
    const double ny = dy / length;
    const double ext_x = x + nx * r;
    const double ext_y = y + ny * r;
    PushPathPoint(out, x - ny * r, y + nx * r, 0.0);
    PushPathPoint(out, ext_x - ny * r, ext_y + nx * r, 0.0);
    PushPathPoint(out, ext_x + ny * r, ext_y - nx * r, 0.0);
    PushPathPoint(out, x + ny * r, y - nx * r, 0.0);
    PushPathPoint(out, x - ny * r, y + nx * r, 0.0);
    return true;
}

inline void StrokeSquareCap(const Path& v, double r, std::vector<Path>& rectangles)
{
    const size_t size = v.size();
    Path start;
    if (StrokeSquareCapPath(v[0], v[1], v[3], v[4], r, start)) {
        rectangles.insert(rectangles.begin(), std::move(start));
    }
    Path end;
    if (StrokeSquareCapPath(v[size - 3], v[size - 2], v[size - 6], v[size - 5], r, end)) {
        rectangles.push_back(std::move(end));
    }
}

// 経路 1 本の線の外周を、塗りと同じ形式の経路の並びにして out の末尾へ足す
inline void StrokeOutline(const Path& v, double thickness, int joints, int caps, double pixel_scale,
                          StrokeWork& work, std::vector<Path>& out)
{
    const size_t size = v.size();
    if (size < 6) {
        return;
    }

    double start_x = v[0];
    double start_y = v[1];
    double control_x = 0.0;
    double control_y = 0.0;

    std::vector<Path> rectangles;
    for (size_t idx = 3; idx + 2 < size; idx += 3) {
        const double x = v[idx];
        const double y = v[idx + 1];
        if (v[idx + 2] != 0.0) {
            control_x = x;
            control_y = y;
            continue;
        }

        rectangles.emplace_back();
        if (v[idx - 1] != 0.0) {
            StrokeCurveRectangle(start_x, start_y, control_x, control_y, x, y,
                                 thickness, pixel_scale, rectangles.back(), true);
        } else {
            StrokeLineRectangle(start_x, start_y, x, y, thickness, rectangles.back());
        }

        if (rectangles.size() > 1) {
            if (joints == 0) {
                StrokeBevelJoin(start_x, start_y, thickness, rectangles, false, work);
            } else if (joints == 1) {
                StrokeMiterJoin(start_x, start_y, x, y, v[idx - 6], v[idx - 5],
                                thickness, rectangles, false, work);
            } else if (joints == 2) {
                StrokeRoundJoin(start_x, start_y, thickness, rectangles, false, work);
            }
        }

        start_x = x;
        start_y = y;
    }

    // 始点と終点がつながっていれば、そこにもつなぎ目を作る。つながっていなければ端を作る
    const double closed_start_x = v[0];
    const double closed_start_y = v[1];
    const double closed_end_x = v[size - 3];
    const double closed_end_y = v[size - 2];
    const bool is_closed = std::fabs(closed_start_x - closed_end_x) < kStrokeEpsilon
        && std::fabs(closed_start_y - closed_end_y) < kStrokeEpsilon
        && rectangles.size() > 1;

    if (is_closed) {
        if (joints == 0) {
            StrokeBevelJoin(closed_start_x, closed_start_y, thickness, rectangles, true, work);
        } else if (joints == 1) {
            StrokeMiterJoin(closed_start_x, closed_start_y, v[3], v[4], v[size - 6], v[size - 5],
                            thickness, rectangles, true, work);
        } else if (joints == 2) {
            StrokeRoundJoin(closed_start_x, closed_start_y, thickness, rectangles, true, work);
        }
    } else if (!rectangles.empty()) {
        if (caps == 1) {
            StrokeRoundCap(v, thickness, rectangles);
        } else if (caps == 2) {
            StrokeSquareCap(v, thickness, rectangles);
        }
    }

    for (Path& rectangle : rectangles) {
        out.push_back(std::move(rectangle));
    }
}

// いまの経路を線の外周の経路に置き換え、それを塗りのメッシュにしたときの要素数を返す。
// 続けて FillMesh を呼ぶと、線のメッシュが得られる。
// 描く線が無ければ 0 を返し、経路には触らない (JavaScript で線を作るときと同じ)。
// thickness は線の太さの半分。pixel_scale は座標 1 単位が画面上で何ピクセルになるか。
inline size_t StrokeSize(PathState& s, double thickness, int joints, int caps, double pixel_scale)
{
    StrokeWork work;
    work.webgpu_rules = true;
    std::vector<Path> outlines;
    // 名前空間を明示する (引数の型から、WebGL 版の同名の関数も候補に入ってしまうため)
    for (const Path& path : s.vertices) {
        gpu::StrokeOutline(path, thickness, joints, caps, pixel_scale, work, outlines);
    }
    if (UsesCurrent(s, true)) {
        gpu::StrokeOutline(s.current, thickness, joints, caps, pixel_scale, work, outlines);
    }
    if (outlines.empty()) {
        return 0;
    }
    s.vertices = std::move(outlines);
    s.current.clear();
    return FillSize(s);
}

} // namespace gpu

} // namespace next2d::shape

#if defined(NEXT2D_SHAPE_GCC_OPTIONS_PUSHED)
#pragma GCC pop_options
#undef NEXT2D_SHAPE_GCC_OPTIONS_PUSHED
#endif
