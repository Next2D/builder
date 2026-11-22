// ソフトウェアミキサー (src/platform/AudioMixer.cpp) のテスト。
// OS の音声 API を使わない実装なので、どの環境でも同じ結果になる。
//
// ビルド (templates/xbox で):
//   cl /nologo /std:c++17 /EHsc /W3 /DNOMINMAX /DNEXT2D_AUDIO_MIXER=1 /Isrc /I. ^
//      tests\audio_mixer_test.cpp src\platform\AudioMixer.cpp src\platform\AudioDecoder.cpp ^
//      /Fe:audio_mixer_test.exe
#include "../src/platform/AudioEngine.h"
#include "../src/platform/AudioDecoder.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
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

// 少しずつ増える音源 (PcmStream) を作る。frames 個ぶんのブロックを確保し、値は frame(i) で決める。
static std::shared_ptr<next2d::PcmBuffer> MakeStream(uint32_t rate, size_t capacity_frames,
                                                     float (*value)(size_t))
{
    auto pcm = std::make_shared<next2d::PcmBuffer>();
    pcm->sample_rate = rate;
    pcm->channels = 1;
    pcm->stream = std::make_shared<next2d::PcmStream>();
    const size_t block = next2d::PcmStream::kBlockFrames;
    for (size_t first = 0; first < capacity_frames; first += block) {
        auto data = std::make_shared<std::vector<float>>(block);
        for (size_t i = 0; i < block; ++i) {
            (*data)[i] = value(first + i);
        }
        pcm->stream->blocks.push_back(data);
    }
    return pcm;
}

static void SetStreamProgress(const std::shared_ptr<next2d::PcmBuffer>& pcm, uint64_t frames, bool complete)
{
    std::lock_guard<std::mutex> lock(pcm->stream->mutex);
    pcm->stream->frames = frames;
    pcm->stream->complete = complete;
}

// 少しずつ増える音源: 続きができるまでは無音で待ち、最後までできてから終了する
static void TestStreamWaitsForMore()
{
    next2d::AudioEngine engine;
    engine.Initialize();
    auto pcm = MakeStream(48000, 4, [](size_t i) { return 0.1f * static_cast<float>(i + 1); });
    SetStreamProgress(pcm, 2, false);

    auto voice = engine.CreateVoice(pcm);
    voice->Start(false);
    float out[4] = {9, 9, 9, 9};
    CHECK(engine.Render(out, 4, 1, 48000));
    CHECK(Near(out[0], 0.1f));
    CHECK(Near(out[1], 0.2f));
    CHECK(out[2] == 0.0f);          // 続きがまだ無い
    CHECK(out[3] == 0.0f);
    CHECK(!voice->IsFinished());

    // 何度合成しても、続きができるまでは同じ位置で待つ
    engine.Render(out, 4, 1, 48000);
    CHECK(out[0] == 0.0f);
    CHECK(!voice->IsFinished());

    SetStreamProgress(pcm, 4, false);
    engine.Render(out, 4, 1, 48000);
    CHECK(Near(out[0], 0.3f));      // 待っていた位置から続く
    CHECK(Near(out[1], 0.4f));
    CHECK(out[2] == 0.0f);
    CHECK(!voice->IsFinished());    // まだ増えるかもしれない

    SetStreamProgress(pcm, 4, true);
    engine.Render(out, 4, 1, 48000);
    CHECK(out[0] == 0.0f);
    CHECK(voice->IsFinished());
}

// 少しずつ増える音源: ブロックの境目をまたいで読める。ループは最後までできてから先頭へ戻る
static void TestStreamBlocksAndLoop()
{
    const size_t block = next2d::PcmStream::kBlockFrames;
    next2d::AudioEngine engine;
    engine.Initialize();
    auto pcm = MakeStream(48000, block + 2, [](size_t i) {
        return static_cast<float>(i % 1000) / 1000.0f;
    });
    SetStreamProgress(pcm, block + 2, false);

    auto voice = engine.CreateVoice(pcm);
    voice->Start(true);
    std::vector<float> out(block + 4);
    engine.Render(out.data(), static_cast<int>(out.size()), 1, 48000);
    CHECK(Near(out[block - 1], static_cast<float>((block - 1) % 1000) / 1000.0f));
    CHECK(Near(out[block], static_cast<float>(block % 1000) / 1000.0f));          // 2 個目のブロックの先頭
    CHECK(Near(out[block + 1], static_cast<float>((block + 1) % 1000) / 1000.0f));
    CHECK(out[block + 2] == 0.0f);   // まだ増えるかもしれないので、先頭へは戻らない
    CHECK(!voice->IsFinished());

    SetStreamProgress(pcm, block + 2, true);
    float loop[3] = {9, 9, 9};
    engine.Render(loop, 3, 1, 48000);
    CHECK(Near(loop[0], 0.0f));      // 先頭へ戻る
    CHECK(Near(loop[1], 0.001f));
    CHECK(Near(loop[2], 0.002f));
    CHECK(!voice->IsFinished());
}

// 少しずつ増える音源: 1 フレームもできずに終わったら (デコード失敗)、すぐ終了
static void TestStreamEmpty()
{
    next2d::AudioEngine engine;
    engine.Initialize();
    auto pcm = MakeStream(48000, 0, [](size_t) { return 0.0f; });
    auto voice = engine.CreateVoice(pcm);
    voice->Start(false);
    float out[2] = {9, 9};
    engine.Render(out, 2, 1, 48000);
    CHECK(out[0] == 0.0f);
    CHECK(!voice->IsFinished());     // まだデコードが始まっていないだけかもしれない

    SetStreamProgress(pcm, 0, true);
    engine.Render(out, 2, 1, 48000);
    CHECK(voice->IsFinished());
}

// 少しずつデコードできない入力 (MP3 でない、短すぎる、開けない) は nullptr
static void TestStreamDecoderRejects()
{
    next2d::PcmBuffer pcm;
    CHECK(!next2d::AudioStreamDecoder::Open(nullptr, pcm));
    CHECK(!next2d::AudioStreamDecoder::Open(
        std::make_unique<std::istringstream>(std::string("RIFF\0\0\0\0WAVEfmt ", 16)), pcm));
    CHECK(!next2d::AudioStreamDecoder::Open(
        std::make_unique<std::istringstream>(std::string("OggS\0\0\0\0\0\0\0\0", 12)), pcm));
    CHECK(!next2d::AudioStreamDecoder::Open(std::make_unique<std::istringstream>(std::string("ID")), pcm));
    // 同期語だけで、フレームとして読めない
    CHECK(!next2d::AudioStreamDecoder::Open(
        std::make_unique<std::istringstream>(std::string("\xFF\xFB\0\0\0\0\0\0", 8)), pcm));
    CHECK(!pcm.stream);
}

// 無音の MP3 を作る (MPEG-1 Layer III、128 kbps、44.1 kHz、ステレオ)。ヘッダの後ろがすべて 0 の
// フレームは、無音のフレームとしてデコードできる。1 フレームは 417 バイトで 1152 サンプル。
static std::string MakeSilentMp3(int frames)
{
    std::string data;
    for (int i = 0; i < frames; ++i) {
        std::string frame(417, '\0');
        frame[0] = static_cast<char>(0xFF);
        frame[1] = static_cast<char>(0xFB);
        frame[2] = static_cast<char>(0x90);
        frame[3] = static_cast<char>(0x00);
        data += frame;
    }
    return data;
}

// MP3 を少しずつデコードする: 指定した量ずつ進み、全体をまとめてデコードした結果と同じになる
static void TestStreamDecoder()
{
    const size_t block = next2d::PcmStream::kBlockFrames;
    const std::string mp3 = MakeSilentMp3(120);   // 138240 サンプル = ブロック 4 個と少し

    next2d::PcmBuffer whole;
    CHECK(next2d::DecodeAudio(std::vector<uint8_t>(mp3.begin(), mp3.end()), whole));
    const size_t whole_frames = whole.channels ? whole.samples.size() / whole.channels : 0;
    CHECK(whole_frames > block * 4);

    next2d::PcmBuffer pcm;
    auto decoder = next2d::AudioStreamDecoder::Open(std::make_unique<std::istringstream>(mp3), pcm);
    CHECK(decoder != nullptr);
    if (!decoder) {
        return;
    }
    CHECK(pcm.stream != nullptr);
    CHECK(pcm.samples.empty());
    CHECK(pcm.channels == whole.channels);
    CHECK(pcm.sample_rate == whole.sample_rate);
    CHECK(pcm.stream->frames == 0);

    // 先頭だけ: 指定した量以上で止まり、まだ続きがある
    CHECK(decoder->DecodeSome(block * 2));
    CHECK(pcm.stream->frames >= block * 2);
    CHECK(pcm.stream->frames < whole_frames);
    CHECK(!pcm.stream->complete);

    // 途中まででも再生できる
    next2d::AudioEngine engine;
    engine.Initialize();
    auto shared = std::make_shared<next2d::PcmBuffer>(pcm);
    auto voice = engine.CreateVoice(shared);
    voice->Start(false);
    float out[64];
    CHECK(engine.Render(out, 32, 2, static_cast<int>(pcm.sample_rate)));
    CHECK(!voice->IsFinished());

    // 最後まで
    int slices = 0;
    while (decoder->DecodeSome(block)) {
        ++slices;
        CHECK(!pcm.stream->complete);
    }
    CHECK(slices >= 1);
    CHECK(pcm.stream->complete);
    CHECK(pcm.stream->frames == whole_frames);
    CHECK(pcm.stream->blocks.size() == (whole_frames + block - 1) / block);
    CHECK(!decoder->DecodeSome(block));   // 終わった後に呼んでも増えない
    CHECK(pcm.stream->frames == whole_frames);

    // 中身も同じ
    bool same = true;
    for (size_t i = 0; i < whole_frames * whole.channels && same; ++i) {
        const size_t per_block = block * whole.channels;
        same = (*pcm.stream->blocks[i / per_block])[i % per_block] == whole.samples[i];
    }
    CHECK(same);
}

int main()
{
    TestStreamDecoder();
    TestStreamWaitsForMore();
    TestStreamBlocksAndLoop();
    TestStreamEmpty();
    TestStreamDecoderRejects();
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
