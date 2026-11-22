// 図形の計算 (src/bindings/ShapeAcceleratorCore.h) のテスト。
// 「経路の組み立て」と「塗りのメッシュの生成」が、Next2D Player (WebGL) の JavaScript と
// 1 ビットも違わない結果を返すことを確かめる。V8 にも OS にも依存しない。
//
// ビルド (templates/xbox で):
//   cl /nologo /utf-8 /std:c++17 /EHsc /W3 /Isrc tests\shape_accelerator_test.cpp /Fe:shape_accelerator_test.exe
//   c++ -std=c++17 -Wall -Wextra -O1 -Isrc -o shape_accelerator_test tests/shape_accelerator_test.cpp
#include "../src/bindings/ShapeAcceleratorCore.h"

#include <cstdio>
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

static uint32_t Fnv(const float* data, size_t length)
{
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(data);
    uint32_t hash = 0x811c9dc5u;
    for (size_t i = 0; i < length * sizeof(float); ++i) {
        hash = (hash ^ bytes[i]) * 0x01000193u;
    }
    return hash;
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

static void TestAgainstJavaScript()
{
    const double scales[] = { 0.03125, 0.25, 1, 2.5, 8, 40 };
    const size_t cases = sizeof(kReferences) / sizeof(kReferences[0]);
    for (size_t i = 0; i < cases; ++i) {
        const Reference& reference = kReferences[i];
        Random random { (0x9e3779b9u ^ (static_cast<uint32_t>(i) * 0x85ebca6bu)) | 1u };
        const int mode = static_cast<int>(i % 3);

        // C++ は引数を評価する順序を決めていないので、乱数は 1 つずつ変数に受けてから並べる
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
        CHECK(commands.size() == reference.command_length);
        CHECK(Fnv(commands.data(), commands.size()) == reference.command_hash);

        // 描画行列と色 (JavaScript では 32 ビットの配列に入っている)
        const double s = scales[i % 6];
        float m[9] = { static_cast<float>(s), 0, 0, 0, static_cast<float>(s * 0.75), 0, 100.5f, -20.25f, 1 };
        if (i % 4 == 3) {
            m[0] = static_cast<float>(s * 0.8);
            m[1] = static_cast<float>(s * 0.6);
            m[3] = static_cast<float>(-s * 0.6);
            m[4] = static_cast<float>(s * 0.8);
        }
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
    if (g_failed) {
        std::printf("%d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("shape accelerator tests passed\n");
    return 0;
}
