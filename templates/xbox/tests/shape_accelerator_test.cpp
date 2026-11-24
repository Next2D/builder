// 図形の計算 (src/bindings/ShapeAcceleratorCore.h) のテスト。
// 「経路の組み立て」と「塗りのメッシュの生成」が、Next2D Player の JavaScript と 1 ビットも
// 違わない結果を返すことを確かめる (WebGL 版の規則と WebGPU 版の規則の両方)。
// 線の外周の計算 (WebGL 版と WebGPU 版) も確かめる。三角関数を使わない部分は 1 ビットも違わないこと、
// 使う部分 (丸いつなぎ目、丸い端) は形と大きさが合うことを見る。
// V8 にも OS にも依存しない。
//
// ビルド (templates/xbox で):
//   cl /nologo /utf-8 /std:c++17 /EHsc /W3 /Isrc tests\shape_accelerator_test.cpp /Fe:shape_accelerator_test.exe
//   c++ -std=c++17 -Wall -Wextra -O1 -Isrc -o shape_accelerator_test tests/shape_accelerator_test.cpp
#include "../src/bindings/ShapeAcceleratorCore.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace next2d::shape;

static int g_failed = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::printf("::error file=templates/xbox/tests/shape_accelerator_test.cpp,line=%d::%s\n", __LINE__, #cond); \
            ++g_failed; \
        } \
    } while (0)

// 経路: 同じ点への移動や直線は足さない。経路の命令でないものに当たったら、その位置を返す
static void TestFeedPath()
{
    PathState state;
    const float commands[] = {
        0, 10, 10,        // moveTo
        2, 10, 10,        // lineTo (同じ点: 足さない)
        2, 20, 10,        // lineTo
        1, 25, 15, 20, 20, // curveTo
        12,               // closePath
        9,                // 経路の命令ではない (beginPath)
        2, 99, 99
    };
    const size_t length = sizeof(commands) / sizeof(commands[0]);
    const size_t next = FeedPath(state, commands, length, 0, 1.0);
    CHECK(next == 15);
    const std::vector<double> expected = {
        10, 10, 0,  20, 10, 0,  25, 15, 1,  20, 20, 0,  10, 10, 0
    };
    CHECK(state.current == expected);
    CHECK(state.vertices.empty());

    // 引数が途中で切れている命令は処理しない
    PathState cut;
    const float truncated[] = { 0, 1, 2, 2, 3 };
    CHECK(FeedPath(cut, truncated, 5, 0, 1.0) == 3);
    CHECK(cut.current.size() == 3);

    // 途中の位置から始められる
    PathState middle;
    CHECK(FeedPath(middle, commands, length, 6, 1.0) == 15);
    CHECK(middle.current.size() == 12);   // lineTo が moveTo を兼ね、曲線が 2 点、closePath が始点を足す
}

// 移動で経路が分かれる。点の少ない経路は、塗りでは捨て、線では残す
static void TestFinalizePaths()
{
    const float commands[] = {
        0, 0, 0,  2, 10, 0,              // 2 点だけの経路
        0, 50, 50,  2, 60, 50,  2, 60, 60 // 3 点の経路
    };
    PathState fill;
    FeedPath(fill, commands, 15, 0, 1.0);
    CHECK(fill.vertices.size() == 1);
    CHECK(FillSize(fill) == kTriangleSize);
    CHECK(fill.vertices.size() == 1);
    CHECK(fill.current.empty());

    PathState stroke;
    FeedPath(stroke, commands, 15, 0, 1.0);
    FinalizePaths(stroke, 6);
    CHECK(stroke.vertices.size() == 2);

    PathState empty;
    CHECK(FillSize(empty) == 0);
    float none[1] = { 7.0f };
    CHECK(FillMesh(empty, none, 1, 0, 1, 0, 0, 1, 0, 0, 1, 1, 1, 1) == 0);
    CHECK(none[0] == 7.0f);
}

// 三角形 1 つ: 頂点の並び (位置 2、曲線用の係数 2、色 4、行列 9 のうち 6) を確かめる
static void TestTriangleMesh()
{
    PathState state;
    const float commands[] = { 0, 1, 2,  2, 3, 4,  2, 5, 6 };
    FeedPath(state, commands, 9, 0, 1.0);
    const size_t size = FillSize(state);
    CHECK(size == kTriangleSize);

    std::vector<float> mesh(2 + size, -1.0f);
    const size_t count = FillMesh(state, mesh.data(), mesh.size(), 2,
        0.5f, 0.25f, 0.125f, 2.0f, 3.0f, 4.0f, 0.1f, 0.2f, 0.3f, 0.4f);
    CHECK(count == 3);
    CHECK(mesh[0] == -1.0f && mesh[1] == -1.0f);   // offset より前には書かない
    CHECK(state.vertices.empty());

    const float points[3][2] = { { 1, 2 }, { 3, 4 }, { 5, 6 } };
    for (size_t vertex = 0; vertex < 3; ++vertex) {
        const float* v = mesh.data() + 2 + vertex * kVertexSize;
        CHECK(v[0] == points[vertex][0] && v[1] == points[vertex][1]);
        CHECK(v[2] == 0.5f && v[3] == 0.5f);
        CHECK(v[4] == 0.1f && v[5] == 0.2f && v[6] == 0.3f && v[7] == 0.4f);
        CHECK(v[8] == 0.5f && v[9] == 0.25f && v[10] == 0.0f);
        CHECK(v[11] == 0.125f && v[12] == 2.0f && v[13] == 0.0f);
        CHECK(v[14] == 3.0f && v[15] == 4.0f && v[16] == 0.0f);
    }

    // 書き込み先が足りないときは、領域の外へ書かない
    PathState small;
    FeedPath(small, commands, 9, 0, 1.0);
    std::vector<float> narrow(kTriangleSize - 1, -1.0f);
    CHECK(FillMesh(small, narrow.data(), narrow.size(), 0, 1, 0, 0, 1, 0, 0, 1, 1, 1, 1) == 0);
    CHECK(narrow[0] == -1.0f);
}

// 2 次ベジェ曲線: 曲線の三角形 (係数 0,0 / 0.5,0 / 1,1) と、扇形の三角形ができる
static void TestCurveMesh()
{
    PathState state;
    const float commands[] = { 0, 0, 0,  1, 10, 0, 10, 10,  2, 0, 10 };
    FeedPath(state, commands, 11, 0, 1.0);
    const size_t size = FillSize(state);
    CHECK(size == kTriangleSize * 2);
    std::vector<float> mesh(size, -1.0f);
    CHECK(FillMesh(state, mesh.data(), mesh.size(), 0, 1, 0, 0, 1, 0, 0, 1, 1, 1, 1) == 6);

    const float* curve = mesh.data();
    CHECK(curve[0] == 0 && curve[1] == 0 && curve[2] == 0.0f && curve[3] == 0.0f);
    CHECK(curve[17] == 10 && curve[18] == 0 && curve[19] == 0.5f && curve[20] == 0.0f);
    CHECK(curve[34] == 10 && curve[35] == 10 && curve[36] == 1.0f && curve[37] == 1.0f);

    const float* fan = mesh.data() + kTriangleSize;
    CHECK(fan[0] == 0 && fan[1] == 0 && fan[2] == 0.5f);
    CHECK(fan[17] == 10 && fan[18] == 10 && fan[19] == 0.5f);
    CHECK(fan[34] == 0 && fan[35] == 10 && fan[36] == 0.5f);
}

// 3 次ベジェ曲線: 画面上で小さければ 2 次ベジェ曲線 1 本、大きければ複数本に分ける
static void TestCubicSubdivision()
{
    const float commands[] = { 0, 0, 0,  3, 0, 100, 100, 100, 100, 0 };
    PathState tiny;
    FeedPath(tiny, commands, 10, 0, 1e-6);
    CHECK(tiny.current.size() == 9);    // 始点 + (制御点, 終点)
    CHECK(tiny.current[3] == 50.0 && tiny.current[4] == 150.0);

    PathState large;
    FeedPath(large, commands, 10, 0, 100.0);
    CHECK(large.current.size() > 9);
    CHECK((large.current.size() - 3) % 6 == 0);
    CHECK(large.current.size() <= 3 + static_cast<size_t>(kMaxSubdivisions) * 6);
    CHECK(large.current[large.current.size() - 3] == 100.0);
    CHECK(large.current[large.current.size() - 2] == 0.0);
}

// ---- JavaScript の結果との突き合わせ --------------------------------------------
// 入力は下の疑似乱数から作る (JavaScript 側も同じ手順で作った)。期待値は、Next2D Player の
// 経路と塗りのメッシュの実装 (PathCommand* と MeshFillGenerate*) に同じ入力を通した結果。
// 中身は長いので、要素数とハッシュ (FNV-1a) で比べる。
struct Reference {
    uint32_t command_length;
    uint32_t command_hash;
    uint32_t mesh_size;
    uint32_t index_count;
    uint32_t mesh_hash;
};

static const Reference kReferences[] = {
    { 171u, 0xed61889cu, 8721u, 513u, 0x1c17682eu },
    { 59u, 0xb136b142u, 765u, 45u, 0x60cc6943u },
    { 138u, 0x25754becu, 17442u, 1026u, 0x80aa1286u },
    { 185u, 0xbc209454u, 16065u, 945u, 0x51e75245u },
    { 67u, 0x610a3f50u, 7446u, 438u, 0x0789d38au },
    { 23u, 0x39b2897du, 1020u, 60u, 0xa7d111beu },
    { 44u, 0x4f73d724u, 2346u, 138u, 0xfb9056eau },
    { 194u, 0x75b64132u, 9027u, 531u, 0x81779e38u },
    { 116u, 0xcb744178u, 8313u, 489u, 0x6f090979u },
    { 130u, 0x7eaca6d6u, 14535u, 855u, 0xabb3e90du },
    { 32u, 0x52583557u, 2550u, 150u, 0x77ab2dc9u },
    { 77u, 0x7375494fu, 8262u, 486u, 0xce78c4c6u },
    { 104u, 0x224234b5u, 3825u, 225u, 0x7b5644bau },
    { 84u, 0x3ce19feau, 3672u, 216u, 0x30e50ca3u },
    { 33u, 0xecc2091du, 3570u, 210u, 0x634d0fdeu },
    { 109u, 0x9f09dc1bu, 13056u, 768u, 0x02e69dc0u },
    { 102u, 0x0d42fb46u, 5202u, 306u, 0x2cf18db9u },
    { 133u, 0x38cfd4d9u, 11832u, 696u, 0x1dffea9bu },
    { 43u, 0x7f9f063bu, 918u, 54u, 0xb18ed459u },
    { 111u, 0x0a0e0a6cu, 3723u, 219u, 0x45bd87dbu },
    { 30u, 0x4074b1b9u, 2397u, 141u, 0x44470c33u },
    { 72u, 0x8e350565u, 4641u, 273u, 0x98c818afu },
    { 77u, 0x4aff4539u, 4029u, 237u, 0x31dc34e6u },
    { 124u, 0x03c5167du, 20604u, 1212u, 0x2620e4cau },
    { 161u, 0x84e31b13u, 9231u, 543u, 0xd9212623u },
    { 66u, 0x825f114bu, 1224u, 72u, 0x19e39de7u },
    { 120u, 0x1ca4860cu, 13107u, 771u, 0x82bc876du },
    { 115u, 0x84abbd7au, 8160u, 480u, 0xd2b75e3cu },
    { 128u, 0x23e398dau, 6681u, 393u, 0x9159b86au },
    { 62u, 0xbcf40c99u, 5559u, 327u, 0x9157fd8bu },
    { 17u, 0x31655259u, 204u, 12u, 0xd1b9e069u },
    { 55u, 0xb456cd95u, 3672u, 216u, 0xa6077cc7u },
    { 50u, 0x925a8731u, 4692u, 276u, 0x7e84ada3u },
    { 32u, 0x666f423fu, 1734u, 102u, 0x389b639eu },
    { 94u, 0x775274d5u, 8670u, 510u, 0x1899f9e0u },
    { 89u, 0x3b1689e6u, 17544u, 1032u, 0xe2be6e54u },
    { 122u, 0x3afe5521u, 3825u, 225u, 0xa524e078u },
    { 171u, 0x1f577b11u, 4743u, 279u, 0xf68a15c7u },
    { 59u, 0xa4732771u, 6018u, 354u, 0xb09aab58u },
    { 150u, 0xe4928446u, 15657u, 921u, 0x707020f5u },
    { 43u, 0x4cae0ba9u, 6273u, 369u, 0x99cfdb64u },
    { 91u, 0xab9537a8u, 4233u, 249u, 0x743bc583u },
    { 66u, 0x355aa00fu, 2040u, 120u, 0xf2cdfb51u },
    { 23u, 0xcfee4de2u, 510u, 30u, 0xed0a5556u },
    { 121u, 0x0cbfdb9du, 8619u, 507u, 0xeba9c697u },
    { 79u, 0xa47815d8u, 7701u, 453u, 0x7d624aa9u },
    { 85u, 0x994b54f6u, 8925u, 525u, 0xd22a399bu },
    { 179u, 0x5dcd5634u, 29427u, 1731u, 0x37291631u },
};

static uint32_t FnvBytes(const void* data, size_t size)
{
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    uint32_t hash = 0x811c9dc5u;
    for (size_t i = 0; i < size; ++i) {
        hash = (hash ^ bytes[i]) * 0x01000193u;
    }
    return hash;
}

static uint32_t Fnv(const float* data, size_t length)
{
    return FnvBytes(data, length * sizeof(float));
}

struct Random {
    uint32_t x;
    uint32_t Next()
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return x;
    }
};

static float Coord(Random& random, int mode)
{
    if (mode == 0) return static_cast<float>((static_cast<double>(random.Next() % 8001u) - 4000) / 16);
    if (mode == 1) return static_cast<float>(static_cast<double>(random.Next() % 7u) - 3);
    return static_cast<float>((static_cast<double>(random.Next() % 64001u) - 32000) / 32);
}

// i 番目の入力 (描画命令の並び)
static std::vector<float> MakeCommands(size_t i)
{
    Random random { (0x9e3779b9u ^ (static_cast<uint32_t>(i) * 0x85ebca6bu)) | 1u };
    const int mode = static_cast<int>(i % 3);

    // C++ は引数を評価する順序を決めていないので、乱数は 1 つずつ取り出して並べる
    std::vector<float> commands;
    auto coords = [&](int count) {
        for (int n = 0; n < count; ++n) commands.push_back(Coord(random, mode));
    };
    commands.push_back(0);
    coords(2);
    const uint32_t count = 4 + random.Next() % 40u;
    for (uint32_t n = 0; n < count; ++n) {
        const uint32_t op = random.Next() % 10u;
        if (op == 0) { commands.push_back(0); coords(2); }
        else if (op <= 3) { commands.push_back(2); coords(2); }
        else if (op <= 5) { commands.push_back(1); coords(4); }
        else if (op <= 7) { commands.push_back(3); coords(6); }
        else if (op == 8) {
            commands.push_back(4);
            coords(2);
            commands.push_back(static_cast<float>((random.Next() % 1600u + 1) / 16.0));
        }
        else { commands.push_back(12); }
    }
    return commands;
}

// i 番目の描画行列 (JavaScript では 32 ビットの配列に入っている)
static void MakeMatrix(size_t i, float* m)
{
    const double scales[] = { 0.03125, 0.25, 1, 2.5, 8, 40 };
    const double s = scales[i % 6];
    const float matrix[9] = { static_cast<float>(s), 0, 0, 0, static_cast<float>(s * 0.75), 0, 100.5f, -20.25f, 1 };
    std::memcpy(m, matrix, sizeof(matrix));
    if (i % 4 == 3) {
        m[0] = static_cast<float>(s * 0.8);
        m[1] = static_cast<float>(s * 0.6);
        m[3] = static_cast<float>(-s * 0.6);
        m[4] = static_cast<float>(s * 0.8);
    }
}

static void TestAgainstJavaScript()
{
    const size_t cases = sizeof(kReferences) / sizeof(kReferences[0]);
    for (size_t i = 0; i < cases; ++i) {
        const Reference& reference = kReferences[i];
        const std::vector<float> commands = MakeCommands(i);
        CHECK(commands.size() == reference.command_length);
        CHECK(Fnv(commands.data(), commands.size()) == reference.command_hash);

        float m[9];
        MakeMatrix(i, m);
        const float color[4] = {
            static_cast<float>(static_cast<double>(i % 5) / 4), 0.5f, 1.0f,
            static_cast<float>(static_cast<double>(i % 3 + 1) / 3)
        };
        const double width = 1920, height = 1080;
        const double scale_x = static_cast<double>(m[0]) * m[0] + static_cast<double>(m[1]) * m[1];
        const double scale_y = static_cast<double>(m[3]) * m[3] + static_cast<double>(m[4]) * m[4];

        PathState state;
        CHECK(FeedPath(state, commands.data(), commands.size(), 0, scale_x > scale_y ? scale_x : scale_y) == commands.size());
        const size_t size = FillSize(state);
        CHECK(size == reference.mesh_size);
        if (size != reference.mesh_size) {
            std::printf("  case %zu: mesh size %zu, expected %u\n", i, size, reference.mesh_size);
            continue;
        }
        std::vector<float> mesh(size);
        const size_t index_count = FillMesh(state, mesh.data(), mesh.size(), 0,
            static_cast<float>(m[0] / width), static_cast<float>(m[1] / height),
            static_cast<float>(m[3] / width), static_cast<float>(m[4] / height),
            static_cast<float>(m[6] / width), static_cast<float>(m[7] / height),
            color[0], color[1], color[2], color[3]);
        CHECK(index_count == reference.index_count);
        const uint32_t hash = Fnv(mesh.data(), mesh.size());
        CHECK(hash == reference.mesh_hash);
        if (hash != reference.mesh_hash) {
            std::printf("  case %zu: mesh hash %08x, expected %08x\n", i, hash, reference.mesh_hash);
            std::printf("::error::webgl case %zu: mesh hash %08x, expected %08x\n", i, hash, reference.mesh_hash);
        }
    }
}

// ---- WebGPU 版の規則 ---------------------------------------------------------------

// 経路: moveTo は常に新しい経路を始め、塗りは経路を消さない
static void TestGpuPath()
{
    gpu::PathState state;
    CHECK(!gpu::HasFillPath(state));
    CHECK(gpu::FillSize(state) == 0);

    const float commands[] = {
        0, 0, 0,  2, 10, 0,               // 2 点だけの経路 (塗りでは三角形にならない)
        0, 50, 50,  2, 50, 50,  2, 60, 50,  2, 60, 60,  12,   // 同じ点への直線は足さない。閉じると始点が加わる
        9
    };
    const size_t length = sizeof(commands) / sizeof(commands[0]);
    CHECK(gpu::FeedPath(state, commands, length, 0, 1.0) == length - 1);
    CHECK(state.vertices.size() == 1);
    const std::vector<double> expected = { 50, 50, 0,  60, 50, 0,  60, 60, 0,  50, 50, 0 };
    CHECK(state.current == expected);
    CHECK(gpu::HasFillPath(state));
    CHECK(gpu::UsesCurrent(state, true));

    // 4 点の経路から三角形 2 つ。頂点は 4 要素
    CHECK(gpu::FillSize(state) == 2 * gpu::kTriangleSize);
    std::vector<float> mesh(2 * gpu::kTriangleSize, -1.0f);
    CHECK(gpu::FillMesh(state, mesh.data(), mesh.size()) == 6);
    const float first[] = { 50, 50, 0.5f, 0.5f,  60, 50, 0.5f, 0.5f,  60, 60, 0.5f, 0.5f };
    CHECK(std::memcmp(mesh.data(), first, sizeof(first)) == 0);
    CHECK(mesh[12] == 50 && mesh[16] == 60 && mesh[17] == 60 && mesh[20] == 50 && mesh[21] == 50);

    // 経路は残っている
    CHECK(gpu::FillSize(state) == 2 * gpu::kTriangleSize);

    // 書き込み先が足りないときは、領域の外へ書かない
    std::vector<float> narrow(gpu::kTriangleSize + 1, -1.0f);
    CHECK(gpu::FillMesh(state, narrow.data(), narrow.size()) == 3);
    CHECK(narrow[gpu::kTriangleSize] == -1.0f);

    gpu::Reset(state);
    CHECK(!gpu::HasFillPath(state));
    CHECK(state.current.empty() && state.vertices.empty());

    // 2 点だけの経路が確定済みなら「経路はあるが三角形は無い」
    gpu::PathState thin;
    const float two[] = { 0, 0, 0,  2, 10, 0,  0, 5, 5 };
    gpu::FeedPath(thin, two, 9, 0, 1.0);
    CHECK(gpu::HasFillPath(thin));
    CHECK(gpu::FillSize(thin) == 0);
}

// 曲線の三角形 (係数 0,0 / 0.5,0 / 1,1) と、制御点を飛ばした扇形の三角形
static void TestGpuCurveMesh()
{
    gpu::PathState state;
    const float commands[] = { 0, 0, 0,  2, 10, 0,  1, 20, 10, 10, 20,  2, 0, 20 };
    gpu::FeedPath(state, commands, 14, 0, 1.0);
    CHECK(gpu::FillSize(state) == 3 * gpu::kTriangleSize);
    std::vector<float> mesh(3 * gpu::kTriangleSize);
    CHECK(gpu::FillMesh(state, mesh.data(), mesh.size()) == 9);
    const float expected[] = {
        0, 0, 0.5f, 0.5f,   10, 0, 0.5f, 0.5f,   10, 20, 0.5f, 0.5f,   // 扇形: 次の点が制御点なので、その先を使う
        10, 0, 0, 0,        20, 10, 0.5f, 0,     10, 20, 1, 1,          // 曲線
        0, 0, 0.5f, 0.5f,   10, 20, 0.5f, 0.5f,  0, 20, 0.5f, 0.5f     // 扇形
    };
    CHECK(std::memcmp(mesh.data(), expected, sizeof(expected)) == 0);
}

// 円弧は画面上の大きさによらず分割し、終点を経路の始点に合わせる
static void TestGpuArc()
{
    gpu::PathState state;
    const float commands[] = { 0, 110, 50,  4, 50, 50, 60 };
    gpu::FeedPath(state, commands, 7, 0, 1.0);
    CHECK(state.current.size() > 3 + 4 * 6);
    CHECK((state.current.size() - 3) % 6 == 0);
    CHECK(state.current[state.current.size() - 3] == 110.0);
    CHECK(state.current[state.current.size() - 2] == 50.0);
    CHECK(state.current_x == 110.0 && state.current_y == 50.0);

    // 拡大率を変えても、円弧の分割は変わらない
    gpu::PathState scaled;
    gpu::FeedPath(scaled, commands, 7, 0, 100.0);
    CHECK(scaled.current == state.current);
}

// 期待値は、WebGPU 版の player の PathCommand と MeshFillGenerate* に同じ入力を通した結果。
// 線に使う経路 ([本数, 要素数, x, y, 制御点か, ...] を 64 ビットで並べたもの) も比べる。
struct GpuReference {
    int mesh_size;          // 経路が 1 本も無ければ -1
    uint32_t index_count;
    uint32_t mesh_hash;
    uint32_t stroke_length;
    uint32_t stroke_hash;
};

static const GpuReference kGpuReferences[] = {
    { 7776, 1944u, 0x80d1252eu, 1966u, 0x105cdf05u },
    { 180, 45u, 0x62238325u, 60u, 0xbf3eb1c7u },
    { 7656, 1914u, 0x967e5bdcu, 1929u, 0x4358450bu },
    { 4968, 1242u, 0x91f8cbefu, 1271u, 0xc63b6823u },
    { 2292, 573u, 0xb92f0e1eu, 588u, 0xe9f3bcc0u },
    { 240, 60u, 0xce63a512u, 68u, 0x656096dbu },
    { 1776, 444u, 0xf9055052u, 459u, 0x6f532fa6u },
    { 5760, 1440u, 0xee7f0820u, 1469u, 0x0d7a0618u },
    { 2556, 639u, 0x2aea01d3u, 668u, 0x6028c11du },
    { 5184, 1296u, 0x931d6702u, 1325u, 0x685c0c04u },
    { 600, 150u, 0x1bd2a529u, 158u, 0xebc506c7u },
    { 2916, 729u, 0xa1ae4c02u, 744u, 0x62d97006u },
    { 2412, 603u, 0xbbfa9d4du, 618u, 0x6cc5ead7u },
    { 2508, 627u, 0x64b5c757u, 642u, 0x5d11b44bu },
    { 1272, 318u, 0xd26b0be6u, 326u, 0xf4a57183u },
    { 4524, 1131u, 0xb6b2e67eu, 1153u, 0xa1b3520au },
    { 1464, 366u, 0x757b6bc4u, 395u, 0xbf02094fu },
    { 3336, 834u, 0xb26b84ffu, 870u, 0x24675790u },
    { 228, 57u, 0x01ca5c15u, 72u, 0xabf18453u },
    { 2136, 534u, 0x6f7a1d2au, 556u, 0x4e7488a4u },
    { 564, 141u, 0x43a25418u, 156u, 0x725f3f1bu },
    { 1128, 282u, 0xa32445adu, 304u, 0xff59047du },
    { 1212, 303u, 0xa9030a2bu, 332u, 0xe8741a73u },
    { 6084, 1521u, 0x23cc09feu, 1550u, 0x6dc45debu },
    { 7116, 1779u, 0x557ebacfu, 1808u, 0xbfdd0576u },
    { 324, 81u, 0xc5191b5eu, 96u, 0xca709cd6u },
    { 4860, 1215u, 0xed685be7u, 1230u, 0x57a050c5u },
    { 2328, 582u, 0x5a18981au, 604u, 0xabb7e6aau },
    { 1836, 459u, 0x631c2289u, 488u, 0xebfde078u },
    { 948, 237u, 0x6f74e435u, 266u, 0xcbeaace8u },
    { 48, 12u, 0x88097f39u, 20u, 0x03c69785u },
    { 2928, 732u, 0xe85838adu, 754u, 0xa3d54661u },
    { 2064, 516u, 0xa59e0492u, 531u, 0x1f6f1291u },
    { 408, 102u, 0xd306a3fau, 117u, 0x14bd85ccu },
    { 2016, 504u, 0xaaa97fedu, 519u, 0x5326a08au },
    { 4992, 1248u, 0x9faca888u, 1270u, 0xcc3bb783u },
    { 1932, 483u, 0x1f23c10eu, 505u, 0x984247c0u },
    { 1980, 495u, 0x268df79bu, 531u, 0xcc75fab1u },
    { 2568, 642u, 0x31763bd9u, 650u, 0xafed8984u },
    { 5460, 1365u, 0xcefd172eu, 1387u, 0x5b0b434eu },
    { 1740, 435u, 0x10a3fcf3u, 450u, 0x17110b28u },
    { 684, 171u, 0xbfc8818fu, 207u, 0x5cf77008u },
    { 1200, 300u, 0x3ccf6cf9u, 322u, 0x0afa725au },
    { 120, 30u, 0xbbf83430u, 38u, 0xf755519fu },
    { 2496, 624u, 0xf4163c2bu, 632u, 0x78ebbdbdu },
    { 3252, 813u, 0x05fab496u, 842u, 0x58a55ba7u },
    { 2460, 615u, 0x3e77903bu, 623u, 0xa1a17c17u },
    { 8292, 2073u, 0x0453c1ddu, 2123u, 0x8873fc45u },
};

static void TestGpuAgainstJavaScript()
{
    const size_t cases = sizeof(kGpuReferences) / sizeof(kGpuReferences[0]);
    for (size_t i = 0; i < cases; ++i) {
        const GpuReference& reference = kGpuReferences[i];
        const std::vector<float> commands = MakeCommands(i);
        float m[9];
        MakeMatrix(i, m);
        const double scale_x = static_cast<double>(m[0]) * m[0] + static_cast<double>(m[1]) * m[1];
        const double scale_y = static_cast<double>(m[3]) * m[3] + static_cast<double>(m[4]) * m[4];

        gpu::PathState state;
        CHECK(gpu::FeedPath(state, commands.data(), commands.size(), 0,
            std::sqrt(scale_x > scale_y ? scale_x : scale_y)) == commands.size());

        const int size = gpu::HasFillPath(state) ? static_cast<int>(gpu::FillSize(state)) : -1;
        CHECK(size == reference.mesh_size);
        if (size != reference.mesh_size) {
            std::printf("  webgpu case %zu: mesh size %d, expected %d\n", i, size, reference.mesh_size);
        } else if (size >= 0) {
            std::vector<float> mesh(static_cast<size_t>(size));
            CHECK(gpu::FillMesh(state, mesh.data(), mesh.size()) == reference.index_count);
            const uint32_t hash = Fnv(mesh.data(), mesh.size());
            CHECK(hash == reference.mesh_hash);
            if (hash != reference.mesh_hash) {
                std::printf("  webgpu case %zu: mesh hash %08x, expected %08x\n", i, hash, reference.mesh_hash);
            }
        }

        std::vector<double> stroke;
        const bool uses_current = gpu::UsesCurrent(state, true);
        stroke.push_back(static_cast<double>(state.vertices.size() + (uses_current ? 1 : 0)));
        for (const std::vector<double>& path : state.vertices) {
            stroke.push_back(static_cast<double>(path.size()));
            stroke.insert(stroke.end(), path.begin(), path.end());
        }
        if (uses_current) {
            stroke.push_back(static_cast<double>(state.current.size()));
            stroke.insert(stroke.end(), state.current.begin(), state.current.end());
        }
        CHECK(stroke.size() == reference.stroke_length);
        const uint32_t stroke_hash = FnvBytes(stroke.data(), stroke.size() * sizeof(double));
        CHECK(stroke_hash == reference.stroke_hash);
        if (stroke_hash != reference.stroke_hash) {
            std::printf("  webgpu case %zu: path hash %08x, expected %08x\n", i, stroke_hash, reference.stroke_hash);
            std::printf("::error::webgpu case %zu: path hash %08x, expected %08x\n", i, stroke_hash, reference.stroke_hash);
        }
    }
}

// ---- 線の外周 ----------------------------------------------------------------------

// 経路 (x, y, 制御点なら 1 の並び) を 1 本持つ状態を作る
static PathState MakePath(const std::vector<double>& points)
{
    PathState state;
    state.current = points;
    return state;
}

static bool Near(const std::vector<double>& actual, const std::vector<double>& expected)
{
    if (actual.size() != expected.size()) {
        return false;
    }
    for (size_t i = 0; i < actual.size(); ++i) {
        if (!(std::fabs(actual[i] - expected[i]) <= 1e-9)) {
            return false;
        }
    }
    return true;
}

// 直線 1 本: 太さの分だけ両側へずらした四角形になる。端の指定が無ければ何も足さない
static void TestStrokeLine()
{
    PathState state = MakePath({ 0, 0, 0,  10, 0, 0 });
    CHECK(StrokeSize(state, 2, 2, 0, 1.0) == kTriangleSize * 3);
    CHECK(state.current.empty());
    CHECK(state.vertices.size() == 1);
    const std::vector<double> rectangle = {
        0, 2, 0,  10, 2, 0,  10, -2, 0,  0, -2, 0,  0, 2, 0
    };
    CHECK(state.vertices[0] == rectangle);

    // 続けて塗りのメッシュにできる (経路は空になる)
    std::vector<float> mesh(kTriangleSize * 3);
    CHECK(FillMesh(state, mesh.data(), mesh.size(), 0, 1, 0, 0, 1, 0, 0, 1, 1, 1, 1) == 9);
    CHECK(mesh[0] == 0.0f && mesh[1] == 2.0f);
    CHECK(mesh[17] == 10.0f && mesh[18] == 2.0f);
    CHECK(mesh[34] == 10.0f && mesh[35] == -2.0f);
    CHECK(state.vertices.empty());

    // 点が 1 つだけの経路は線にならない
    PathState single = MakePath({ 5, 5, 0 });
    CHECK(StrokeSize(single, 2, 2, 0, 1.0) == 0);
    CHECK(single.vertices.empty());

    PathState empty;
    CHECK(StrokeSize(empty, 2, 2, 1, 1.0) == 0);
}

// 直角に曲がる線のつなぎ目 (外側の角を埋める)
static void TestStrokeJoins()
{
    const std::vector<double> corner = { 0, 0, 0,  10, 0, 0,  10, 10, 0 };
    const std::vector<double> first  = { 0, 1, 0,  10, 1, 0,  10, -1, 0,  0, -1, 0,  0, 1, 0 };
    const std::vector<double> second = { 9, 0, 0,  9, 10, 0,  11, 10, 0,  11, 0, 0,  9, 0, 0 };

    // bevel: 角の点と、2 つの四角形の外側の点を結ぶ
    PathState bevel = MakePath(corner);
    StrokeSize(bevel, 1, 0, 0, 1.0);
    CHECK(bevel.vertices.size() == 3);
    if (bevel.vertices.size() == 3) {
        CHECK(bevel.vertices[0] == first);
        CHECK(bevel.vertices[1] == std::vector<double>({ 10, 0, 0,  11, 0, 0,  10, -1, 0,  10, 0, 0 }));
        CHECK(bevel.vertices[2] == second);
    }

    // miter: 外側の 2 点を線の方向へ延ばした交点 (11, -1) まで埋める
    PathState miter = MakePath(corner);
    StrokeSize(miter, 1, 1, 0, 1.0);
    CHECK(miter.vertices.size() == 3);
    if (miter.vertices.size() == 3) {
        CHECK(miter.vertices[1] == std::vector<double>({
            10, 0, 0,  11, 0, 0,  11, -1, 0,  10, 0, 0,  10, -1, 0,  11, -1, 0
        }));
    }

    // round: 角の点を中心に、外側の 2 点の間を 8 等分した扇形
    PathState round = MakePath(corner);
    StrokeSize(round, 1, 2, 0, 1.0);
    CHECK(round.vertices.size() == 3);
    if (round.vertices.size() == 3) {
        const std::vector<double>& join = round.vertices[1];
        CHECK(join.size() == 30);
        if (join.size() == 30) {
            CHECK(join[0] == 10 && join[1] == 0);
            CHECK(std::fabs(join[3] - 11) <= 1e-9 && std::fabs(join[4]) <= 1e-9);
            CHECK(std::fabs(join[27] - 10) <= 1e-9 && std::fabs(join[28] + 1) <= 1e-9);
            // 途中の点も、角から太さの分だけ離れている
            for (size_t i = 3; i < 30; i += 3) {
                CHECK(std::fabs(std::hypot(join[i] - 10, join[i + 1]) - 1) <= 1e-9);
            }
        }
    }

    // まっすぐ続く線には、つなぎ目を足さない
    PathState straight = MakePath({ 0, 0, 0,  10, 0, 0,  20, 0, 0 });
    StrokeSize(straight, 1, 1, 0, 1.0);
    CHECK(straight.vertices.size() == 2);

    // 閉じた経路は、始点にもつなぎ目ができる (端は足さない)
    PathState closed = MakePath({ 0, 0, 0,  10, 0, 0,  10, 10, 0,  0, 10, 0,  0, 0, 0 });
    StrokeSize(closed, 1, 0, 2, 1.0);
    CHECK(closed.vertices.size() == 8);   // 四角形 4 + つなぎ目 4
}

// 線の端
static void TestStrokeCaps()
{
    // square: 端の点から線の向きへ、太さの分だけ張り出す
    PathState square = MakePath({ 0, 0, 0,  10, 0, 0 });
    StrokeSize(square, 2, 2, 2, 1.0);
    CHECK(square.vertices.size() == 3);
    if (square.vertices.size() == 3) {
        CHECK(Near(square.vertices[0], {
            -4, -2, 0,  -4, 2, 0,  0, 2, 0,  -4, -2, 0,  0, 2, 0,  0, -2, 0
        }));
        CHECK(Near(square.vertices[2], {
            14, 2, 0,  14, -2, 0,  10, -2, 0,  14, 2, 0,  10, -2, 0,  10, 2, 0
        }));
    }

    // round: 端の点を中心にした半円 (17 点)
    PathState round = MakePath({ 0, 0, 0,  10, 0, 0 });
    StrokeSize(round, 2, 2, 1, 1.0);
    CHECK(round.vertices.size() == 3);
    if (round.vertices.size() == 3) {
        const std::vector<double>& head = round.vertices[0];
        const std::vector<double>& tail = round.vertices[2];
        CHECK(head.size() == 51 && tail.size() == 51);
        if (head.size() == 51 && tail.size() == 51) {
            for (size_t i = 0; i < 51; i += 3) {
                CHECK(std::fabs(std::hypot(head[i], head[i + 1]) - 2) <= 1e-9);
                CHECK(head[i] <= 1e-9);          // 線の外側 (x <= 0) へ張り出す
                CHECK(std::fabs(std::hypot(tail[i] - 10, tail[i + 1]) - 2) <= 1e-9);
                CHECK(tail[i] >= 10 - 1e-9);
            }
        }
    }
}

// 曲線: 画面上の大きさに応じた数に分け、左側を順に、右側を逆順にたどる輪郭になる
static void TestStrokeCurve()
{
    // 拡大率が分からないとき (0) は 5 回 (32 区間) に分ける
    PathState state = MakePath({ 0, 0, 0,  10, 0, 1,  10, 10, 0 });
    StrokeSize(state, 1, 2, 0, 0.0);
    CHECK(state.vertices.size() == 1);
    if (state.vertices.size() == 1) {
        const std::vector<double>& outline = state.vertices[0];
        CHECK(outline.size() == 6 + 12 * 32);
        // 始点は進む向き (+x) の左へ、終点は進む向き (+y) の左へずれる
        CHECK(outline[0] == 0 && outline[1] == 1);
        CHECK(outline[3 * 64] == 9 && outline[3 * 64 + 1] == 10);
        // 右側は終点から始点へ戻る
        CHECK(outline[3 * 65] == 11 && outline[3 * 65 + 1] == 10);
        CHECK(outline[outline.size() - 3] == 0 && outline[outline.size() - 2] == -1);
    }

    // 小さく表示するときは分けない
    PathState small = MakePath({ 0, 0, 0,  10, 0, 1,  10, 10, 0 });
    StrokeSize(small, 1, 2, 0, 0.001);
    CHECK(small.vertices.size() == 1);
    if (small.vertices.size() == 1) {
        CHECK(small.vertices[0].size() == 6 + 12);
    }
}

// Math.hypot と同じ値を返す
static void TestJsHypot()
{
    CHECK(JsHypot(3, 4) == 5);
    CHECK(JsHypot(0, 0) == 0);
    CHECK(JsHypot(-5, 0) == 5);
    CHECK(JsHypot(std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN())
        == std::numeric_limits<double>::infinity());
    const double nan = JsHypot(1, std::numeric_limits<double>::quiet_NaN());
    CHECK(nan != nan);
}

// 線のメッシュの基準値: 入力の長さ、メッシュの要素数、頂点数、メッシュのハッシュ値。
// 入力は TestAgainstJavaScript と同じ並び (MakeCommands / MakeMatrix)。
// Next2D Player (WebGL) の JavaScript (canvas の内外判定は、実行環境の canvas と同じ計算) で求めた値。
// 三角関数を使う場合 (丸いつなぎ目、丸い端) は、処理系で最後の桁が変わりうるので、ハッシュ値は
// 持たない (0)。要素数と頂点数だけを比べる。
struct StrokeReference {
    uint32_t command_length;
    uint32_t mesh_size;
    uint32_t index_count;
    uint32_t mesh_hash;
};

static const StrokeReference kStrokeReferences[] = {
    { 171u, 121278u, 7134u, 0xa24ead79u },
    { 59u, 23511u, 1383u, 0xf73465f4u },
    { 138u, 449208u, 26424u, 0x00000000u },
    { 185u, 455175u, 26775u, 0x00000000u },
    { 67u, 138924u, 8172u, 0x00000000u },
    { 23u, 56559u, 3327u, 0x00000000u },
    { 44u, 17136u, 1008u, 0x5f661573u },
    { 194u, 174369u, 10257u, 0x32631317u },
    { 116u, 249033u, 14649u, 0x00000000u },
    { 130u, 326553u, 19209u, 0x9f4508afu },
    { 32u, 54417u, 3201u, 0xa6dfe8aau },
    { 77u, 441150u, 25950u, 0x00000000u },
    { 104u, 77877u, 4581u, 0x00000000u },
    { 84u, 60384u, 3552u, 0x00000000u },
    { 33u, 109701u, 6453u, 0x00000000u },
    { 109u, 303297u, 17841u, 0x05b52770u },
    { 102u, 123522u, 7266u, 0x1e7f1dd2u },
    { 133u, 569619u, 33507u, 0x00000000u },
    { 43u, 24276u, 1428u, 0xf4b6c8d4u },
    { 111u, 72216u, 4248u, 0x87aceaf3u },
    { 30u, 95217u, 5601u, 0x00000000u },
    { 72u, 92004u, 5412u, 0x00000000u },
    { 77u, 93075u, 5475u, 0x00000000u },
    { 124u, 981036u, 57708u, 0x00000000u },
    { 161u, 105825u, 6225u, 0x062d01b9u },
    { 66u, 43962u, 2586u, 0x45d82599u },
    { 120u, 363885u, 21405u, 0x00000000u },
    { 115u, 263517u, 15501u, 0x6d4c0314u },
    { 128u, 128979u, 7587u, 0x290948e2u },
    { 62u, 193749u, 11397u, 0x00000000u },
    { 17u, 8823u, 519u, 0x00000000u },
    { 55u, 55845u, 3285u, 0x00000000u },
    { 50u, 158406u, 9318u, 0x00000000u },
    { 32u, 49980u, 2940u, 0x979434d8u },
    { 94u, 164220u, 9660u, 0x15f287d0u },
    { 89u, 829107u, 48771u, 0x00000000u },
    { 122u, 75990u, 4470u, 0xfabf8176u },
    { 171u, 111180u, 6540u, 0x1adc8d92u },
    { 59u, 140097u, 8241u, 0x00000000u },
    { 150u, 399738u, 23514u, 0x00000000u },
    { 43u, 118473u, 6969u, 0x00000000u },
    { 91u, 235008u, 13824u, 0x00000000u },
    { 66u, 34833u, 2049u, 0x2b63d9deu },
    { 23u, 27489u, 1617u, 0xe654ed7bu },
    { 121u, 326910u, 19230u, 0x00000000u },
    { 79u, 156519u, 9207u, 0x51d235a0u },
    { 85u, 174828u, 10284u, 0xa4cd345bu },
    { 179u, 1426062u, 83886u, 0x00000000u },
};

// NaN は 1 つの表現にそろえてから数える (符号などが処理系で違う)
static uint32_t FnvCanonical(const float* data, size_t length)
{
    std::vector<uint32_t> words(length);
    for (size_t i = 0; i < length; ++i) {
        if (data[i] != data[i]) {
            words[i] = 0x7fc00000u;
        } else {
            std::memcpy(&words[i], &data[i], sizeof(uint32_t));
        }
    }
    return FnvBytes(words.data(), length * sizeof(uint32_t));
}

static void TestStrokeAgainstJavaScript()
{
    const double thicknesses[] = { 0.5, 1, 2.5, 10 };
    const size_t cases = sizeof(kStrokeReferences) / sizeof(kStrokeReferences[0]);
    for (size_t i = 0; i < cases; ++i) {
        const StrokeReference& reference = kStrokeReferences[i];
        const std::vector<float> commands = MakeCommands(i);
        CHECK(commands.size() == reference.command_length);

        float m[9];
        MakeMatrix(i, m);
        const float color[4] = {
            static_cast<float>(static_cast<double>(i % 5) / 4), 0.5f, 1.0f,
            static_cast<float>(static_cast<double>(i % 3 + 1) / 3)
        };
        const double width = 1920, height = 1080;
        const double scale_x = static_cast<double>(m[0]) * m[0] + static_cast<double>(m[1]) * m[1];
        const double scale_y = static_cast<double>(m[3]) * m[3] + static_cast<double>(m[4]) * m[4];
        const double pixel_x = std::sqrt(scale_x);
        const double pixel_y = std::sqrt(scale_y);
        const int joints = static_cast<int>(i % 3);
        const int caps = static_cast<int>(i / 3 % 3);

        PathState state;
        CHECK(FeedPath(state, commands.data(), commands.size(), 0, scale_x > scale_y ? scale_x : scale_y) == commands.size());
        const size_t size = StrokeSize(state, thicknesses[i % 4] / 2, joints, caps,
            pixel_x > pixel_y ? pixel_x : pixel_y);
        CHECK(size == reference.mesh_size);
        if (size != reference.mesh_size) {
            std::printf("  stroke case %zu: mesh size %zu, expected %u\n", i, size, reference.mesh_size);
            std::printf("::error::stroke case %zu: mesh size %zu, expected %u\n", i, size, reference.mesh_size);
            continue;
        }
        std::vector<float> mesh(size);
        const size_t index_count = FillMesh(state, mesh.data(), mesh.size(), 0,
            static_cast<float>(m[0] / width), static_cast<float>(m[1] / height),
            static_cast<float>(m[3] / width), static_cast<float>(m[4] / height),
            static_cast<float>(m[6] / width), static_cast<float>(m[7] / height),
            color[0], color[1], color[2], color[3]);
        CHECK(index_count == reference.index_count);
        if (reference.mesh_hash == 0) {
            continue;
        }
        const uint32_t hash = FnvCanonical(mesh.data(), mesh.size());
        CHECK(hash == reference.mesh_hash);
        if (hash != reference.mesh_hash) {
            std::printf("  stroke case %zu: mesh hash %08x, expected %08x\n", i, hash, reference.mesh_hash);
            std::printf("::error::stroke case %zu: mesh hash %08x, expected %08x\n", i, hash, reference.mesh_hash);
        }
    }
}

// ---- 線の外周 (WebGPU 版) -----------------------------------------------------------

static gpu::PathState MakeGpuPath(const std::vector<double>& points)
{
    gpu::PathState state;
    state.current = points;
    return state;
}

// 直線と端: 丸い端は 8 分割の扇形、四角い端は 5 点の四角形。描く線が無ければ経路に触らない
static void TestGpuStrokeLine()
{
    gpu::PathState state = MakeGpuPath({ 0, 0, 0,  10, 0, 0 });
    CHECK(gpu::StrokeSize(state, 2, 2, 0, 1.0) == gpu::kTriangleSize * 3);
    CHECK(state.current.empty());
    CHECK(state.vertices.size() == 1);
    const std::vector<double> rectangle = {
        0, 2, 0,  10, 2, 0,  10, -2, 0,  0, -2, 0,  0, 2, 0
    };
    CHECK(state.vertices[0] == rectangle);

    // 塗りのメッシュにできる。flat なら曲線用の係数はすべて 0.5
    std::vector<float> mesh(gpu::kTriangleSize * 3);
    CHECK(gpu::FillMesh(state, mesh.data(), mesh.size(), true) == 9);
    CHECK(mesh[0] == 0.0f && mesh[1] == 2.0f && mesh[2] == 0.5f && mesh[3] == 0.5f);
    CHECK(mesh[4] == 10.0f && mesh[5] == 2.0f);
    CHECK(mesh[8] == 10.0f && mesh[9] == -2.0f);

    // 四角い端: 端の点から外へ太さの分だけ延ばした四角形が、前後に付く
    gpu::PathState square = MakeGpuPath({ 0, 0, 0,  10, 0, 0 });
    CHECK(gpu::StrokeSize(square, 2, 1, 2, 1.0) == gpu::kTriangleSize * 9);
    CHECK(square.vertices.size() == 3);
    const std::vector<double> start_cap = {
        0, -2, 0,  -2, -2, 0,  -2, 2, 0,  0, 2, 0,  0, -2, 0
    };
    const std::vector<double> end_cap = {
        10, 2, 0,  12, 2, 0,  12, -2, 0,  10, -2, 0,  10, 2, 0
    };
    CHECK(Near(square.vertices[0], start_cap));
    CHECK(square.vertices[1] == rectangle);
    CHECK(Near(square.vertices[2], end_cap));

    // 丸い端: 中心と 9 点
    gpu::PathState round = MakeGpuPath({ 0, 0, 0,  10, 0, 0 });
    CHECK(gpu::StrokeSize(round, 2, 1, 1, 1.0) == gpu::kTriangleSize * (8 + 3 + 8));
    CHECK(round.vertices.size() == 3);
    CHECK(round.vertices[0].size() == 30);
    CHECK(round.vertices[0][0] == 0 && round.vertices[0][1] == 0);
    CHECK(std::fabs(round.vertices[0][3] - 0) < 1e-9 && std::fabs(round.vertices[0][4] - 2) < 1e-9);
    CHECK(std::fabs(round.vertices[0][15] + 2) < 1e-9 && std::fabs(round.vertices[0][16]) < 1e-9);
    CHECK(std::fabs(round.vertices[2][15] - 12) < 1e-9 && std::fabs(round.vertices[2][16]) < 1e-9);

    // 点が 1 つだけの経路は線にならず、経路はそのまま残る
    gpu::PathState single = MakeGpuPath({ 5, 5, 0 });
    CHECK(gpu::StrokeSize(single, 2, 2, 0, 1.0) == 0);
    CHECK(single.current.size() == 3);
    CHECK(single.vertices.empty());

    // 長さ 0 の直線は、太さ 0 の四角形になる (値が無くならない)
    gpu::PathState zero = MakeGpuPath({ 3, 4, 0,  3, 4, 0 });
    CHECK(gpu::StrokeSize(zero, 2, 2, 0, 1.0) == gpu::kTriangleSize * 3);
    const std::vector<double> point = {
        3, 4, 0,  3, 4, 0,  3, 4, 0,  3, 4, 0,  3, 4, 0
    };
    CHECK(zero.vertices[0] == point);
}

// つなぎ目: 曲線どうしでも作る。書きかけの経路と確定した経路の両方を線にする
static void TestGpuStrokeJoins()
{
    // 直角に曲がる 2 本の直線 (miter): 四角形 2 つの間に、つなぎ目 (6 点) が入る
    gpu::PathState miter = MakeGpuPath({ 0, 0, 0,  10, 0, 0,  10, 10, 0 });
    CHECK(gpu::StrokeSize(miter, 1, 1, 0, 1.0) == gpu::kTriangleSize * (3 + 4 + 3));
    CHECK(miter.vertices.size() == 3);
    const std::vector<double> join = {
        10, 0, 0,  11, 0, 0,  11, -1, 0,  10, 0, 0,  10, -1, 0,  11, -1, 0
    };
    CHECK(Near(miter.vertices[1], join));

    // bevel: 4 点
    gpu::PathState bevel = MakeGpuPath({ 0, 0, 0,  10, 0, 0,  10, 10, 0 });
    CHECK(gpu::StrokeSize(bevel, 1, 0, 0, 1.0) == gpu::kTriangleSize * (3 + 2 + 3));
    CHECK(bevel.vertices[1].size() == 12);

    // round: 中心と 9 点
    gpu::PathState round = MakeGpuPath({ 0, 0, 0,  10, 0, 0,  10, 10, 0 });
    CHECK(gpu::StrokeSize(round, 1, 2, 0, 1.0) == gpu::kTriangleSize * (3 + 8 + 3));
    CHECK(round.vertices[1].size() == 30);

    // 確定した経路と書きかけの経路
    gpu::PathState two;
    two.vertices.push_back({ 0, 0, 0,  10, 0, 0 });
    two.current = { 0, 5, 0,  10, 5, 0 };
    CHECK(gpu::StrokeSize(two, 1, 1, 0, 1.0) == gpu::kTriangleSize * 6);
    CHECK(two.vertices.size() == 2);
    CHECK(two.current.empty());
}

// 曲線: 輪郭の右側は、各区間の始点が前の区間の終点と同じ値になる
static void TestGpuStrokeCurve()
{
    gpu::PathState state = MakeGpuPath({ 0, 0, 0,  50, 80, 1,  100, 0, 0 });
    const size_t size = gpu::StrokeSize(state, 3, 1, 0, 4.0);
    CHECK(size > 0);
    CHECK(state.vertices.size() == 1);
    const std::vector<double>& outline = state.vertices[0];
    // 点の数は 2 + 4 * 区間数。制御点と通過点が交互に並ぶ
    const size_t points = outline.size() / 3;
    CHECK(points >= 6 && (points - 2) % 4 == 0);
    CHECK(size == (points - 2) * gpu::kTriangleSize);
    for (size_t idx = 0; idx < points; ++idx) {
        const size_t half = points / 2;
        const size_t local = idx < half ? idx : idx - half;
        CHECK(outline[idx * 3 + 2] == (local % 2 == 1 ? 1.0 : 0.0));
    }
}

// Next2D Player (WebGPU) の JavaScript (canvas の内外判定は、実行環境の canvas と同じ計算) で求めた値。
// 三角関数を使う場合 (丸いつなぎ目、丸い端) は、ハッシュ値を持たない (0)。
struct GpuStrokeReference {
    uint32_t command_length;
    uint32_t mesh_size;
    uint32_t index_count;
    uint32_t mesh_hash;
    uint32_t flat_hash;
};

static const GpuStrokeReference kGpuStrokeReferences[] = {
    { 171u, 43548u, 10887u, 0xe6b519edu, 0xbda74ebdu },
    { 59u, 5532u, 1383u, 0x68970482u, 0x71d038a2u },
    { 138u, 132624u, 33156u, 0x00000000u, 0x00000000u },
    { 185u, 108624u, 27156u, 0x00000000u, 0x00000000u },
    { 67u, 36612u, 9153u, 0x00000000u, 0x00000000u },
    { 23u, 13812u, 3453u, 0x00000000u, 0x00000000u },
    { 44u, 8856u, 2214u, 0xd414d170u, 0x933dcf54u },
    { 194u, 48648u, 12162u, 0x75ee9ccdu, 0x09941a81u },
    { 116u, 66156u, 16539u, 0x00000000u, 0x00000000u },
    { 130u, 82872u, 20718u, 0xd5dfbfd0u, 0xd96cef7cu },
    { 32u, 14628u, 3657u, 0xf3b781a2u, 0xf4ce3322u },
    { 77u, 111900u, 27975u, 0x00000000u, 0x00000000u },
    { 104u, 24012u, 6003u, 0x00000000u, 0x00000000u },
    { 84u, 19020u, 4755u, 0x00000000u, 0x00000000u },
    { 33u, 23820u, 5955u, 0x00000000u, 0x00000000u },
    { 109u, 75216u, 18804u, 0xbd0979e6u, 0x0e084ea2u },
    { 102u, 31128u, 7782u, 0xc83ff7d1u, 0xb01ac5a9u },
    { 133u, 146988u, 36747u, 0x00000000u, 0x00000000u },
    { 43u, 5844u, 1461u, 0x045a1308u, 0x096af038u },
    { 111u, 20988u, 5247u, 0x05dd116au, 0x91bd841eu },
    { 30u, 24516u, 6129u, 0x00000000u, 0x00000000u },
    { 72u, 19332u, 4833u, 0x00000000u, 0x00000000u },
    { 77u, 23292u, 5823u, 0x00000000u, 0x00000000u },
    { 124u, 256572u, 64143u, 0x00000000u, 0x00000000u },
    { 161u, 44484u, 11121u, 0x267f68e2u, 0xc33e27b2u },
    { 66u, 10452u, 2613u, 0x2b5c7dabu, 0x678d3193u },
    { 120u, 93660u, 23415u, 0x00000000u, 0x00000000u },
    { 115u, 63492u, 15873u, 0xe8148fdeu, 0x8283655au },
    { 128u, 35340u, 8835u, 0x08d3b969u, 0xcab985c9u },
    { 62u, 39708u, 9927u, 0x00000000u, 0x00000000u },
    { 17u, 1908u, 477u, 0x00000000u, 0x00000000u },
    { 55u, 20028u, 5007u, 0x00000000u, 0x00000000u },
    { 50u, 45960u, 11490u, 0x00000000u, 0x00000000u },
    { 32u, 11784u, 2946u, 0xfc7d2d0au, 0xbc8b01e6u },
    { 94u, 44136u, 11034u, 0x949aefebu, 0xbdba4b9bu },
    { 89u, 231516u, 57879u, 0x00000000u, 0x00000000u },
    { 122u, 20832u, 5208u, 0x433ecc60u, 0xb297d8d0u },
    { 171u, 32016u, 8004u, 0x85cad8bbu, 0xab3522dfu },
    { 59u, 40308u, 10077u, 0x00000000u, 0x00000000u },
    { 150u, 99792u, 24948u, 0x00000000u, 0x00000000u },
    { 43u, 27876u, 6969u, 0x00000000u, 0x00000000u },
    { 91u, 45336u, 11334u, 0x00000000u, 0x00000000u },
    { 66u, 11364u, 2841u, 0xf688bb6au, 0x2a20988au },
    { 23u, 6588u, 1647u, 0x6663d1d3u, 0xd4b25e03u },
    { 121u, 86148u, 21537u, 0x00000000u, 0x00000000u },
    { 79u, 36132u, 9033u, 0xe862480eu, 0x95ade4eau },
    { 85u, 42480u, 10620u, 0x57bdecf0u, 0x5684d5a4u },
    { 179u, 364632u, 91158u, 0x00000000u, 0x00000000u },
};

static void TestGpuStrokeAgainstJavaScript()
{
    const double thicknesses[] = { 0.5, 1, 2.5, 10 };
    const size_t cases = sizeof(kGpuStrokeReferences) / sizeof(kGpuStrokeReferences[0]);
    for (size_t i = 0; i < cases; ++i) {
        const GpuStrokeReference& reference = kGpuStrokeReferences[i];
        const std::vector<float> commands = MakeCommands(i);
        CHECK(commands.size() == reference.command_length);

        float m[9];
        MakeMatrix(i, m);
        const double scale_x = std::sqrt(static_cast<double>(m[0]) * m[0] + static_cast<double>(m[1]) * m[1]);
        const double scale_y = std::sqrt(static_cast<double>(m[3]) * m[3] + static_cast<double>(m[4]) * m[4]);
        const double pixel_scale = scale_x > scale_y ? scale_x : scale_y;
        const int joints = static_cast<int>(i % 3);
        const int caps = static_cast<int>(i / 3 % 3);

        gpu::PathState state;
        CHECK(gpu::FeedPath(state, commands.data(), commands.size(), 0, pixel_scale) == commands.size());
        const size_t size = gpu::StrokeSize(state, thicknesses[i % 4] / 2, joints, caps, pixel_scale);
        CHECK(size == reference.mesh_size);
        if (size != reference.mesh_size) {
            std::printf("  webgpu stroke case %zu: mesh size %zu, expected %u\n", i, size, reference.mesh_size);
            std::printf("::error::webgpu stroke case %zu: mesh size %zu, expected %u\n", i, size, reference.mesh_size);
            continue;
        }
        std::vector<float> mesh(size);
        CHECK(gpu::FillMesh(state, mesh.data(), mesh.size()) == reference.index_count);
        std::vector<float> flat(size);
        CHECK(gpu::FillMesh(state, flat.data(), flat.size(), true) == reference.index_count);
        if (reference.mesh_hash == 0) {
            continue;
        }
        const uint32_t hash = FnvCanonical(mesh.data(), mesh.size());
        CHECK(hash == reference.mesh_hash);
        if (hash != reference.mesh_hash) {
            std::printf("  webgpu stroke case %zu: mesh hash %08x, expected %08x\n", i, hash, reference.mesh_hash);
            std::printf("::error::webgpu stroke case %zu: mesh hash %08x, expected %08x\n", i, hash, reference.mesh_hash);
        }
        const uint32_t flat_hash = FnvCanonical(flat.data(), flat.size());
        CHECK(flat_hash == reference.flat_hash);
        if (flat_hash != reference.flat_hash) {
            std::printf("  webgpu stroke case %zu: flat hash %08x, expected %08x\n", i, flat_hash, reference.flat_hash);
            std::printf("::error::webgpu stroke case %zu: flat hash %08x, expected %08x\n", i, flat_hash, reference.flat_hash);
        }
    }
}

int main()
{
    TestFeedPath();
    TestFinalizePaths();
    TestTriangleMesh();
    TestCurveMesh();
    TestCubicSubdivision();
    TestAgainstJavaScript();
    TestGpuPath();
    TestGpuCurveMesh();
    TestGpuArc();
    TestGpuAgainstJavaScript();
    TestStrokeLine();
    TestStrokeJoins();
    TestStrokeCaps();
    TestStrokeCurve();
    TestJsHypot();
    TestStrokeAgainstJavaScript();
    TestGpuStrokeLine();
    TestGpuStrokeJoins();
    TestGpuStrokeCurve();
    TestGpuStrokeAgainstJavaScript();
    if (g_failed) {
        std::printf("%d check(s) failed\n", g_failed);
        // 結果が食い違うのは、たいてい積和の融合 (FMA) が入ったとき。原因を切り分ける情報を出す
        const char* arch =
#if defined(__aarch64__) || defined(_M_ARM64)
            "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
            "x64";
#else
            "other";
#endif
        const char* compiler =
#if defined(__VERSION__)
            __VERSION__;
#elif defined(_MSC_FULL_VER)
            "msvc";
#else
            "unknown";
#endif
        int fast_fma = 0;
#if defined(__FP_FAST_FMA) || defined(__FMA__)
        fast_fma = 1;
#endif
        // 融合されていれば 0 以外になる (a * b を丸めてから引くと 0)
        volatile double a = 1.0 + 0x1p-30, b = 1.0 - 0x1p-30;
        const double product = a * b;
        const double residual = a * b - product;
        std::printf("::error::%d check(s) failed (%s, %s, fma available %d, contraction residual %g)\n",
            g_failed, arch, compiler, fast_fma, residual);
        return 1;
    }
    std::printf("shape accelerator tests passed\n");
    return 0;
}
