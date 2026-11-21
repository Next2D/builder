#include "HostCache.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>

namespace next2d::hostcache {

namespace {

std::mutex g_mutex;
std::string g_directory;        // 設定された保存先 (空 = 既定)
std::atomic<bool> g_written{false};

std::filesystem::path ResolveDirectory()
{
    std::string directory;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        directory = g_directory;
    }
    std::filesystem::path path;
    if (!directory.empty()) {
        path = std::filesystem::u8path(directory);
    } else {
#ifdef _WIN32
        const char* base = std::getenv("LOCALAPPDATA");
        if (!base || !*base) {
            return {};
        }
        path = std::filesystem::path(base) / "Next2D" / "cache";
#else
        return {};
#endif
    }
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return path;
}

} // namespace

void SetDirectory(const std::string& directory)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_directory = directory;
}

std::string Directory()
{
    const auto text = ResolveDirectory().u8string();   // C++20 では char8_t の文字列
    return std::string(text.begin(), text.end());
}

bool Read(const std::string& name, std::vector<uint8_t>* out)
{
    const std::filesystem::path directory = ResolveDirectory();
    if (directory.empty()) {
        return false;
    }
    std::ifstream ifs(directory / name, std::ios::binary);
    if (!ifs) {
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    return !out->empty();
}

bool Write(const std::string& name, const void* data, size_t size)
{
    const std::filesystem::path directory = ResolveDirectory();
    if (directory.empty()) {
        return false;
    }
    const std::filesystem::path target = directory / name;
    const std::filesystem::path temporary = directory / (name + ".tmp");
    {
        std::ofstream ofs(temporary, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return false;
        }
        ofs.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!ofs) {
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::remove(target, ec);
    ec.clear();
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    g_written.store(true);
    return true;
}

bool TakeWritten()
{
    return g_written.exchange(false);
}

} // namespace next2d::hostcache
