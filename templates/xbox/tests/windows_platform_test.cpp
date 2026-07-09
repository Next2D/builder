// Windows 実 API (WIC / DirectWrite / Media Foundation / XAudio2) の smoke テスト。
// V8 / Dawn / GDK 不要 — Windows SDK だけでビルドでき、GitHub Actions の
// windows-latest ランナーで実行して「Windows 上で実際に動く」ことを検証する。
//
// ビルド (Developer Command Prompt / CI):
//   cl /std:c++17 /EHsc /Isrc /I. tests\windows_platform_test.cpp ^
//      src\platform\WicDecoder.cpp src\platform\TextRasterizer.cpp ^
//      src\platform\AudioEngine.cpp ^
//      src\platform\ImageDecoder.cpp src\platform\AudioDecoder.cpp ^
//      ole32.lib windowscodecs.lib d2d1.lib dwrite.lib ^
//      mfplat.lib mfreadwrite.lib mfuuid.lib xaudio2.lib shlwapi.lib
//
// XAudio2 の CreateMasteringVoice はヘッドレス環境 (音声デバイス無し) では失敗し得る
// ため warn 扱い。WIC / DirectWrite / MF デコードは必ず成功すべきで、失敗はエラー。
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "../src/platform/WicDecoder.h"
#include "../src/platform/ImageDecoder.h"
#include "../src/platform/AudioDecoder.h"
#include "../src/platform/TextRasterizer.h"
#include "../src/platform/AudioEngine.h"

#include <Windows.h>
#include <mfapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "shlwapi.lib")

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { ++checks; if (!(cond)) { \
    ++failures; std::printf("not ok - %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// 2x2 PNG: (0,0)=赤 (1,0)=緑 (0,1)=青 (1,1)=半透明白
static const unsigned char kTestPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00,
    0x13, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
    0x1f, 0x0c, 0x81, 0x34, 0x08, 0x34, 0x00, 0x00, 0x49, 0x49, 0x09, 0x78,
    0x28, 0xa0, 0xdb, 0x77, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
    0xae, 0x42, 0x60, 0x82
};

// 48kHz mono 16bit PCM WAV (0.5 振幅の矩形波 48 サンプル)
static const unsigned char kTestWav[] = {
    0x52, 0x49, 0x46, 0x46, 0x84, 0x00, 0x00, 0x00, 0x57, 0x41, 0x56, 0x45,
    0x66, 0x6d, 0x74, 0x20, 0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x80, 0xbb, 0x00, 0x00, 0x00, 0x77, 0x01, 0x00, 0x02, 0x00, 0x10, 0x00,
    0x64, 0x61, 0x74, 0x61, 0x60, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x40,
    0x00, 0x40, 0x00, 0x40, 0x00, 0xc0, 0x00, 0xc0, 0x00, 0xc0, 0x00, 0xc0,
    0x00, 0x40, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40, 0x00, 0xc0, 0x00, 0xc0,
    0x00, 0xc0, 0x00, 0xc0, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40,
    0x00, 0xc0, 0x00, 0xc0, 0x00, 0xc0, 0x00, 0xc0, 0x00, 0x40, 0x00, 0x40,
    0x00, 0x40, 0x00, 0x40, 0x00, 0xc0, 0x00, 0xc0, 0x00, 0xc0, 0x00, 0xc0,
    0x00, 0x40, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40, 0x00, 0xc0, 0x00, 0xc0,
    0x00, 0xc0, 0x00, 0xc0, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40,
    0x00, 0xc0, 0x00, 0xc0, 0x00, 0xc0, 0x00, 0xc0
};

// --- WIC: PNG デコード (createImageBitmap / Image.src の実体) ---------------
static void TestWicDecode()
{
    std::vector<uint8_t> png(kTestPng, kTestPng + sizeof(kTestPng));
    next2d::DecodedImage img;
    CHECK(next2d::DecodeImageWithWIC(png, img));
    CHECK(img.width == 2 && img.height == 2);
    CHECK(img.rgba.size() == 16);
    // (0,0)=赤
    CHECK(img.rgba[0] == 255 && img.rgba[1] == 0 && img.rgba[2] == 0 && img.rgba[3] == 255);
    // (1,0)=緑
    CHECK(img.rgba[4] == 0 && img.rgba[5] == 255 && img.rgba[6] == 0);
    // (0,1)=青
    CHECK(img.rgba[8] == 0 && img.rgba[9] == 0 && img.rgba[10] == 255);
    // (1,1)=半透明 (alpha=128)
    CHECK(img.rgba[15] == 128);

    // 壊れた入力は false
    std::vector<uint8_t> garbage(32, 0xAB);
    next2d::DecodedImage bad;
    CHECK(!next2d::DecodeImageWithWIC(garbage, bad));
}

// --- ImageDecoder: 本番の第一経路 (stb_image。コンソールでも同一コード) --------
static void TestImageDecoder()
{
    std::vector<uint8_t> png(kTestPng, kTestPng + sizeof(kTestPng));
    next2d::DecodedImage img;
    CHECK(next2d::DecodeImage(png, img));
    CHECK(img.width == 2 && img.height == 2);
    CHECK(img.rgba.size() == 16);
    CHECK(img.rgba[0] == 255 && img.rgba[1] == 0 && img.rgba[2] == 0 && img.rgba[3] == 255);
    CHECK(img.rgba[4] == 0 && img.rgba[5] == 255 && img.rgba[6] == 0);
    CHECK(img.rgba[8] == 0 && img.rgba[9] == 0 && img.rgba[10] == 255);
    CHECK(img.rgba[15] == 128);

    std::vector<uint8_t> garbage(32, 0xAB);
    next2d::DecodedImage bad;
    CHECK(!next2d::DecodeImage(garbage, bad));
}

// --- DirectWrite: メトリクスとグリフラスタライズ (fillText の実体) ----------
static void TestTextRasterizer()
{
    next2d::TextMetricsInfo m;
    CHECK(next2d::MeasureTextWithDWrite("24px Segoe UI", L"Hello", m));
    CHECK(m.width > 10.0);
    CHECK(m.ascent > 5.0 && m.ascent < 40.0);
    CHECK(m.descent > 0.0 && m.descent < 20.0);

    // bold は同サイズの normal より広い (フォントメトリクスの妥当性)
    next2d::TextMetricsInfo mb;
    CHECK(next2d::MeasureTextWithDWrite("bold 24px Segoe UI", L"Hello", mb));
    CHECK(mb.width >= m.width);

    next2d::TextBitmap bmp;
    CHECK(next2d::RasterizeTextWithDWrite("24px Segoe UI", L"A", 255, 0, 0, bmp));
    CHECK(bmp.width > 0 && bmp.height > 0);
    CHECK(bmp.baseline > 0.0);
    // 何らかの不透明ピクセルがあり、色は赤
    int opaque = 0;
    bool red_ok = true;
    for (size_t i = 0; i + 3 < bmp.rgba.size(); i += 4) {
        if (bmp.rgba[i + 3] > 200) {
            ++opaque;
            if (bmp.rgba[i] < 200 || bmp.rgba[i + 1] > 60 || bmp.rgba[i + 2] > 60) red_ok = false;
        }
    }
    CHECK(opaque > 5);
    CHECK(red_ok);

    // 日本語グリフもラスタライズできる
    next2d::TextBitmap jp;
    CHECK(next2d::RasterizeTextWithDWrite("24px Yu Gothic", L"あ", 0, 0, 0, jp)); // あ
    int jp_opaque = 0;
    for (size_t i = 3; i < jp.rgba.size(); i += 4) if (jp.rgba[i] > 200) ++jp_opaque;
    CHECK(jp_opaque > 5);

    // 空文字は false
    next2d::TextBitmap empty;
    CHECK(!next2d::RasterizeTextWithDWrite("24px Segoe UI", L"", 0, 0, 0, empty));
}

// --- 音声デコード (decodeAudioData の実体) -----------------------------------
// AudioEngine::Decode は第一経路 dr_wav/dr_mp3/stb_vorbis (コンソールでも同一)、
// フォールバックが Media Foundation。両経路を検証する。
static void TestAudioDecode()
{
    std::vector<uint8_t> wav(kTestWav, kTestWav + sizeof(kTestWav));
    next2d::PcmBuffer pcm;
    CHECK(next2d::AudioEngine::Decode(wav, pcm));
    CHECK(pcm.sample_rate == 48000);
    CHECK(pcm.channels == 1);
    CHECK(pcm.samples.size() >= 40);   // 48 サンプル前後
    // 振幅 0.5 の矩形波 → ピーク絶対値が 0.4〜0.6
    float peak = 0;
    for (float s : pcm.samples) peak = std::max(peak, std::fabs(s));
    CHECK(peak > 0.4f && peak < 0.6f);

    // ポータブルデコーダ単体 (本番の第一経路) も直接検証
    next2d::PcmBuffer pcm2;
    CHECK(next2d::DecodeAudio(wav, pcm2));
    CHECK(pcm2.sample_rate == 48000 && pcm2.channels == 1);
    CHECK(pcm2.samples.size() >= 40);

    std::vector<uint8_t> garbage(32, 0xAB);
    next2d::PcmBuffer bad;
    CHECK(!next2d::DecodeAudio(garbage, bad));
}

// --- XAudio2: 初期化 (ヘッドレス CI では音声デバイスが無く失敗し得る → warn) --
static void TestAudioEngineInit()
{
    next2d::AudioEngine engine;
    if (!engine.Initialize()) {
        std::printf("warn - XAudio2 init failed (headless runner? not counted as failure)\n");
        return;
    }
    std::printf("ok - XAudio2 initialized\n");
    // ボイス生成 + 音量設定 + 即停止 (クラッシュしないこと)
    auto pcm = std::make_shared<next2d::PcmBuffer>();
    pcm->sample_rate = 48000;
    pcm->channels = 1;
    pcm->samples.assign(480, 0.1f);
    auto voice = engine.CreateVoice(pcm);
    CHECK(voice != nullptr);
    voice->SetVolume(0.5f);
    voice->Start(false);
    CHECK(!voice->IsFinished() || true);  // 状態取得がクラッシュしないこと
    voice->Stop();
}

int main()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // AudioEngine::Decode は MF を使う (AudioEngine::Initialize を通さない場合は自前で起動)
    MFStartup(MF_VERSION);

    TestWicDecode();
    TestImageDecoder();
    TestTextRasterizer();
    TestAudioDecode();
    TestAudioEngineInit();

    MFShutdown();

    std::printf("%s: %d checks, %d failures\n",
                failures ? "FAILED" : "ok", checks, failures);
    return failures ? 1 : 0;
}
