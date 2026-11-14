// GlApi: 関数テーブルモードのテーブル実体とローダ。
// 直接リンクモードでは空の翻訳単位になる。
#include <GLES3/gl3.h>

#if defined(NEXT2D_GL_FUNCTION_TABLE) && NEXT2D_GL_FUNCTION_TABLE

namespace next2d {
namespace glapi {

using ProcLoader = void* (*)(const char* name, void* user);

struct Table {
#define N2D_GL(name) decltype(&::name) name##_ = nullptr;
#include "GlFunctions.inc"
#undef N2D_GL
};

Table g_table;

int Load(ProcLoader loader, void* user, const char** first_missing)
{
    int missing = 0;
    if (first_missing) {
        *first_missing = nullptr;
    }
    if (!loader) {
        return -1;
    }
#define N2D_GL(name)                                                        \
    g_table.name##_ = reinterpret_cast<decltype(&::name)>(loader(#name, user)); \
    if (!g_table.name##_) {                                                 \
        if (missing == 0 && first_missing) {                                \
            *first_missing = #name;                                         \
        }                                                                   \
        ++missing;                                                          \
    }
#include "GlFunctions.inc"
#undef N2D_GL
    return missing;
}

} // namespace glapi
} // namespace next2d

#endif // NEXT2D_GL_FUNCTION_TABLE
