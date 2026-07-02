#include "AssetLoader.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace next2d {

AssetLoader::AssetLoader(std::string root)
    : root_(std::move(root))
{
}

std::string AssetLoader::Resolve(const std::string& url) const
{
    // 先頭の "/" や "./"、絶対 URL のオリジンを剥がして assets/app 基準に寄せる。
    std::string path = url;

    // スキーム付き (file://, http://localhost/...) はパス部だけ取り出す
    const auto scheme = path.find("://");
    if (scheme != std::string::npos) {
        const auto slash = path.find('/', scheme + 3);
        path = (slash != std::string::npos) ? path.substr(slash) : "/";
    }

    // クエリ/ハッシュ除去
    const auto q = path.find_first_of("?#");
    if (q != std::string::npos) {
        path = path.substr(0, q);
    }

    while (!path.empty() && (path.front() == '/' || path.front() == '\\')) {
        path.erase(path.begin());
    }
    if (path.rfind("./", 0) == 0) {
        path = path.substr(2);
    }

    return (fs::path(root_) / path).string();
}

std::optional<std::vector<uint8_t>> AssetLoader::ReadBinary(const std::string& url) const
{
    const std::string path = Resolve(url);
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        return std::nullopt;
    }
    const std::streamsize size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    if (size > 0 && !ifs.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return std::nullopt;
    }
    return buffer;
}

std::optional<std::string> AssetLoader::ReadText(const std::string& url) const
{
    const std::string path = Resolve(url);
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return std::nullopt;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

std::optional<std::string> AssetLoader::ResolveEntryModule() const
{
    // vite が出力する index.html から <script type="module" src="..."> を抽出する。
    const fs::path index = fs::path(root_) / "index.html";
    std::ifstream ifs(index, std::ios::binary);
    if (!ifs) {
        return std::nullopt;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    const std::string html = oss.str();

    // type="module" を含む <script ... src="..."> を優先して拾う。
    std::regex module_re(
        R"(<script[^>]*type=["']module["'][^>]*src=["']([^"']+)["'])",
        std::regex::icase);
    std::smatch m;
    if (std::regex_search(html, m, module_re)) {
        return (fs::path(root_) / Resolve(m[1].str()).substr(root_.size() + 1)).string();
    }

    // 逆順 (src が先) も試す
    std::regex module_re2(
        R"(<script[^>]*src=["']([^"']+)["'][^>]*type=["']module["'])",
        std::regex::icase);
    if (std::regex_search(html, m, module_re2)) {
        return (fs::path(root_) / Resolve(m[1].str()).substr(root_.size() + 1)).string();
    }

    return std::nullopt;
}

} // namespace next2d
