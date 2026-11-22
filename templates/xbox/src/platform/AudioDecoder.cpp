#include "AudioDecoder.h"

// dr_mp3 / dr_wav / stb_vorbis: public domain のヘッダオンリーデコーダ (third_party/)。
// ファイル入出力の機能は使わない (入力はメモリか、呼び出し側が渡すストリーム)。
// 実装は本 TU に集約する (ODR 一意)。
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

// ---- 少しずつデコードする ---------------------------------------------------

struct AudioStreamDecoder::Impl {
    drmp3 mp3 = {};
    bool opened = false;
    std::unique_ptr<std::istream> input;
    std::shared_ptr<PcmStream> stream;
    uint32_t channels = 0;
    size_t tail_frames = 0;                       // 最後のブロックに入っているフレーム数
    std::shared_ptr<std::vector<float>> tail;     // 書き込み中のブロック

    static size_t Read(void* user, void* out, size_t bytes)
    {
        std::istream& in = *static_cast<Impl*>(user)->input;
        in.read(static_cast<char*>(out), static_cast<std::streamsize>(bytes));
        const size_t read = static_cast<size_t>(in.gcount());
        if (!in) {
            in.clear();   // 末尾まで読んだ後も、位置の移動を続けられるようにする
        }
        return read;
    }

    static drmp3_bool32 Seek(void* user, int offset, drmp3_seek_origin origin)
    {
        std::istream& in = *static_cast<Impl*>(user)->input;
        in.clear();
        in.seekg(offset, origin == DRMP3_SEEK_SET ? std::ios::beg
                        : origin == DRMP3_SEEK_CUR ? std::ios::cur : std::ios::end);
        if (!in) {
            in.clear();
            return DRMP3_FALSE;
        }
        return DRMP3_TRUE;
    }

    static drmp3_bool32 Tell(void* user, drmp3_int64* cursor)
    {
        std::istream& in = *static_cast<Impl*>(user)->input;
        const std::streamoff position = in.tellg();
        if (position < 0) {
            in.clear();
            return DRMP3_FALSE;
        }
        *cursor = static_cast<drmp3_int64>(position);
        return DRMP3_TRUE;
    }
};

AudioStreamDecoder::AudioStreamDecoder()
    : impl_(std::make_unique<Impl>())
{
}

AudioStreamDecoder::~AudioStreamDecoder()
{
    if (impl_->opened) {
        drmp3_uninit(&impl_->mp3);
    }
}

std::unique_ptr<AudioStreamDecoder> AudioStreamDecoder::Open(std::unique_ptr<std::istream> input, PcmBuffer& out)
{
    if (!input || !*input) {
        return nullptr;
    }

    // MP3 だけを扱う: 先頭が ID3 タグか、フレームの同期語。それ以外 (WAV / OGG など) は
    // 呼び出し側が全体をデコードする。
    unsigned char head[4] = {};
    input->read(reinterpret_cast<char*>(head), sizeof(head));
    const bool enough = input->gcount() == static_cast<std::streamsize>(sizeof(head));
    input->clear();
    input->seekg(0, std::ios::beg);
    if (!enough || !*input) {
        return nullptr;
    }
    const bool looks_mp3 = std::memcmp(head, "ID3", 3) == 0
        || (head[0] == 0xFF && (head[1] & 0xE0) == 0xE0);
    if (!looks_mp3) {
        return nullptr;
    }

    std::unique_ptr<AudioStreamDecoder> decoder(new AudioStreamDecoder());
    Impl& impl = *decoder->impl_;
    impl.input = std::move(input);
    if (!drmp3_init(&impl.mp3, Impl::Read, Impl::Seek, Impl::Tell, nullptr, &impl, nullptr)) {
        return nullptr;
    }
    impl.opened = true;
    if (impl.mp3.channels == 0 || impl.mp3.sampleRate == 0) {
        return nullptr;
    }

    impl.channels = impl.mp3.channels;
    impl.stream = std::make_shared<PcmStream>();
    if (impl.mp3.totalPCMFrameCount != DRMP3_UINT64_MAX) {
        impl.stream->expected_frames = impl.mp3.totalPCMFrameCount;
    }
    out.channels = impl.mp3.channels;
    out.sample_rate = impl.mp3.sampleRate;
    out.samples.clear();
    out.stream = impl.stream;
    return decoder;
}

bool AudioStreamDecoder::DecodeSome(uint64_t max_frames)
{
    Impl& impl = *impl_;
    uint64_t produced = 0;
    while (produced < max_frames) {
        if (!impl.tail || impl.tail_frames == PcmStream::kBlockFrames) {
            // 新しいブロック。確保してから並びへ足す (並びの変更だけを mutex の中で行う)
            impl.tail = std::make_shared<std::vector<float>>(PcmStream::kBlockFrames * impl.channels);
            impl.tail_frames = 0;
            std::lock_guard<std::mutex> lock(impl.stream->mutex);
            impl.stream->blocks.push_back(impl.tail);
        }

        // 確定済みの範囲 (frames) より後ろへ書くので、再生する側とは重ならない
        const drmp3_uint64 read = drmp3_read_pcm_frames_f32(
            &impl.mp3, PcmStream::kBlockFrames - impl.tail_frames,
            impl.tail->data() + impl.tail_frames * impl.channels);

        std::lock_guard<std::mutex> lock(impl.stream->mutex);
        if (read == 0) {
            impl.stream->complete = true;
            return false;
        }
        impl.tail_frames += static_cast<size_t>(read);
        impl.stream->frames += read;
        produced += read;
    }
    return true;
}

} // namespace next2d
