// AudioEngine: デコード済み PCM を再生する。
// WebAudio の最小サブセット (decode 済み PCM の play/stop/volume/loop) を支える。
//
// 実装は 2 つあり、ビルド時に選ぶ:
//   - XAudio2 (AudioEngine.cpp): Windows の既定。エンジンが自分で音声デバイスへ出力する。
//   - ソフトウェアミキサー (AudioMixer.cpp): Windows 以外、または NEXT2D_AUDIO_MIXER=1。
//     エンジンは再生中のボイスを合成するだけで、出力は呼び出し側が Render() で
//     PCM を受け取って行う (音声出力の API をホスト本体の外に持つ構成用)。
//
// NOTE: 完全な WebAudio グラフ (AnalyserNode, フィルタ, パンナー等) は未対応。
//       Next2D の効果音/BGM 再生に必要な範囲を実装する。拡張は README 参照。
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#if defined(_WIN32) && !(defined(NEXT2D_AUDIO_MIXER) && NEXT2D_AUDIO_MIXER)
#define NEXT2D_AUDIO_XAUDIO2 1
#else
#define NEXT2D_AUDIO_XAUDIO2 0
#endif

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

namespace next2d {

class AudioEngine;
struct AudioMixerState;   // ソフトウェアミキサーの共有状態 (AudioMixer.cpp)
struct AudioMixerVoice;   // ミキサーが合成するボイス 1 本の状態 (AudioMixer.cpp)

// 32bit float インターリーブ PCM
struct PcmBuffer {
    uint32_t sample_rate = 48000;
    uint32_t channels = 2;
    std::vector<float> samples;  // interleaved
};

// 単一の再生ボイス。start/stop/volume を提供する。
class AudioVoice {
public:
    AudioVoice(AudioEngine* engine, std::shared_ptr<PcmBuffer> buffer);
    ~AudioVoice();

    void Start(bool loop);
    void Stop();
    void SetVolume(float v);

    // 非ループ再生が最後まで進んだ (= 再生終了) かどうか。"ended" 発火判定に使う。
    bool IsFinished() const;

private:
    std::shared_ptr<PcmBuffer> buffer_;
    bool started_ = false;
    bool looping_ = false;

#if NEXT2D_AUDIO_XAUDIO2
    IXAudio2SourceVoice* voice_ = nullptr;
#else
    // エンジンより長生きしても安全なよう、ミキサーの共有状態を自分でも保持する
    std::shared_ptr<AudioMixerState> mixer_;
    std::shared_ptr<AudioMixerVoice> mixer_voice_;
#endif
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    bool Initialize();

    // 圧縮音声 (mp3 / wav / ogg など) を PCM float へデコードする。
    static bool Decode(const std::vector<uint8_t>& input, PcmBuffer& out);

    // 再生ボイスを生成する。呼び出し側が寿命を管理する。
    std::shared_ptr<AudioVoice> CreateVoice(std::shared_ptr<PcmBuffer> buffer);

    // ミキサー実装のみ: 再生中のボイスを合成し、out (interleaved float、frames × channels 個)
    // へ書き込む。out は上書きされる。音声出力のスレッドなど、他のスレッドから呼んでよい。
    // 合成した場合 true。XAudio2 実装ではエンジンが自分で出力するので何もせず false。
    bool Render(float* out, int frames, int channels, int sample_rate);

#if NEXT2D_AUDIO_XAUDIO2
    IXAudio2* xaudio() const { return xaudio_; }
#else
    const std::shared_ptr<AudioMixerState>& mixer() const { return mixer_; }
#endif

private:
#if NEXT2D_AUDIO_XAUDIO2
    IXAudio2* xaudio_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
#else
    std::shared_ptr<AudioMixerState> mixer_;
#endif
};

} // namespace next2d
