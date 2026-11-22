// AudioEngine の XAudio2 実装 (AudioEngine.h を参照)。
#include "AudioEngine.h"
#include "AudioDecoder.h"

#if NEXT2D_AUDIO_XAUDIO2

#include <xaudio2.h>
#include <iostream>

// Media Foundation / shlwapi はコンソール (Game Core OS) に無いデスクトップ専用 API。
// 第一経路は AudioDecoder (dr_mp3/dr_wav/stb_vorbis) で、MF は AAC 等の
// デスクトップ限定フォールバックのみ。XAudio2 は両プラットフォームで使える。
#if !NEXT2D_XBOX_CONSOLE
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>
#include <wrl/client.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shlwapi.lib")

using Microsoft::WRL::ComPtr;
#endif

namespace next2d {

AudioVoice::AudioVoice(AudioEngine* engine, std::shared_ptr<PcmBuffer> buffer)
    : buffer_(std::move(buffer))
{
    IXAudio2* xaudio = engine ? engine->xaudio() : nullptr;
    if (!xaudio || !buffer_) {
        return;   // 音声デバイス無し: 再生開始と同時に終了として扱う
    }
    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels = static_cast<WORD>(buffer_->channels);
    wfx.nSamplesPerSec = buffer_->sample_rate;
    wfx.wBitsPerSample = 32;
    wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    xaudio->CreateSourceVoice(&voice_, &wfx);
}

AudioVoice::~AudioVoice()
{
    if (voice_) {
        voice_->Stop();
        voice_->DestroyVoice();
    }
}

void AudioVoice::Start(bool loop)
{
    // 音声デバイスが無い環境 (CI 等) でも「再生開始→即終了」として扱えるよう、
    // voice_ の有無に関わらず開始状態は記録する (IsFinished が ended 発火を導く)。
    started_ = true;
    looping_ = loop;
    if (!voice_) {
        return;
    }
    if (buffer_->stream) {
        // 少しずつ増える音源: できているブロックから順に渡す (続きは Pump で渡す)
        next_block_ = 0;
        voice_->Start(0);
        Pump();
        return;
    }
    XAUDIO2_BUFFER buf = {};
    buf.AudioBytes = static_cast<UINT32>(buffer_->samples.size() * sizeof(float));
    buf.pAudioData = reinterpret_cast<const BYTE*>(buffer_->samples.data());
    buf.Flags = XAUDIO2_END_OF_STREAM;
    buf.LoopCount = loop ? XAUDIO2_LOOP_INFINITE : 0;
    voice_->SubmitSourceBuffer(&buf);
    voice_->Start(0);
}

void AudioVoice::Stop()
{
    if (voice_) {
        voice_->Stop();
        voice_->FlushSourceBuffers();
    }
    started_ = false;
    next_block_ = 0;
}

// 少しずつ増える音源のブロックを、出力の待ち行列へ渡す。
//   - 渡すのは中身が確定したブロックだけ: いっぱいになったブロックと、デコードが終わった後の
//     最後のブロック (デコードする側が続きを書いている途中のブロックは渡さない)。
//   - 待ち行列には数秒ぶんだけ入れておく (XAudio2 の待ち行列は 64 個まで)。
//   - ループ再生は、最後まで渡し終えたら先頭のブロックへ戻る。
void AudioVoice::Pump()
{
    if (!voice_ || !started_ || !buffer_ || !buffer_->stream) {
        return;
    }
    const size_t kQueuedBlocks = 8;

    uint64_t frames = 0;
    bool complete = false;
    {
        std::lock_guard<std::mutex> lock(buffer_->stream->mutex);
        frames = buffer_->stream->frames;
        complete = buffer_->stream->complete;
        for (size_t b = stream_blocks_.size(); b < buffer_->stream->blocks.size(); ++b) {
            stream_blocks_.push_back(buffer_->stream->blocks[b]);
        }
    }
    const size_t full_blocks = static_cast<size_t>(frames / PcmStream::kBlockFrames);
    const size_t tail_frames = static_cast<size_t>(frames % PcmStream::kBlockFrames);
    // 渡せるブロックの数
    const size_t ready_blocks = full_blocks + (complete && tail_frames > 0 ? 1 : 0);

    XAUDIO2_VOICE_STATE state = {};
    voice_->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    size_t queued = state.BuffersQueued;
    while (queued < kQueuedBlocks) {
        if (next_block_ >= ready_blocks) {
            if (!complete || !looping_ || ready_blocks == 0) {
                break;   // 続きを待つ / 最後まで渡した
            }
            next_block_ = 0;
        }
        const size_t block_frames = next_block_ < full_blocks ? PcmStream::kBlockFrames : tail_frames;
        XAUDIO2_BUFFER buf = {};
        buf.AudioBytes = static_cast<UINT32>(block_frames * buffer_->channels * sizeof(float));
        buf.pAudioData = reinterpret_cast<const BYTE*>(stream_blocks_[next_block_]->data());
        if (complete && !looping_ && next_block_ + 1 == ready_blocks) {
            buf.Flags = XAUDIO2_END_OF_STREAM;
        }
        if (FAILED(voice_->SubmitSourceBuffer(&buf))) {
            break;
        }
        ++next_block_;
        ++queued;
    }
}

bool AudioVoice::IsFinished() const
{
    if (!started_ || looping_) {
        return false;
    }
    if (!voice_) {
        // 実ボイスが無い (音声デバイス無し) 場合は開始と同時に終了扱い。
        return true;
    }
    XAUDIO2_VOICE_STATE state = {};
    voice_->GetState(&state);
    if (buffer_ && buffer_->stream) {
        // 少しずつ増える音源: 最後まで渡し終えて、出力も読み終えたら終了
        uint64_t frames = 0;
        bool complete = false;
        {
            std::lock_guard<std::mutex> lock(buffer_->stream->mutex);
            frames = buffer_->stream->frames;
            complete = buffer_->stream->complete;
        }
        const size_t blocks = static_cast<size_t>(
            (frames + PcmStream::kBlockFrames - 1) / PcmStream::kBlockFrames);
        return complete && next_block_ >= blocks && state.BuffersQueued == 0;
    }
    return state.BuffersQueued == 0;
}

void AudioVoice::SetVolume(float v)
{
    if (voice_) {
        voice_->SetVolume(v);
    }
}

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine()
{
    if (master_) {
        master_->DestroyVoice();
    }
    if (xaudio_) {
        xaudio_->Release();
    }
#if !NEXT2D_XBOX_CONSOLE
    MFShutdown();
#endif
}

bool AudioEngine::Initialize()
{
#if !NEXT2D_XBOX_CONSOLE
    if (FAILED(MFStartup(MF_VERSION))) {
        std::cerr << "[Audio] MFStartup failed" << std::endl;
        return false;
    }
#endif
    if (FAILED(XAudio2Create(&xaudio_, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
        std::cerr << "[Audio] XAudio2Create failed" << std::endl;
        return false;
    }
    if (FAILED(xaudio_->CreateMasteringVoice(&master_))) {
        std::cerr << "[Audio] CreateMasteringVoice failed" << std::endl;
        return false;
    }
    return true;
}

bool AudioEngine::Decode(const std::vector<uint8_t>& input, PcmBuffer& out)
{
    // 第一候補: ポータブルデコーダ (dr_mp3/dr_wav/stb_vorbis)。
    // コンソール (Game Core) には Media Foundation が無いため、共通経路を先に
    // 通すことで PC 検証と実機の挙動を一致させる。
    if (DecodeAudio(input, out)) {
        return true;
    }

#if NEXT2D_XBOX_CONSOLE
    // コンソール: MF が無いため共通デコーダで扱えない形式は失敗とする
    // (呼び出し側の graceful degradation = 無音バッファで進行)。
    return false;
#else
    // フォールバック (デスクトップのみ): Media Foundation の SourceReader。
    // AAC/WMA 等、MP3/WAV/OGG 以外の形式への保険。
    // メモリストリームから 32bit float PCM へ変換して読み出す。
    ComPtr<IStream> stream =
        SHCreateMemStream(input.data(), static_cast<UINT>(input.size()));
    if (!stream) {
        return false;
    }

    ComPtr<IMFByteStream> byte_stream;
    if (FAILED(MFCreateMFByteStreamOnStream(stream.Get(), &byte_stream))) {
        return false;
    }

    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromByteStream(byte_stream.Get(), nullptr, &reader))) {
        return false;
    }

    // 出力を float PCM に指定
    ComPtr<IMFMediaType> pcm_type;
    MFCreateMediaType(&pcm_type);
    pcm_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pcm_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    if (FAILED(reader->SetCurrentMediaType(
            MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, pcm_type.Get()))) {
        return false;
    }

    ComPtr<IMFMediaType> actual;
    reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual);
    UINT32 channels = 2, sample_rate = 48000;
    actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sample_rate);
    out.channels = channels;
    out.sample_rate = sample_rate;

    for (;;) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
                                      nullptr, &flags, nullptr, &sample))) {
            return false;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            break;
        }
        if (!sample) {
            continue;
        }

        ComPtr<IMFMediaBuffer> media_buffer;
        sample->ConvertToContiguousBuffer(&media_buffer);
        BYTE* data = nullptr;
        DWORD length = 0;
        media_buffer->Lock(&data, nullptr, &length);
        const float* f = reinterpret_cast<const float*>(data);
        out.samples.insert(out.samples.end(), f, f + length / sizeof(float));
        media_buffer->Unlock();
    }

    return !out.samples.empty();
#endif // NEXT2D_XBOX_CONSOLE
}

std::shared_ptr<AudioVoice> AudioEngine::CreateVoice(std::shared_ptr<PcmBuffer> buffer)
{
    const bool streamed = buffer && buffer->stream;
    auto voice = std::make_shared<AudioVoice>(this, std::move(buffer));
    if (streamed) {
        stream_voices_.push_back(voice);
    }
    return voice;
}

void AudioEngine::Pump()
{
    for (size_t i = 0; i < stream_voices_.size();) {
        if (const std::shared_ptr<AudioVoice> voice = stream_voices_[i].lock()) {
            voice->Pump();
            ++i;
        } else {
            stream_voices_.erase(stream_voices_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}

bool AudioEngine::Render(float*, int, int, int)
{
    return false;   // XAudio2 が自分で出力する
}

} // namespace next2d

#endif // NEXT2D_AUDIO_XAUDIO2
