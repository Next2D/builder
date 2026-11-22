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
#include <mutex>
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

// 少しずつデコードされて増えていく PCM (BGM のような長い音源用)。
// 全体をデコードし終えるのを待たずに、先頭ができた時点で鳴らし始めるために使う。
//   - データは同じ大きさのブロックの並びで持つ。ブロックは確保したら動かさないので、
//     デコードする側が続きを書いている間も、再生する側は確定済みの範囲を読める。
//   - frames (確定済みのフレーム数) / complete / blocks の並びは mutex で保護する。
//     ブロックの中身は、frames より前の範囲であれば mutex なしで読んでよい。
struct PcmStream {
    static constexpr size_t kBlockFrames = 32768;   // 44.1 kHz で約 0.74 秒

    std::mutex mutex;
    std::vector<std::shared_ptr<std::vector<float>>> blocks;   // 各 kBlockFrames × channels 個
    uint64_t frames = 0;        // 確定済みのフレーム数
    bool complete = false;      // 最後までデコードし終えた (これ以上増えない)
    // 全体のフレーム数の見込み (ファイルに記録があるとき)。0 = 不明
    uint64_t expected_frames = 0;
};

// 32bit float インターリーブ PCM
struct PcmBuffer {
    uint32_t sample_rate = 48000;
    uint32_t channels = 2;
    std::vector<float> samples;  // interleaved
    // 少しずつ増える音源のときに設定する。設定されていれば samples は使わない。
    std::shared_ptr<PcmStream> stream;
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

    // 少しずつ増える音源 (PcmBuffer::stream) の、新しくできた分を出力へ回す。毎フレーム呼ぶ
    // (AudioEngine::Pump から)。ソフトウェアミキサーは合成のときに直接読むので何もしない。
    void Pump();

private:
    std::shared_ptr<PcmBuffer> buffer_;
    bool started_ = false;
    bool looping_ = false;

#if NEXT2D_AUDIO_XAUDIO2
    IXAudio2SourceVoice* voice_ = nullptr;
    // 少しずつ増える音源: 次に出力へ渡すブロックの番号と、渡したブロックの控え
    // (出力が読み終えるまでメモリを保つ)。
    size_t next_block_ = 0;
    std::vector<std::shared_ptr<std::vector<float>>> stream_blocks_;
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

    // 少しずつ増える音源を再生しているボイスへ、新しくできた分を回す。メインスレッドから毎フレーム呼ぶ。
    void Pump();

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
    // 少しずつ増える音源を再生するボイス (Pump の対象)。メインスレッドからだけ触る。
    std::vector<std::weak_ptr<AudioVoice>> stream_voices_;

#if NEXT2D_AUDIO_XAUDIO2
    IXAudio2* xaudio_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
#else
    std::shared_ptr<AudioMixerState> mixer_;
#endif
};

} // namespace next2d
