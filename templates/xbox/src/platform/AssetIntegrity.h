#pragma once

// ゲームのファイルが、ビルドした後に書き換えられていないかを確かめる。
//
// builder はビルド時に、動作を決めるファイル (JS / HTML / JSON / wasm) の SHA-256 を一覧にして
// ホストの実行ファイルへ組み込む。ホストはファイルを読むたびに照合し、合わないものは使わない。
// 一覧が設定されていないとき (builder を通さない開発用のビルド) は、何も確かめない。
//
// ヘッダだけで完結させている (ホストのソースを個別に列挙しているビルドに、ファイルを足さずに済むように)。

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>

namespace next2d {
namespace integrity {

// ---- SHA-256 (FIPS 180-4) ----
class Sha256 {
public:
    Sha256() { Reset(); }

    void Update(const uint8_t* data, size_t size)
    {
        total_ += size;
        while (size > 0) {
            const size_t take = (size < 64 - used_) ? size : 64 - used_;
            std::memcpy(block_ + used_, data, take);
            used_ += take;
            data += take;
            size -= take;
            if (used_ == 64) {
                Compress(block_);
                used_ = 0;
            }
        }
    }

    std::string FinishHex()
    {
        const uint64_t bits = total_ * 8;
        const uint8_t one = 0x80;
        const uint8_t zero = 0x00;
        Update(&one, 1);
        while (used_ != 56) {
            Update(&zero, 1);
        }
        uint8_t length[8];
        for (int i = 0; i < 8; ++i) {
            length[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        }
        Update(length, 8);

        static const char* const digits = "0123456789abcdef";
        std::string hex;
        hex.reserve(64);
        for (uint32_t word : state_) {
            for (int shift = 28; shift >= 0; shift -= 4) {
                hex.push_back(digits[(word >> shift) & 0xF]);
            }
        }
        return hex;
    }

private:
    void Reset()
    {
        state_ = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
        used_ = 0;
        total_ = 0;
    }

    static uint32_t Rotr(uint32_t value, int count)
    {
        return (value >> count) | (value << (32 - count));
    }

    void Compress(const uint8_t* block)
    {
        static const uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
        };
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = static_cast<uint32_t>(block[i * 4]) << 24 | static_cast<uint32_t>(block[i * 4 + 1]) << 16
                 | static_cast<uint32_t>(block[i * 4 + 2]) << 8 | static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = h + s1 + ch + k[i] + w[i];
            const uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = s0 + maj;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::array<uint32_t, 8> state_{};
    uint8_t block_[64]{};
    size_t used_ = 0;
    uint64_t total_ = 0;
};

inline std::string Sha256Hex(const uint8_t* data, size_t size)
{
    Sha256 hash;
    hash.Update(data, size);
    return hash.FinishHex();
}

// ---- 一覧 ----

// キー (assets/app からの相対パス、または js/bootstrap.js) -> SHA-256 (16 進 64 桁)
inline std::unordered_map<std::string, std::string>& Manifest()
{
    static std::unordered_map<std::string, std::string> manifest;
    return manifest;
}

inline bool& EnabledFlag()
{
    static bool enabled = false;
    return enabled;
}

// 一覧を設定する。形式は 1 行 1 ファイルで "<SHA-256 の 16 進 64 桁><空白 2 つ><キー>" (sha256sum と同じ)。
// ホストを作る前に 1 度だけ呼ぶ。空や null なら照合しない。
inline void SetManifest(const char* text)
{
    auto& manifest = Manifest();
    manifest.clear();
    EnabledFlag() = false;
    if (!text) {
        return;
    }
    const std::string all(text);
    size_t begin = 0;
    while (begin < all.size()) {
        size_t end = all.find('\n', begin);
        if (end == std::string::npos) {
            end = all.size();
        }
        std::string line = all.substr(begin, end - begin);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.size() > 66 && line[64] == ' ' && line[65] == ' ') {
            manifest[line.substr(66)] = line.substr(0, 64);
        }
        begin = end + 1;
    }
    EnabledFlag() = !manifest.empty();
}

inline bool Enabled()
{
    return EnabledFlag();
}

// 動作を決めるファイル (一覧に無ければ使わせないもの) かどうか。
inline bool IsCode(const std::string& key)
{
    const auto dot = key.rfind('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string extension = key.substr(dot);
    for (char& c : extension) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return extension == ".js" || extension == ".mjs" || extension == ".html"
        || extension == ".json" || extension == ".wasm";
}

// 読む前に全体の照合が要るファイルかどうか (少しずつ読む経路は、これが true のファイルに使わない)。
inline bool NeedsCheck(const std::string& key)
{
    return Enabled() && (Manifest().count(key) != 0 || IsCode(key));
}

// 読んだ内容を使ってよいか。一覧にあるファイルは一致が必要。一覧に無いファイルは、
// 動作を決めるもの (ビルドの後に足されたスクリプトなど) なら使わせない。
inline bool Verify(const std::string& key, const uint8_t* data, size_t size)
{
    if (!Enabled()) {
        return true;
    }
    const auto& manifest = Manifest();
    const auto found = manifest.find(key);
    if (found == manifest.end()) {
        if (!IsCode(key)) {
            return true;
        }
        std::cerr << "[HostCore] integrity check failed (not part of the build): " << key << std::endl;
        return false;
    }
    if (Sha256Hex(data, size) != found->second) {
        std::cerr << "[HostCore] integrity check failed (modified): " << key << std::endl;
        return false;
    }
    return true;
}

inline bool Verify(const std::string& key, const std::string& text)
{
    return Verify(key, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

} // namespace integrity
} // namespace next2d
