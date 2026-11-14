// GlApi: WebGL2 バインディングが OpenGL ES 3 を呼ぶ経路の切り替え。
//
// 既定 (直接リンク): GLES3/gl3.h のプロトタイプをそのまま呼び、libGLESv2 相当の
// ライブラリへリンクする (ANGLE / Mesa など)。
//
// 関数テーブルモード (NEXT2D_GL_FUNCTION_TABLE=1): gl* の呼び出しを関数ポインタの
// テーブル経由に置き換える。テーブルは起動時に、埋め込み側が渡す「名前 → 関数アドレス」
// のローダで埋める。GL のシンボルを直接リンクできない環境 (コンテキスト生成後に
// 関数アドレスを問い合わせる方式のプラットフォーム、GL を別モジュールが持つ構成) 用。
//
// バインディング本体 (WebGL2.cpp) はどちらのモードでも同じ gl* の記述のままでよい。
#pragma once

#include <GLES3/gl3.h>

#if defined(NEXT2D_GL_FUNCTION_TABLE) && NEXT2D_GL_FUNCTION_TABLE

namespace next2d {
namespace glapi {

// 関数名 (例 "glClear") から関数アドレスを返す。見つからなければ nullptr。
using ProcLoader = void* (*)(const char* name, void* user);

struct Table {
#define N2D_GL(name) decltype(&::name) name##_ = nullptr;
#include "GlFunctions.inc"
#undef N2D_GL
};

extern Table g_table;

// テーブルを埋める。解決できなかった関数の数を返す (0 = 全て解決)。
// first_missing が非 null なら、最初に解決できなかった関数名を返す。
int Load(ProcLoader loader, void* user, const char** first_missing);

} // namespace glapi
} // namespace next2d

// これ以降、gl* の呼び出しはテーブル経由になる (プロトタイプ宣言より後に置くこと)。
#include "GlFunctionMacros.inc"

#endif // NEXT2D_GL_FUNCTION_TABLE
