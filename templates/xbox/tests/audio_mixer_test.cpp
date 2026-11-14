// ソフトウェアミキサー (src/platform/AudioMixer.cpp) のテスト。
// OS の音声 API を使わない実装なので、どの環境でも同じ結果になる。
//
// ビルド (templates/xbox で):
//   cl /nologo /std:c++17 /EHsc /W3 /DNOMINMAX /DNEXT2D_AUDIO_MIXER=1 /Isrc /I. ^
//      tests\audio_mixer_test.cpp src\platform\AudioMixer.cpp src\platform\AudioDecoder.cpp ^
//      /Fe:audio_mixer_test.exe
#include "../src/platform/AudioEngine.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

static int g_failed = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::printf("::error file=templates/xbox/tests/audio_mixer_test.cpp,line=%d::%s\n", __LINE__, #cond); \
            ++g_failed; \
        } \
    } while (0)

static bool Near(float a, float b)
{
    return std::fabs(a - b) < 1e-4f;
}

static std::shared_ptr<next2d::PcmBuffer> MakeBuffer(uint32_t rate, uint32_t channels,
                                                     std::vector<float> samples)
{
    auto pcm = std::make_shared<next2d::PcmBuffer>();
    pcm->sample_rate = rate;
    pcm->channels = channels;
    pcm->samples = std::move(samples);
    return pcm;
}

// 出力先がつながっていない間は「再生開始と同時に終了」
static void TestNoConsumer()
{
    next2d::AudioEngine engine;
    CHECK(engine.Initialize());
    auto voice = engine.CreateVoice(MakeBuffer(48000, 1, {0.5f, 0.5f}));
    CHECK(!voice->IsFinished());   // 未開始
    voice->Start(false);
    CHECK(voice->IsFinished());
}

// モノラル → ステレオ、音量、終了判定
static void TestOneShot()
{
    next2d::AudioEngine engine;
    engine.Initialize();
    float out[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    CHECK(engine.Render(out, 4, 2, 48000));   // 再生中のボイス無し → 無音
    for (float s : out) {
        CHECK(s == 0.0f);
    }

    auto voice = engine.CreateVoice(MakeBuffer(48000, 1, {0.25f, 0.5f, -0.5f}));
    voice->SetVolume(0.5f);
    voice->Start(false);
    CHECK(!voice->IsFinished());

    CHECK(engine.Render(out, 4, 2, 48000));
    CHECK(Near(out[0], 0.125f) && Near(out[1], 0.125f));
    CHECK(Near(out[2], 0.25f) && Near(out[3], 0.25f));
    CHECK(Near(out[4], -0.25f) && Near(out[5], -0.25f));
    CHECK(out[6] == 0.0f && out[7] == 0.0f);   // 末尾を過ぎたら無音
    CHECK(voice->IsFinished());

    // 再生し直せる
    voice->Start(false);
    CHECK(!voice->IsFinished());
    engine.Render(out, 1, 2, 48000);
    CHECK(Near(out[0], 0.125f));
    voice->Stop();
    engine.Render(out, 1, 2, 48000);
    CHECK(out[0] == 0.0f);
    CHECK(!voice->IsFinished());   // Stop は終了扱いにしない (XAudio2 実装と同じ)
}

// ループ、複数ボイスの加算、クリップ
static void TestLoopAndMix()
{
    next2d::AudioEngine engine;
    engine.Initialize();
    auto loop = engine.CreateVoice(MakeBuffer(48000, 2, {0.5f, -0.5f, 0.25f, -0.25f}));
    loop->Start(true);
    float out[10];
    engine.Render(out, 5, 2, 48000);
    CHECK(Near(out[0], 0.5f) && Near(out[1], -0.5f));
    CHECK(Near(out[2], 0.25f) && Near(out[3], -0.25f));
    CHECK(Near(out[4], 0.5f) && Near(out[5], -0.5f));    // 先頭へ戻る
    CHECK(Near(out[8], 0.5f) && Near(out[9], -0.5f));
    CHECK(!loop->IsFinished());

    auto loud = engine.CreateVoice(MakeBuffer(48000, 2, {0.75f, -0.75f}));
    loud->Start(false);
    engine.Render(out, 1, 2, 48000);
    // loop は 2 フレーム周期で 5 フレーム進んだので 2 フレーム目 (0.25) にいる
    CHECK(Near(out[0], 1.0f) && Near(out[1], -1.0f));   // 0.25 + 0.75 = 1.0

    loud->Start(false);
    loud->SetVolume(4.0f);
    engine.Render(out, 1, 2, 48000);
    CHECK(out[0] == 1.0f && out[1] == -1.0f);           // [-1, 1] に収める
}

// サンプリングレート変換 (線形補間) と、ステレオ → モノラル
static void TestResample()
{
    next2d::AudioEngine engine;
    engine.Initialize();
    auto voice = engine.CreateVoice(MakeBuffer(24000, 1, {0.0f, 0.5f, 1.0f}));
    voice->Start(false);
    float out[8];
    engine.Render(out, 8, 1, 48000);
    CHECK(Near(out[0], 0.0f) && Near(out[1], 0.25f) && Near(out[2], 0.5f) && Near(out[3], 0.75f));
    CHECK(Near(out[4], 1.0f));
    CHECK(out[6] == 0.0f && out[7] == 0.0f);
    CHECK(voice->IsFinished());

    auto stereo = engine.CreateVoice(MakeBuffer(48000, 2, {0.5f, 0.25f}));
    stereo->Start(false);
    engine.Render(out, 1, 1, 48000);
    CHECK(Near(out[0], 0.375f));
}

// 破棄したボイスや空のバッファで壊れない
static void TestLifetime()
{
    next2d::AudioEngine engine;
    engine.Initialize();
    float out[4];
    {
        auto voice = engine.CreateVoice(MakeBuffer(48000, 1, {0.5f, 0.5f}));
        voice->Start(true);
    }
    CHECK(engine.Render(out, 2, 2, 48000));
    CHECK(out[0] == 0.0f);

    auto empty = engine.CreateVoice(MakeBuffer(48000, 2, {}));
    empty->Start(false);
    engine.Render(out, 2, 2, 48000);
    CHECK(empty->IsFinished());

    CHECK(!engine.Render(nullptr, 2, 2, 48000));
    CHECK(!engine.Render(out, 0, 2, 48000));

    // エンジンより長生きするボイス
    std::shared_ptr<next2d::AudioVoice> survivor;
    {
        next2d::AudioEngine temporary;
        survivor = temporary.CreateVoice(MakeBuffer(48000, 1, {0.5f}));
    }
    survivor->Start(false);
    survivor->Stop();
}

int main()
{
    TestNoConsumer();
    TestOneShot();
    TestLoopAndMix();
    TestResample();
    TestLifetime();
    if (g_failed) {
        std::printf("%d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("audio mixer tests passed\n");
    return 0;
}
