#include "EmbeddedAssets.h"

#include <filesystem>
#include <map>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace fs = std::filesystem;

namespace next2d {

namespace {

std::map<std::string, std::vector<uint8_t>> g_assets;
std::string g_root;   // assets/app の正規化済み絶対パス
bool g_loaded = false;
bool g_has = false;

inline uint32_t Read32LE(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0])
         | static_cast<uint32_t>(p[1]) << 8
         | static_cast<uint32_t>(p[2]) << 16
         | static_cast<uint32_t>(p[3]) << 24;
}

} // namespace

bool ParseEmbeddedPak(const uint8_t* data, std::size_t size,
                      std::vector<std::pair<std::string, std::vector<uint8_t>>>* out)
{
    if (!data || size < 12) {
        return false;
    }
    if (data[0] != 'N' || data[1] != '2' || data[2] != 'D' || data[3] != 'A') {
        return false;
    }
    // data[4..7] = version (現状 1 のみ。将来の互換のため値は検査しない)
    const uint32_t count = Read32LE(data + 8);
    std::size_t off = 12;
    for (uint32_t i = 0; i < count; ++i) {
        if (off + 4 > size) return false;
        const uint32_t key_len = Read32LE(data + off);
        off += 4;
        if (key_len > size || off + key_len > size) return false;
        std::string key(reinterpret_cast<const char*>(data + off), key_len);
        off += key_len;

        if (off + 4 > size) return false;
        const uint32_t data_len = Read32LE(data + off);
        off += 4;
        if (data_len > size || off + data_len > size) return false;
        std::vector<uint8_t> bytes(data + off, data + off + data_len);
        off += data_len;

        if (out) {
            out->emplace_back(std::move(key), std::move(bytes));
        }
    }
    return true;
}

bool InitEmbeddedAssets()
{
    if (g_loaded) {
        return g_has;
    }
    g_loaded = true;

#ifdef _WIN32
    // 実行ファイル自身のリソースから "N2DASSETS" (RCDATA) を取り出す。
    HRSRC res = FindResourceW(nullptr, L"N2DASSETS", RT_RCDATA);
    if (res) {
        HGLOBAL handle = LoadResource(nullptr, res);
        const DWORD sz = SizeofResource(nullptr, res);
        const void* ptr = handle ? LockResource(handle) : nullptr;
        if (ptr && sz >= 12) {
            std::vector<std::pair<std::string, std::vector<uint8_t>>> entries;
            if (ParseEmbeddedPak(static_cast<const uint8_t*>(ptr), sz, &entries)) {
                for (auto& e : entries) {
                    g_assets.emplace(std::move(e.first), std::move(e.second));
                }
                g_has = true;
            }
        }
    }
#endif

    return g_has;
}

bool HasEmbeddedAssets()
{
    return g_has;
}

void SetEmbeddedAssetsRoot(const std::string& abs_root)
{
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(fs::path(abs_root), ec);
    g_root = ec ? abs_root : canon.string();
}

const std::vector<uint8_t>* GetEmbeddedAsset(const std::string& key)
{
    if (!g_has) {
        return nullptr;
    }
    auto it = g_assets.find(key);
    return it == g_assets.end() ? nullptr : &it->second;
}

void ForEachEmbeddedAsset(
    const std::function<void(const std::string& key,
                             const std::vector<uint8_t>& data)>& callback)
{
    if (!g_has) {
        return;
    }
    for (const auto& [key, data] : g_assets) {
        callback(key, data);
    }
}

const std::vector<uint8_t>* GetEmbeddedAssetByAbsPath(const std::string& abs)
{
    if (!g_has || g_root.empty()) {
        return nullptr;
    }
    // 純粋に字句的な相対化 (FS 参照なし)。abs も root も weakly_canonical 済みを前提。
    const fs::path rel = fs::path(abs).lexically_relative(fs::path(g_root));
    if (rel.empty()) {
        return nullptr;
    }
    const std::string key = rel.generic_string();
    // root 外 ("../..") は埋め込み対象外。
    if (key.rfind("..", 0) == 0) {
        return nullptr;
    }
    return GetEmbeddedAsset(key);
}

} // namespace next2d
