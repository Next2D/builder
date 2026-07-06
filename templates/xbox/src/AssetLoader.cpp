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

// base64 デコード (data: URI 用)。Image.cpp の同等実装と同じ仕様。
static bool DecodeBase64(const std::string& in, std::vector<uint8_t>* out)
{
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out->clear();
    out->reserve(in.size() / 4 * 3);
    int buf = 0, bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
        const int v = val(c);
        if (v < 0) return false;
        buf = buf << 6 | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<uint8_t>(buf >> bits & 0xFF));
        }
    }
    return true;
}

// data: URI を「%xx」percent-decode する (base64 でない稀なケース用)。
static std::vector<uint8_t> PercentDecode(const std::string& in)
{
    std::vector<uint8_t> out;
    out.reserve(in.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            const int hi = hex(in[i + 1]);
            const int lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<uint8_t>(hi << 4 | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(static_cast<uint8_t>(in[i]));
    }
    return out;
}

std::optional<std::vector<uint8_t>> AssetLoader::ReadBinary(const std::string& url) const
{
    // data: URI (Vite の ?inline はアセットを data:<mime>;base64,<payload> に埋め込む)。
    // このゲームは全サウンドを ?inline で取り込むため実体は data URI。画像は Image.cpp が
    // 対応済みだが、音声 (new Audio / Sound の XHR fetch) もこの ReadBinary を通るので
    // ここで一括デコードする。未対応だと data: をファイルパス扱いして開けず、全 SFX/BGM が
    // 無音になる (XAudio2 の可否とは無関係に実機でも無音)。
    if (url.rfind("data:", 0) == 0) {
        const auto comma = url.find(',');
        if (comma == std::string::npos) {
            return std::nullopt;
        }
        const std::string meta = url.substr(5, comma - 5);  // 例: "audio/mpeg;base64"
        const std::string payload = url.substr(comma + 1);
        if (meta.find("base64") != std::string::npos) {
            std::vector<uint8_t> out;
            if (!DecodeBase64(payload, &out)) {
                return std::nullopt;
            }
            return out;
        }
        return PercentDecode(payload);
    }

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
