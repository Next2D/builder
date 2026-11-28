#pragma once

// ゲームのファイルの照合に使う一覧 (platform/AssetIntegrity.h)。
// builder が書き出すときに、このファイルを実際の一覧で置き換える。
// ここにあるのは、builder を通さずにホストだけをビルドするときの空の一覧 (照合しない)。
// 1 要素が 1 ファイルで "<SHA-256 の 16 進 64 桁><空白 2 つ><キー>"。最後は nullptr。

namespace next2d {

inline const char* const kAssetIntegrityLines[] = {
    nullptr
};

} // namespace next2d
