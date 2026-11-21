// HostCache: 作り直せるデータ (シェーダのリンク結果など) を、次の起動へ持ち越すための保存先。
//
// 保存したデータは消えても動作に影響しない (無ければ作り直すだけ)。ゲームの保存データ
// (localStorage / indexedDB、HostContext::storage_dir) とは別の場所に置く。
// 保存先が無い環境では、何も保存しない。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace next2d::hostcache {

// 保存先のディレクトリ (UTF-8) を設定する。空なら既定に戻す。コアを作る前に呼ぶ。
void SetDirectory(const std::string& directory);

// 保存先のディレクトリ。設定が無ければ既定 (Windows は %LOCALAPPDATA%\Next2D\cache、
// それ以外の環境は無し = 空文字列)。
std::string Directory();

// name のファイルを読む。無ければ false。
bool Read(const std::string& name, std::vector<uint8_t>* out);

// name のファイルへ書く (途中で止まっても壊れたファイルが残らないよう、別名で書いてから置き換える)。
bool Write(const std::string& name, const void* data, size_t size);

// 前回の呼び出し以降に Write が成功していれば true を返し、印を下ろす。
// 書いた内容を確定する操作が必要な保存先のために、埋め込み側へ知らせる合図に使う。
bool TakeWritten();

} // namespace next2d::hostcache
