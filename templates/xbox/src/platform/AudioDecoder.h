// 音声デコード (エンコード済みバイト列 -> float32 interleaved PCM)。
//
// 第一候補は dr_mp3 / dr_wav / stb_vorbis (MP3/WAV/OGG、全プラットフォーム共通、
// public domain)。コンソール (Game Core OS) にはデスクトップの Media Foundation が
// 無いため、MF 依存を第一経路から外して PC 検証と実機の挙動を一致させる。
// デスクトップのみ AAC 等の保険として AudioEngine::Decode 側に MF フォールバックが残る。
#pragma once

#include "AudioEngine.h"   // PcmBuffer

#include <cstdint>
#include <vector>

namespace next2d {

bool DecodeAudio(const std::vector<uint8_t>& input, PcmBuffer& out);

} // namespace next2d
