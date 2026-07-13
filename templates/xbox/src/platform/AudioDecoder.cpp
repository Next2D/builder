#include "AudioDecoder.h"

// dr_mp3 / dr_wav / stb_vorbis: public domain のヘッダオンリーデコーダ (third_party/)。
// メモリ入力のみ使用する。実装は本 TU に集約する (ODR 一意)。
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "third_party/dr_mp3.h"

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "third_party/dr_wav.h"

// stb_vorbis は .c 配布のためここで取り込む (STB_VORBIS_HEADER_ONLY は使わない)。
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "third_party/stb_vorbis.c"

#include <cstring>

namespace next2d {

namespace {

bool DecodeMp3(const std::vector<uint8_t>& input, PcmBuffer& out)
{
    drmp3_config config = {};
    drmp3_uint64 total_frames = 0;
    float* frames = drmp3_open_memory_and_read_pcm_frames_f32(
        input.data(), input.size(), &config, &total_frames, nullptr);
    if (!frames || total_frames == 0 || config.channels == 0) {
        if (frames) {
            drmp3_free(frames, nullptr);
        }
        return false;
    }
    out.channels    = config.channels;
    out.sample_rate = config.sampleRate;
    out.samples.assign(frames, frames + total_frames * config.channels);
    drmp3_free(frames, nullptr);
    return true;
}

bool DecodeWav(const std::vector<uint8_t>& input, PcmBuffer& out)
{
    unsigned int channels = 0, sample_rate = 0;
    drwav_uint64 total_frames = 0;
    float* frames = drwav_open_memory_and_read_pcm_frames_f32(
        input.data(), input.size(), &channels, &sample_rate, &total_frames, nullptr);
    if (!frames || total_frames == 0 || channels == 0) {
        if (frames) {
            drwav_free(frames, nullptr);
        }
        return false;
    }
    out.channels    = channels;
    out.sample_rate = sample_rate;
    out.samples.assign(frames, frames + total_frames * channels);
    drwav_free(frames, nullptr);
    return true;
}

bool DecodeOgg(const std::vector<uint8_t>& input, PcmBuffer& out)
{
    int err = 0;
    stb_vorbis* vorbis = stb_vorbis_open_memory(
        input.data(), static_cast<int>(input.size()), &err, nullptr);
    if (!vorbis) {
        return false;
    }
    const stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    const unsigned int total_frames = stb_vorbis_stream_length_in_samples(vorbis);
    if (info.channels <= 0 || total_frames == 0) {
        stb_vorbis_close(vorbis);
        return false;
    }
    out.channels    = static_cast<uint32_t>(info.channels);
    out.sample_rate = info.sample_rate;
    out.samples.resize(static_cast<size_t>(total_frames) * info.channels);
    const int read = stb_vorbis_get_samples_float_interleaved(
        vorbis, info.channels, out.samples.data(),
        static_cast<int>(out.samples.size()));
    stb_vorbis_close(vorbis);
    if (read <= 0) {
        out.samples.clear();
        return false;
    }
    out.samples.resize(static_cast<size_t>(read) * info.channels);
    return true;
}

} // namespace

bool DecodeAudio(const std::vector<uint8_t>& input, PcmBuffer& out)
{
    if (input.size() < 12) {
        return false;
    }
    const uint8_t* d = input.data();

    // マジックナンバーで形式を判定して該当デコーダを先に試し、
    // 外れたら残りをカスケード (誤判定・ヘッダ無し MP3 の保険)。
    const bool looks_wav = std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d + 8, "WAVE", 4) == 0;
    const bool looks_ogg = std::memcmp(d, "OggS", 4) == 0;
    const bool looks_mp3 = std::memcmp(d, "ID3", 3) == 0
        || (d[0] == 0xFF && (d[1] & 0xE0) == 0xE0);

    if (looks_wav && DecodeWav(input, out)) {
        return true;
    }
    if (looks_ogg && DecodeOgg(input, out)) {
        return true;
    }
    if (looks_mp3 && DecodeMp3(input, out)) {
        return true;
    }

    // 判定が外れた場合のカスケード
    return DecodeWav(input, out) || DecodeOgg(input, out) || DecodeMp3(input, out);
}

} // namespace next2d
