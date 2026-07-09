#include "AudioEngine.h"
#include "AudioDecoder.h"

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

AudioVoice::AudioVoice(IXAudio2* xaudio, std::shared_ptr<PcmBuffer> buffer)
    : buffer_(std::move(buffer))
{
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
    return std::make_shared<AudioVoice>(xaudio_, std::move(buffer));
}

} // namespace next2d
