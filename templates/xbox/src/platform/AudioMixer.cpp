// AudioEngine のソフトウェアミキサー実装 (AudioEngine.h を参照)。
// 再生中のボイスを 1 本の PCM に合成するだけで、OS の音声 API は使わない。
// 出力は呼び出し側が Render() で受け取って行う。
#include "AudioEngine.h"
#include "AudioDecoder.h"

#if !NEXT2D_AUDIO_XAUDIO2

#include <algorithm>
#include <cmath>
#include <mutex>

namespace next2d {

// ボイス 1 本の再生状態。すべて AudioMixerState::mutex で保護する。
struct AudioMixerVoice {
    std::shared_ptr<PcmBuffer> buffer;
    bool playing = false;     // Start 済みで、まだ止まっていない
    bool looping = false;
    bool finished = false;    // 非ループ再生が最後まで進んだ
    double position = 0.0;    // 再生位置 (元データのフレーム単位)
    float volume = 1.0f;
    // 少しずつ増える音源: 確定済みのブロックの控え (合成のたびに、増えた分だけ足す)
    std::vector<std::shared_ptr<std::vector<float>>> blocks;
};

struct AudioMixerState {
    std::mutex mutex;
    std::vector<std::weak_ptr<AudioMixerVoice>> voices;
    // 一度でも Render が呼ばれたか。出力先がつながっていない環境では再生が進まないので、
    // その場合は「再生開始 → 即終了」として扱う (終了待ちのアプリが止まらないように)。
    bool has_consumer = false;
};

AudioVoice::AudioVoice(AudioEngine* engine, std::shared_ptr<PcmBuffer> buffer)
    : buffer_(std::move(buffer))
{
    mixer_ = engine ? engine->mixer() : nullptr;
    if (!mixer_ || !buffer_) {
        return;
    }
    mixer_voice_ = std::make_shared<AudioMixerVoice>();
    mixer_voice_->buffer = buffer_;
    std::lock_guard<std::mutex> lock(mixer_->mutex);
    mixer_->voices.push_back(mixer_voice_);
}

AudioVoice::~AudioVoice()
{
    if (mixer_ && mixer_voice_) {
        std::lock_guard<std::mutex> lock(mixer_->mutex);
        mixer_voice_->playing = false;
    }
}

void AudioVoice::Start(bool loop)
{
    started_ = true;
    looping_ = loop;
    if (!mixer_ || !mixer_voice_) {
        return;
    }
    std::lock_guard<std::mutex> lock(mixer_->mutex);
    mixer_voice_->position = 0.0;
    mixer_voice_->looping = loop;
    mixer_voice_->finished = false;
    mixer_voice_->playing = true;
}

void AudioVoice::Stop()
{
    started_ = false;
    if (!mixer_ || !mixer_voice_) {
        return;
    }
    std::lock_guard<std::mutex> lock(mixer_->mutex);
    mixer_voice_->playing = false;
}

bool AudioVoice::IsFinished() const
{
    if (!started_ || looping_) {
        return false;
    }
    if (!mixer_ || !mixer_voice_) {
        return true;
    }
    std::lock_guard<std::mutex> lock(mixer_->mutex);
    return mixer_voice_->finished || !mixer_->has_consumer;
}

void AudioVoice::SetVolume(float v)
{
    if (!mixer_ || !mixer_voice_) {
        return;
    }
    std::lock_guard<std::mutex> lock(mixer_->mutex);
    mixer_voice_->volume = v;
}

void AudioVoice::Pump()
{
    // 合成 (Render) のときに、確定済みの範囲を直接読む
}

void AudioEngine::Pump()
{
}

AudioEngine::AudioEngine()
    : mixer_(std::make_shared<AudioMixerState>())
{
}

AudioEngine::~AudioEngine() = default;

bool AudioEngine::Initialize()
{
    return true;
}

bool AudioEngine::Decode(const std::vector<uint8_t>& input, PcmBuffer& out)
{
    // 可搬なデコーダ (dr_mp3 / dr_wav / stb_vorbis) のみ。扱えない形式は失敗とし、
    // 呼び出し側が無音バッファで進行する。
    return DecodeAudio(input, out);
}

std::shared_ptr<AudioVoice> AudioEngine::CreateVoice(std::shared_ptr<PcmBuffer> buffer)
{
    return std::make_shared<AudioVoice>(this, std::move(buffer));
}

bool AudioEngine::Render(float* out, int frames, int channels, int sample_rate)
{
    if (!out || frames <= 0 || channels <= 0 || sample_rate <= 0) {
        return false;
    }
    std::fill(out, out + static_cast<size_t>(frames) * channels, 0.0f);

    std::lock_guard<std::mutex> lock(mixer_->mutex);
    mixer_->has_consumer = true;

    auto& voices = mixer_->voices;
    for (size_t v = 0; v < voices.size();) {
        const std::shared_ptr<AudioMixerVoice> voice = voices[v].lock();
        if (!voice) {
            voices.erase(voices.begin() + static_cast<std::ptrdiff_t>(v));
            continue;
        }
        ++v;
        if (!voice->playing || !voice->buffer) {
            continue;
        }

        const PcmBuffer& pcm = *voice->buffer;
        const int src_channels = static_cast<int>(pcm.channels);

        // 少しずつ増える音源は、いま確定している範囲だけを読む。まだ増える (complete でない)
        // 間は、末尾に着いても終わりにせず、続きができるのを待つ。
        size_t src_frames = src_channels > 0 ? pcm.samples.size() / src_channels : 0;
        bool complete = true;
        if (pcm.stream) {
            std::lock_guard<std::mutex> stream_lock(pcm.stream->mutex);
            src_frames = static_cast<size_t>(pcm.stream->frames);
            complete = pcm.stream->complete;
            for (size_t b = voice->blocks.size(); b < pcm.stream->blocks.size(); ++b) {
                voice->blocks.push_back(pcm.stream->blocks[b]);
            }
        }
        if (pcm.sample_rate == 0 || src_channels <= 0 || (src_frames == 0 && complete)) {
            // 空のバッファ (デコード失敗時の無音) は即終了
            voice->playing = false;
            voice->finished = true;
            continue;
        }

        const double step = static_cast<double>(pcm.sample_rate) / sample_rate;
        const float* src = pcm.samples.data();
        const bool streamed = static_cast<bool>(pcm.stream);
        const auto& blocks = voice->blocks;
        // フレーム i の先頭 (チャンネル 0) の位置
        const auto frame = [&](size_t i) -> const float* {
            return streamed
                ? blocks[i / PcmStream::kBlockFrames]->data() + (i % PcmStream::kBlockFrames) * src_channels
                : src + i * src_channels;
        };
        for (int f = 0; f < frames; ++f) {
            if (voice->position >= static_cast<double>(src_frames)) {
                if (!complete) {
                    break;   // 続きがまだできていない: この先は無音にして待つ
                }
                if (voice->looping) {
                    voice->position = std::fmod(voice->position, static_cast<double>(src_frames));
                } else {
                    voice->playing = false;
                    voice->finished = true;
                    break;
                }
            }
            const size_t i0 = static_cast<size_t>(voice->position);
            // 線形補間。末尾の次は、ループ時は先頭、非ループ時 (と、続きを待っている間) は末尾の値を使う
            const size_t i1 = (i0 + 1 < src_frames) ? i0 + 1 : (voice->looping && complete ? 0 : i0);
            const float t = static_cast<float>(voice->position - static_cast<double>(i0));
            const float* s0 = frame(i0);
            const float* s1 = frame(i1);

            float* dst = out + static_cast<size_t>(f) * channels;
            for (int c = 0; c < channels; ++c) {
                float sample = 0.0f;
                if (src_channels == 1) {
                    sample = s0[0] + (s1[0] - s0[0]) * t;
                } else if (channels == 1) {
                    // 多チャンネル → モノラル: 左右の平均
                    const float a = (s0[0] + s0[1]) * 0.5f;
                    const float b = (s1[0] + s1[1]) * 0.5f;
                    sample = a + (b - a) * t;
                } else {
                    // 出力チャンネルが元より多ければ、余りは無音にする
                    if (c < src_channels) {
                        const float a = s0[c];
                        const float b = s1[c];
                        sample = a + (b - a) * t;
                    }
                }
                dst[c] += sample * voice->volume;
            }
            voice->position += step;
        }
    }

    // 合成結果を [-1, 1] に収める
    const size_t total = static_cast<size_t>(frames) * channels;
    for (size_t i = 0; i < total; ++i) {
        out[i] = std::max(-1.0f, std::min(1.0f, out[i]));
    }
    return true;
}

} // namespace next2d

#endif // !NEXT2D_AUDIO_XAUDIO2
