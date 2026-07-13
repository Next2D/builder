// EmbeddedAssets: exe に埋め込んだ assets pak (Windows RCDATA) を解析して提供する。
//
// 目的: 書き出した *.exe の隣に平文の JS/HTML を置かず、ゲーム資産(assets/app 一式 +
// js/bootstrap.js)を exe 内リソースへ格納する。埋め込みが無い(開発ビルド等)場合は
// 全 API が「見つからない」を返し、呼び出し側はファイルシステムへフォールバックする。
// これにより埋め込みは純粋に加算的で、既存のファイル読み経路を壊さない。
//
// pak フォーマット (リトルエンディアン uint32):
//   [4]  magic "N2DA"
//   [4]  uint32 version (=1)
//   [4]  uint32 count
//   count 回:
//     [4]         uint32 key_len
//     [key_len]   key   (UTF-8, フォワードスラッシュ相対パス)
//     [4]         uint32 data_len
//     [data_len]  data
//
// key は次のいずれか:
//   - assets/app 基準の相対パス (例 "app.js", "index.html", "assets/xxx.js")
//   - "js/bootstrap.js" / "js/selftest.js" (exe_dir 基準)
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace next2d {

// exe リソースから pak をロードして解析する (プロセスで一度だけ)。
// 埋め込みが存在し解析に成功したとき true。以後の Has/Get はこの結果に従う。
bool InitEmbeddedAssets();

// 埋め込みが利用可能か。
bool HasEmbeddedAssets();

// assets/app のルート絶対パスを登録する。GetEmbeddedAssetByAbsPath の
// 相対キー算出に使う。main で AssetLoader の root と同じ値を渡す。
void SetEmbeddedAssetsRoot(const std::string& abs_root);

// 相対キー直接指定で検索する。見つかればバイト列ポインタ(プロセス生存中有効)、無ければ nullptr。
const std::vector<uint8_t>* GetEmbeddedAsset(const std::string& key);

// 全エントリを列挙する (フォント等、キーが事前に分からない資材の収集用)。
// callback には key とデータ (プロセス生存中有効) が渡る。
void ForEachEmbeddedAsset(
    const std::function<void(const std::string& key,
                             const std::vector<uint8_t>& data)>& callback);

// モジュールローダ用: assets/app 配下の絶対パスを相対キーへ変換して検索する。
// root 未登録 / abs が root 配下でない場合は nullptr。
const std::vector<uint8_t>* GetEmbeddedAssetByAbsPath(const std::string& abs);

// テスト用: 生の pak バッファを解析して (key -> data) を out へ格納する。
// プラットフォーム非依存で macOS 上でも検証できる。成功時 true。
bool ParseEmbeddedPak(const uint8_t* data, std::size_t size,
                      std::vector<std::pair<std::string, std::vector<uint8_t>>>* out);

} // namespace next2d
