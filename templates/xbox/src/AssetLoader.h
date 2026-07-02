// AssetLoader: assets/app 以下(builder が配置した JS/HTML/画像/音声)を読み込む。
// fetch / XMLHttpRequest / Image / AudioContext のローカル読み込みの基盤。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace next2d {

class AssetLoader {
public:
    // root は実行ファイル隣の "assets/app" を指す。
    explicit AssetLoader(std::string root);

    // アプリのエントリ JS パスを index.html の <script type="module" src> から解決する。
    std::optional<std::string> ResolveEntryModule() const;

    // 相対 URL/パスを assets/app 基準の絶対パスへ解決する。
    std::string Resolve(const std::string& url) const;

    // バイナリ読み込み。存在しなければ nullopt。
    std::optional<std::vector<uint8_t>> ReadBinary(const std::string& url) const;

    // テキスト読み込み。
    std::optional<std::string> ReadText(const std::string& url) const;

    const std::string& root() const { return root_; }

private:
    std::string root_;
};

} // namespace next2d
