// ShapeAcceleratorCore: 図形の「経路の組み立て」と「塗りのメッシュの生成」の計算本体。
// V8 に依存しないので、単体テスト (tests/shape_accelerator_test.cpp) からも使える。
// 計算の順序と、32 ビットへ丸める位置は、Next2D Player (WebGL) の JavaScript と合わせてある。
// 説明は ShapeAccelerator.cpp を参照。
#pragma once

// 積和をまとめて 1 命令で計算する最適化 (FMA) を止める。丸めの回数が変わると、player の
// JavaScript での計算と結果が一致しなくなる。
#if defined(_MSC_VER)
#pragma fp_contract(off)
#else
#pragma STDC FP_CONTRACT OFF
#endif

#include <cmath>
#include <cstdint>
#include <cstring>
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

} // namespace next2d::shape
