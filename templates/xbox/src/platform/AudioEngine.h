// AudioEngine: XAudio2 のマスタリングボイスを保持し、PCM バッファを再生する。
// WebAudio の最小サブセット (decode 済み PCM の play/stop/volume/loop) を支える。
//
// NOTE: 完全な WebAudio グラフ (AnalyserNode, フィルタ, パンナー等) は未対応。
//       Next2D の効果音/BGM 再生に必要な範囲を実装する。拡張は README 参照。
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

namespace next2d {

// 32bit float インターリーブ PCM
struct PcmBuffer {
    uint32_t sample_rate = 48000;
    uint32_t channels = 2;
    std::vector<float> samples;  // interleaved
};

// 単一の再生ボイス。start/stop/volume を提供する。実装は AudioEngine.cpp。
class AudioVoice {
public:
    AudioVoice(IXAudio2* xaudio, std::shared_ptr<PcmBuffer> buffer);
    ~AudioVoice();

    void Start(bool loop);
    void Stop();
    void SetVolume(float v);

    // 非ループ再生でキューが空になった (= 再生終了) かどうか。"ended" 発火判定に使う。
    bool IsFinished() const;

private:
    std::shared_ptr<PcmBuffer> buffer_;
    IXAudio2SourceVoice* voice_ = nullptr;
    bool started_ = false;
    bool looping_ = false;
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    bool Initialize();

    // Media Foundation で圧縮音声(mp3/aac/wav 等)を PCM float へデコードする。
    static bool Decode(const std::vector<uint8_t>& input, PcmBuffer& out);

    // 再生ボイスを生成する。呼び出し側が寿命を管理する。
    std::shared_ptr<AudioVoice> CreateVoice(std::shared_ptr<PcmBuffer> buffer);

    IXAudio2* xaudio() const { return xaudio_; }

private:
    IXAudio2* xaudio_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
};

} // namespace next2d
