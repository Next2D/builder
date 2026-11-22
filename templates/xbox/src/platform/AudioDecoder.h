// 音声デコード (エンコード済みバイト列 -> float32 interleaved PCM)。
//
// 第一候補は dr_mp3 / dr_wav / stb_vorbis (MP3/WAV/OGG、全プラットフォーム共通、
// public domain)。コンソール (Game Core OS) にはデスクトップの Media Foundation が
// 無いため、MF 依存を第一経路から外して PC 検証と実機の挙動を一致させる。
// デスクトップのみ AAC 等の保険として AudioEngine::Decode 側に MF フォールバックが残る。
#pragma once

#include "AudioEngine.h"   // PcmBuffer

#include <cstdint>
#include <istream>
#include <memory>
#include <vector>

namespace next2d {

bool DecodeAudio(const std::vector<uint8_t>& input, PcmBuffer& out);

// 音源を少しずつ読んでデコードする (BGM のような長い音源を、全体のデコードを待たずに
// 先頭ができた時点で鳴らし始めるため)。デコードした分は PcmBuffer::stream へ足していく。
// 対応する形式は MP3。それ以外は Open が nullptr を返すので、DecodeAudio で全体をデコードする。
class AudioStreamDecoder {
public:
    // input: 読み取りと位置の移動ができるストリーム。少しずつデコードできる形式でなければ nullptr。
    // 成功したら out の sample_rate / channels / stream を設定する (samples は使わない)。
    static std::unique_ptr<AudioStreamDecoder> Open(std::unique_ptr<std::istream> input, PcmBuffer& out);

    ~AudioStreamDecoder();

    // max_frames フレームを目安に続きをデコードして足す。まだ続きがあれば true。
    // 最後まで終えたら stream の complete を立てて false を返す。
    bool DecodeSome(uint64_t max_frames);

private:
    AudioStreamDecoder();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace next2d
