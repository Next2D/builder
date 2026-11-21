// WebGL2RenderingContext → OpenGL ES 3 バインディング。
//
// player の WebGL バックエンド (@next2d/webgl) をネイティブの OpenGL ES 3 上で動かす。
// WebGL2 は OpenGL ES 3.0 とほぼ 1 対 1 なので、大半のメソッドは引数変換だけの素通し。
// 素通しにできない WebGL 固有の意味論だけをここで吸収する:
//   - WebGLBuffer / WebGLTexture 等のオブジェクトラッパー (GL 名は内部フィールドに保持)
//   - UNPACK_FLIP_Y_WEBGL / UNPACK_PREMULTIPLY_ALPHA_WEBGL (GL に無いので CPU で適用)
//   - canvas / ImageBitmap をソースにする texImage2D / texSubImage2D
//   - getBufferSubData (ES3 に無いので glMapBufferRange で読む)
//   - getParameter の戻り値の型 (文字列 / TypedArray / 真偽値 / 数値)
//   - ドライバが境界外メモリを読み書きしないための、ピクセル転送サイズの事前検証
//
// コンテキスト生成と提示は GlesContext (gpu/GlesContext.h) 越し。このファイルが呼ぶのは
// Khronos 公開仕様の OpenGL ES 3.0 関数のみで、プラットフォーム固有 API には依存しない。
//
// 未実装 (player は未使用): transform feedback、query、compressedTex*、
// getUniform / getVertexAttrib / getTexParameter 等の一部 getter、*_BINDING の getParameter。
#include "../Bindings.h"

#include "../../HostContext.h"
#include "../ImageSource.h"
#include "../../gpu/GlesContext.h"
#include "../../platform/HostCache.h"
#include "../../v8/V8Util.h"

#include "GlApi.h"   // GLES3/gl3.h + (任意) 関数テーブル経由の呼び出し

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace next2d {

using v8util::Str;
using v8util::SetValue;

namespace {

using Args = v8::FunctionCallbackInfo<v8::Value>;

// WebGL 固有の pixelStorei パラメータ (GL には存在しない)
constexpr GLenum kUnpackFlipYWebGL            = 0x9240;
constexpr GLenum kUnpackPremultiplyAlphaWebGL = 0x9241;
constexpr GLenum kUnpackColorspaceWebGL       = 0x9243;
constexpr GLenum kBrowserDefaultWebGL         = 0x9244;
constexpr GLenum kDepthStencil                = 0x84F9;

struct GLState {
    HostContext* host = nullptr;
    GlesContext* gles = nullptr;

    bool  unpack_flip_y      = false;
    bool  unpack_premultiply = false;
    GLint unpack_alignment   = 4;
    GLint unpack_row_length  = 0;
    GLint unpack_skip_rows   = 0;
    GLint unpack_skip_pixels = 0;
    GLint pack_alignment     = 4;
    GLint pack_row_length    = 0;
    GLint pack_skip_rows     = 0;
    GLint pack_skip_pixels   = 0;

    GLuint draw_framebuffer  = 0;   // 0 = デフォルトフレームバッファ (WebGL 上の null)

    // GL の状態の控え。確保した領域を消すとき (ZeroInitializeStorage) に、いまの状態を GL へ
    // 問い合わせずに済ませるためのもの。状態の問い合わせ (glGet* / glIsEnabled) は、それまでに
    // 依頼した処理が終わるのを待つことがあり、GPU の処理が溜まっている場面で長い停止になる
    // (開発機で、1 回 50〜90 ms の待ちを確認)。
    GLuint  bound_renderbuffer = 0;
    bool    scissor_enabled    = false;
    GLint   scissor_box[4]     = {0, 0, 0, 0};
    GLboolean color_mask[4]    = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLboolean depth_mask       = GL_TRUE;
    GLuint  stencil_mask_front = 0xFFFFFFFFu;
    GLuint  stencil_mask_back  = 0xFFFFFFFFu;

    // バインディング側の検証で検出したエラー。getError が GL のエラーより先に返す。
    GLenum synthetic_error   = GL_NO_ERROR;

    std::vector<uint8_t> scratch;
    std::vector<GLfloat> fscratch;
    std::vector<GLint>   iscratch;

    // レンダーバッファが実際に持っている領域 (renderbufferStorage の使い回し用)
    struct RenderbufferStorage {
        GLenum format = 0;
        GLsizei width = 0;
        GLsizei height = 0;
    };
    std::unordered_map<GLuint, RenderbufferStorage> renderbuffer_storage;

    // シェーダのリンク結果の保存と再利用 (linkProgram を参照)
    struct ShaderInfo {
        std::string source;
        bool compile_requested = false;   // compileShader が呼ばれた
        bool compiled = false;            // 実際にコンパイルした
    };
    struct ProgramInfo {
        std::vector<GLuint> shaders;      // attachShader された順
        std::string attributes;           // bindAttribLocation の内容
        bool cacheable = true;
    };
    std::unordered_map<GLuint, ShaderInfo> shaders;
    std::unordered_map<GLuint, ProgramInfo> programs;
};

inline GLState* S(const Args& a)
{
    return static_cast<GLState*>(a.Data().As<v8::External>()->Value());
}

inline void SetError(GLState* s, GLenum error)
{
    if (s->synthetic_error == GL_NO_ERROR) {
        s->synthetic_error = error;
    }
}

// デフォルトフレームバッファへ描いたら提示が必要であることを記録する。
inline void Touch(GLState* s)
{
    if (s->draw_framebuffer == 0 && s->gles) {
        s->gles->needs_present = true;
    }
}

// ---- 引数変換 ---------------------------------------------------------------

inline int32_t I(const Args& a, int i)
{
    if (i >= a.Length()) {
        return 0;
    }
    v8::Local<v8::Value> v = a[i];
    if (v->IsInt32()) {
        return v.As<v8::Int32>()->Value();
    }
    if (v->IsBoolean()) {
        return v->IsTrue() ? 1 : 0;
    }
    return v->Int32Value(a.GetIsolate()->GetCurrentContext()).FromMaybe(0);
}

inline uint32_t U(const Args& a, int i)
{
    if (i >= a.Length()) {
        return 0;
    }
    v8::Local<v8::Value> v = a[i];
    if (v->IsUint32()) {
        return v.As<v8::Uint32>()->Value();
    }
    if (v->IsBoolean()) {
        return v->IsTrue() ? 1u : 0u;
    }
    return v->Uint32Value(a.GetIsolate()->GetCurrentContext()).FromMaybe(0);
}

inline double D(const Args& a, int i)
{
    if (i >= a.Length()) {
        return 0.0;
    }
    v8::Local<v8::Value> v = a[i];
    if (v->IsNumber()) {
        return v.As<v8::Number>()->Value();
    }
    return v->NumberValue(a.GetIsolate()->GetCurrentContext()).FromMaybe(0.0);
}

inline GLfloat F(const Args& a, int i)
{
    return static_cast<GLfloat>(D(a, i));
}

inline GLboolean B(const Args& a, int i)
{
    return (i < a.Length() && a[i]->BooleanValue(a.GetIsolate())) ? GL_TRUE : GL_FALSE;
}

// バッファオブジェクト内オフセットを GL のポインタ引数へ変換する。
inline const void* Offset(const Args& a, int i)
{
    return reinterpret_cast<const void*>(static_cast<intptr_t>(D(a, i)));
}

// ---- オブジェクトラッパー ---------------------------------------------------
//
// WebGLBuffer 等は「内部フィールド 0 = 種別、1 = GL 名 (または GLsync)」の JS オブジェクト。
// 種別を照合するので、テクスチャをバッファとして渡すような取り違えは 0 (無効) になる。

enum Kind : int32_t {
    kBuffer = 1,
    kTexture,
    kFramebuffer,
    kRenderbuffer,
    kShader,
    kProgram,
    kVertexArray,
    kSampler,
    kUniformLocation,
    kSync,
};

v8::Local<v8::ObjectTemplate> WrapperTemplate(v8::Isolate* isolate)
{
    // Eternal は isolate と同じ寿命で、終了時の解放順序を気にしなくてよい。
    // スレッドごとに持つ (worker を専用スレッドの別 isolate で動かす構成のため)
    thread_local v8::Isolate* owner = nullptr;
    thread_local v8::Eternal<v8::ObjectTemplate> tmpl;
    if (owner != isolate) {
        v8::Local<v8::ObjectTemplate> t = v8::ObjectTemplate::New(isolate);
        t->SetInternalFieldCount(2);
        tmpl = v8::Eternal<v8::ObjectTemplate>(isolate, t);
        owner = isolate;
    }
    return tmpl.Get(isolate);
}

v8::Local<v8::Object> Wrap(v8::Isolate* isolate, Kind kind, v8::Local<v8::Value> payload)
{
    v8::Local<v8::Object> obj =
        WrapperTemplate(isolate)->NewInstance(isolate->GetCurrentContext()).ToLocalChecked();
    obj->SetInternalField(0, v8::Integer::New(isolate, kind));
    obj->SetInternalField(1, payload);
    return obj;
}

bool Payload(v8::Local<v8::Value> value, Kind kind, v8::Local<v8::Value>* out)
{
    if (!value->IsObject()) {
        return false;
    }
    v8::Local<v8::Object> obj = value.As<v8::Object>();
    if (obj->InternalFieldCount() != 2) {
        return false;
    }
    v8::Local<v8::Data> k = obj->GetInternalField(0);
    if (!k->IsValue() || !k.As<v8::Value>()->IsInt32()
        || k.As<v8::Value>().As<v8::Int32>()->Value() != kind) {
        return false;
    }
    v8::Local<v8::Data> p = obj->GetInternalField(1);
    if (!p->IsValue()) {
        return false;
    }
    *out = p.As<v8::Value>();
    return true;
}

// null / undefined / 種別違い / 削除済みは 0。
GLuint Name(const Args& a, int i, Kind kind)
{
    v8::Local<v8::Value> p;
    if (i < a.Length() && Payload(a[i], kind, &p) && p->IsUint32()) {
        return p.As<v8::Uint32>()->Value();
    }
    return 0;
}

// null / 無効な WebGLUniformLocation は -1 (GL は -1 への uniform 設定を黙って無視する)。
GLint Location(const Args& a, int i)
{
    v8::Local<v8::Value> p;
    if (i < a.Length() && Payload(a[i], kUniformLocation, &p) && p->IsInt32()) {
        return p.As<v8::Int32>()->Value();
    }
    return -1;
}

GLsync Sync(const Args& a, int i)
{
    v8::Local<v8::Value> p;
    if (i < a.Length() && Payload(a[i], kSync, &p) && p->IsExternal()) {
        return static_cast<GLsync>(p.As<v8::External>()->Value());
    }
    return nullptr;
}

void ReturnName(const Args& a, Kind kind, GLuint name)
{
    v8::Isolate* isolate = a.GetIsolate();
    a.GetReturnValue().Set(Wrap(isolate, kind, v8::Integer::NewFromUnsigned(isolate, name)));
}

// delete* 済みのラッパーを無効化する (以後 0 として扱う)。
void Invalidate(const Args& a, int i)
{
    v8::Isolate* isolate = a.GetIsolate();
    a[i].As<v8::Object>()->SetInternalField(1, v8::Integer::New(isolate, 0));
}

// ---- バイト列 ---------------------------------------------------------------

struct Span {
    uint8_t* data = nullptr;
    size_t   size = 0;     // バイト数
    size_t   elem = 1;     // 1 要素のバイト数 (srcOffset の単位)
    bool     ok   = false;
};

Span GetBytes(v8::Local<v8::Value> v)
{
    Span s;
    if (v->IsArrayBufferView()) {
        v8::Local<v8::ArrayBufferView> view = v.As<v8::ArrayBufferView>();
        s.size = view->ByteLength();
        s.data = static_cast<uint8_t*>(view->Buffer()->Data()) + view->ByteOffset();
        if (v->IsTypedArray()) {
            const size_t n = v.As<v8::TypedArray>()->Length();
            s.elem = n ? s.size / n : 1;
        }
        s.ok = true;
    } else if (v->IsArrayBuffer()) {
        v8::Local<v8::ArrayBuffer> buf = v.As<v8::ArrayBuffer>();
        s.data = static_cast<uint8_t*>(buf->Data());
        s.size = buf->ByteLength();
        s.ok = true;
    } else if (v->IsSharedArrayBuffer()) {
        v8::Local<v8::SharedArrayBuffer> buf = v.As<v8::SharedArrayBuffer>();
        s.data = static_cast<uint8_t*>(buf->Data());
        s.size = buf->ByteLength();
        s.ok = true;
    }
    return s;
}

// WebGL2 の (srcData, srcOffset, length) 形式。offset / length は要素単位、length 0 = 末尾まで。
// 範囲外なら ok=false。
Span SubSpan(Span s, size_t offset_elems, size_t length_elems)
{
    if (!s.ok) {
        return s;
    }
    const size_t total = s.elem ? s.size / s.elem : 0;
    if (offset_elems > total || length_elems > total - offset_elems) {
        s.ok = false;
        return s;
    }
    const size_t count = length_elems ? length_elems : total - offset_elems;
    s.data += offset_elems * s.elem;
    s.size = count * s.elem;
    return s;
}

// ---- ピクセル転送 -----------------------------------------------------------

// 1 ピクセルのバイト数。未対応の組み合わせは 0 (呼び出し側が検証をあきらめる)。
size_t BytesPerPixel(GLenum format, GLenum type)
{
    size_t channels = 0;
    switch (format) {
    case GL_RED: case GL_RED_INTEGER: case GL_ALPHA: case GL_LUMINANCE:
    case GL_DEPTH_COMPONENT:
        channels = 1; break;
    case GL_RG: case GL_RG_INTEGER: case GL_LUMINANCE_ALPHA:
        channels = 2; break;
    case GL_RGB: case GL_RGB_INTEGER:
        channels = 3; break;
    case GL_RGBA: case GL_RGBA_INTEGER:
        channels = 4; break;
    case GL_DEPTH_STENCIL:
        return (type == GL_UNSIGNED_INT_24_8) ? 4 : (type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV ? 8 : 0);
    default:
        return 0;
    }
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE:
        return channels;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT:
        return channels * 2;
    case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT:
        return channels * 4;
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_5_5_5_1:
        return 2;
    case GL_UNSIGNED_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_10F_11F_11F_REV:
    case GL_UNSIGNED_INT_5_9_9_9_REV:
        return 4;
    default:
        return 0;
    }
}

inline size_t AlignUp(size_t value, size_t alignment)
{
    return alignment > 1 ? (value + alignment - 1) / alignment * alignment : value;
}

// 画像ソース転送中だけ UNPACK_ROW_LENGTH / SKIP_* を 0 に戻す (スクラッチは詰めた行で渡すため)。
class ScopedTightUnpack {
public:
    explicit ScopedTightUnpack(GLState* s) : s_(s)
    {
        active_ = s->unpack_row_length || s->unpack_skip_rows || s->unpack_skip_pixels;
        if (active_) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        }
    }
    ~ScopedTightUnpack()
    {
        if (active_) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, s_->unpack_row_length);
            glPixelStorei(GL_UNPACK_SKIP_ROWS, s_->unpack_skip_rows);
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, s_->unpack_skip_pixels);
        }
    }
private:
    GLState* s_;
    bool active_ = false;
};

struct Pixels {
    const void* data = nullptr;
    bool from_image = false;   // true のとき data は詰めた RGBA8 行 (s->scratch)
};

// 指定の *_BUFFER_BINDING にバッファが束縛されているか。
bool BufferBound(GLenum binding)
{
    GLint name = 0;
    glGetIntegerv(binding, &name);
    return name != 0;
}

// tex(Sub)Image の転送元 a[index] (+ 任意の srcOffset a[index+1]) を GL へ渡せる形にする。
//   - 省略 / null        : data = nullptr (texImage は領域確保のみ)
//   - 数値               : PIXEL_UNPACK_BUFFER 内オフセット
//   - ArrayBufferView    : サイズを検証し、UNPACK_FLIP_Y_WEBGL なら行を反転した複製を渡す
//   - canvas/ImageBitmap : RGBA8 を取り出し、flipY / premultiply を適用した複製を渡す。
//                          dims_given=false なら *width / *height をソースの寸法で埋める
// 失敗時は synthetic_error を立てて false。
bool ResolvePixels(const Args& a, int index, GLState* s,
                   GLsizei* width, GLsizei* height, GLsizei depth,
                   GLenum format, GLenum type, bool dims_given, Pixels* out)
{
    if (index >= a.Length() || a[index]->IsNullOrUndefined()) {
        return true;
    }
    v8::Local<v8::Value> src = a[index];
    // WebGL2 の規則: オフセット指定は PIXEL_UNPACK_BUFFER が束縛されているときだけ、
    // それ以外の転送元は束縛されていないときだけ有効。守らない呼び出しをドライバへ渡すと
    // オフセットが生ポインタとして解釈されるので、ここで必ず弾く。
    const bool unpack_buffer = BufferBound(GL_PIXEL_UNPACK_BUFFER_BINDING);
    if (src->IsNumber() != unpack_buffer) {
        SetError(s, GL_INVALID_OPERATION);
        return false;
    }
    if (src->IsNumber()) {
        out->data = Offset(a, index);
        return true;
    }

    Span span = GetBytes(src);
    if (span.ok) {
        if (index + 1 < a.Length()) {
            span = SubSpan(span, U(a, index + 1), 0);
            if (!span.ok) {
                SetError(s, GL_INVALID_OPERATION);
                return false;
            }
        }
        const size_t bpp = BytesPerPixel(format, type);
        const bool plain = !s->unpack_row_length && !s->unpack_skip_rows && !s->unpack_skip_pixels;
        if (*width < 0 || *height < 0 || depth < 0) {
            SetError(s, GL_INVALID_VALUE);
            return false;
        }
        const size_t w = static_cast<size_t>(*width);
        const size_t rows = static_cast<size_t>(*height) * static_cast<size_t>(depth);
        if (bpp && plain && w && rows) {
            const size_t stride = AlignUp(w * bpp, static_cast<size_t>(s->unpack_alignment));
            const size_t need = stride * (rows - 1) + w * bpp;
            if (span.size < need) {
                // ドライバに境界外を読ませない (WebGL でも INVALID_OPERATION)。
                SetError(s, GL_INVALID_OPERATION);
                return false;
            }
            if (s->unpack_flip_y) {
                // 各レイヤー (depth) ごとに行順を反転する。
                const size_t h = static_cast<size_t>(*height);
                s->scratch.resize(stride * rows);
                for (size_t z = 0; z < static_cast<size_t>(depth); ++z) {
                    for (size_t y = 0; y < h; ++y) {
                        const size_t from = (z * h + y) * stride;
                        const size_t to = (z * h + (h - 1 - y)) * stride;
                        std::memcpy(s->scratch.data() + to, span.data + from, w * bpp);
                    }
                }
                out->data = s->scratch.data();
                return true;
            }
        } else if (!(bpp && plain)) {
            // 寸法を検証できない組み合わせ: 最低限、空のバッファは渡さない。
            if (!span.size && w && rows) {
                SetError(s, GL_INVALID_OPERATION);
                return false;
            }
        }
        out->data = span.data;
        return true;
    }

    // canvas / OffscreenCanvas / ImageBitmap / Image。ホストの画像ソースは RGBA8 straight alpha。
    const uint8_t* rgba = nullptr;
    uint32_t sw = 0, sh = 0;
    if (!GetImageSourcePixels(a.GetIsolate(), src, &rgba, &sw, &sh) || !rgba) {
        SetError(s, GL_INVALID_VALUE);
        return false;
    }
    if (format != GL_RGBA || type != GL_UNSIGNED_BYTE) {
        SetError(s, GL_INVALID_OPERATION);
        return false;
    }
    if (!dims_given) {
        *width = static_cast<GLsizei>(sw);
        *height = static_cast<GLsizei>(sh);
    }
    // 寸法指定ありはソース左上からの部分矩形 (はみ出す分は切り詰める)。
    const size_t w = std::min<size_t>(static_cast<size_t>(std::max(*width, 0)), sw);
    const size_t h = std::min<size_t>(static_cast<size_t>(std::max(*height, 0)), sh);
    *width = static_cast<GLsizei>(w);
    *height = static_cast<GLsizei>(h);
    s->scratch.resize(w * h * 4);
    for (size_t y = 0; y < h; ++y) {
        const uint8_t* from = rgba + y * static_cast<size_t>(sw) * 4;
        uint8_t* to = s->scratch.data() + (s->unpack_flip_y ? (h - 1 - y) : y) * w * 4;
        if (s->unpack_premultiply) {
            for (size_t x = 0; x < w; ++x) {
                const uint32_t alpha = from[x * 4 + 3];
                to[x * 4 + 0] = static_cast<uint8_t>((from[x * 4 + 0] * alpha + 127) / 255);
                to[x * 4 + 1] = static_cast<uint8_t>((from[x * 4 + 1] * alpha + 127) / 255);
                to[x * 4 + 2] = static_cast<uint8_t>((from[x * 4 + 2] * alpha + 127) / 255);
                to[x * 4 + 3] = static_cast<uint8_t>(alpha);
            }
        } else {
            std::memcpy(to, from, w * 4);
        }
    }
    out->data = s->scratch.data();
    out->from_image = true;
    return true;
}

// ---- 確保した領域のゼロ初期化 -----------------------------------------------
// WebGL は、新しく確保したテクスチャとレンダーバッファの中身を必ずゼロ (透明な黒、
// 深度 1、ステンシル 0) にする。OpenGL ES では中身は不定で、以前のメモリの内容が残る。
// アプリは「確保直後は透明」を前提に、クリアせずに描き足すことがある (キャッシュ用の
// テクスチャなど) ため、初期化しないとノイズが透けて見える。確保の直後に一時的な
// フレームバッファへ取り付けてクリアする。

enum class StorageClass { Color, ColorInt, ColorUint, Depth, Stencil, DepthStencil };

StorageClass ClassifyStorage(GLenum internalformat)
{
    switch (internalformat) {
        case GL_DEPTH_COMPONENT16:
        case GL_DEPTH_COMPONENT24:
        case GL_DEPTH_COMPONENT32F:
            return StorageClass::Depth;
        case GL_STENCIL_INDEX8:
            return StorageClass::Stencil;
        case GL_DEPTH24_STENCIL8:
        case GL_DEPTH32F_STENCIL8:
            return StorageClass::DepthStencil;
        case GL_R8UI: case GL_R16UI: case GL_R32UI:
        case GL_RG8UI: case GL_RG16UI: case GL_RG32UI:
        case GL_RGBA8UI: case GL_RGBA16UI: case GL_RGBA32UI:
        case GL_RGB10_A2UI:
            return StorageClass::ColorUint;
        case GL_R8I: case GL_R16I: case GL_R32I:
        case GL_RG8I: case GL_RG16I: case GL_RG32I:
        case GL_RGBA8I: case GL_RGBA16I: case GL_RGBA32I:
            return StorageClass::ColorInt;
        default:
            return StorageClass::Color;
    }
}

// 現在束縛されている 2D テクスチャの level、または現在束縛されているレンダーバッファを
// ゼロで初期化する。描画に使えない形式 (フレームバッファが不完全になる) は何もしない。
// 描画先の束縛・マスク・シザーは呼び出し前の状態へ戻す。
bool PerfEnabled();   // 環境変数 NEXT2D_HOST_PERF (定義は下)

// 環境変数 NEXT2D_HOST_PERF_SYNC: glFinish で実行を待って、個々の処理の時間を測る
bool PerfSyncEnabled()
{
    static const bool enabled = std::getenv("NEXT2D_HOST_PERF_SYNC") != nullptr;
    return enabled;
}

// ---- シェーダのリンク結果の保存と再利用 -------------------------------------
// シェーダのコンパイルとリンクは環境によって重い (開発機で 1 つ 30〜190 ms)。アプリは新しい
// 表現を初めて描くときにプログラムを作るので、その瞬間に画面が止まる。
// リンクした結果 (プログラムバイナリ) を保存先 (platform/HostCache.h) へ書いておき、次に同じ
// ソースのプログラムを作るときは、コンパイルもリンクもせずにそれを読み込む。
//   - compileShader は、その場ではコンパイルしない (保存した結果が使えれば不要になるため)。
//     リンクで保存が使えなかったとき、またはコンパイル結果を問い合わせられたときに行う。
//   - 鍵は、GL の実装を表す文字列 (ドライバが変わればバイナリの互換性は無い) と、
//     各シェーダのソース、属性の割り当て。
//   - 保存した結果の読み込みに失敗したら (ドライバの更新など)、通常どおりコンパイルして
//     リンクし、保存し直す。
// 保存先が無い環境では、遅らせたコンパイルをリンクのときに行うだけで、動作は変わらない。

uint64_t HashBytes(const void* data, size_t size, uint64_t hash)
{
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= p[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

uint64_t HashString(const std::string& text, uint64_t hash)
{
    hash = HashBytes(text.data(), text.size(), hash);
    const unsigned char separator = 0xFF;
    return HashBytes(&separator, 1, hash);
}

void EnsureShaderCompiled(GLState* s, GLuint shader)
{
    auto it = s->shaders.find(shader);
    if (it == s->shaders.end() || !it->second.compile_requested || it->second.compiled) {
        return;
    }
    glCompileShader(shader);
    it->second.compiled = true;
}

void LinkProgramWithCache(GLState* s, GLuint program)
{
    auto found = s->programs.find(program);
    const bool tracked = found != s->programs.end() && found->second.cacheable
        && !found->second.shaders.empty();
    std::string file;
    if (tracked && !hostcache::Directory().empty()) {
        uint64_t hash = 1469598103934665603ull;
        for (const GLenum name : { GL_VENDOR, GL_RENDERER, GL_VERSION }) {
            const GLubyte* text = glGetString(name);
            hash = HashString(text ? reinterpret_cast<const char*>(text) : "", hash);
        }
        for (const GLuint shader : found->second.shaders) {
            const auto info = s->shaders.find(shader);
            hash = HashString(info != s->shaders.end() ? info->second.source : std::string(), hash);
        }
        hash = HashString(found->second.attributes, hash);
        char name[40];
        std::snprintf(name, sizeof(name), "program-%016llx.bin", static_cast<unsigned long long>(hash));
        file = name;

        // 保存した結果: 先頭 4 バイトがバイナリの形式、残りが本体
        std::vector<uint8_t> bytes;
        if (hostcache::Read(file, &bytes) && bytes.size() > 4) {
            GLenum format = 0;
            std::memcpy(&format, bytes.data(), 4);
            glProgramBinary(program, format, bytes.data() + 4, static_cast<GLsizei>(bytes.size() - 4));
            GLint linked = 0;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            if (linked) {
                return;
            }
            for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; ++i) {}   // 読み込みの失敗はここで捨てる
        }
    }

    if (found != s->programs.end()) {
        for (const GLuint shader : found->second.shaders) {
            EnsureShaderCompiled(s, shader);
        }
    }
    glLinkProgram(program);

    if (!file.empty()) {
        GLint linked = 0;
        GLint length = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
        if (linked && length > 0) {
            std::vector<uint8_t> bytes(static_cast<size_t>(length) + 4);
            GLenum format = 0;
            GLsizei written = 0;
            glGetProgramBinary(program, length, &written, &format, bytes.data() + 4);
            if (written > 0 && glGetError() == GL_NO_ERROR) {
                std::memcpy(bytes.data(), &format, 4);
                hostcache::Write(file, bytes.data(), static_cast<size_t>(written) + 4);
            }
        }
    }
}

// region_width / region_height が正のときは、左下からその大きさの範囲だけを消す
// (領域を大きめに持っているレンダーバッファで、使う範囲だけを消すため)。
// state を渡すと (レンダーバッファのとき)、いまの GL の状態を問い合わせずに state の控えを使い、
// フレームバッファの状態の確認やエラーの読み捨てもしない (どれも、溜まった処理を待つことがある)。
void ZeroInitializeStorage(bool renderbuffer, GLint level, GLenum internalformat,
                           GLsizei region_width = 0, GLsizei region_height = 0,
                           const GLState* state = nullptr)
{
    const bool shadowed = renderbuffer && state != nullptr;

    GLint name = 0;
    GLint saved_fbo = 0;
    GLboolean saved_scissor = GL_FALSE;
    GLint saved_scissor_box[4] = {0, 0, 0, 0};
    GLboolean saved_color_mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLboolean saved_depth_mask = GL_TRUE;
    GLint saved_stencil_front = 0, saved_stencil_back = 0;
    if (shadowed) {
        name = static_cast<GLint>(state->bound_renderbuffer);
        saved_fbo = static_cast<GLint>(state->draw_framebuffer
            ? state->draw_framebuffer
            : (state->gles ? state->gles->default_framebuffer() : 0));
        saved_scissor = state->scissor_enabled ? GL_TRUE : GL_FALSE;
        std::memcpy(saved_scissor_box, state->scissor_box, sizeof(saved_scissor_box));
        std::memcpy(saved_color_mask, state->color_mask, sizeof(saved_color_mask));
        saved_depth_mask = state->depth_mask;
        saved_stencil_front = static_cast<GLint>(state->stencil_mask_front);
        saved_stencil_back = static_cast<GLint>(state->stencil_mask_back);
    } else {
        glGetIntegerv(renderbuffer ? GL_RENDERBUFFER_BINDING : GL_TEXTURE_BINDING_2D, &name);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &saved_fbo);
        saved_scissor = glIsEnabled(GL_SCISSOR_TEST);
        glGetIntegerv(GL_SCISSOR_BOX, saved_scissor_box);
        glGetBooleanv(GL_COLOR_WRITEMASK, saved_color_mask);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &saved_depth_mask);
        glGetIntegerv(GL_STENCIL_WRITEMASK, &saved_stencil_front);
        glGetIntegerv(GL_STENCIL_BACK_WRITEMASK, &saved_stencil_back);
    }
    if (name == 0) {
        return;
    }

    const StorageClass kind = ClassifyStorage(internalformat);
    const GLenum attachment =
        kind == StorageClass::Depth ? GL_DEPTH_ATTACHMENT :
        kind == StorageClass::Stencil ? GL_STENCIL_ATTACHMENT :
        kind == StorageClass::DepthStencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_COLOR_ATTACHMENT0;

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    if (renderbuffer) {
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, attachment, GL_RENDERBUFFER,
                                  static_cast<GLuint>(name));
    } else {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment, GL_TEXTURE_2D,
                               static_cast<GLuint>(name), level);
    }

    if (shadowed || glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        if (region_width > 0 && region_height > 0) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, region_width, region_height);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glStencilMaskSeparate(GL_FRONT, 0xFFFFFFFFu);
        glStencilMaskSeparate(GL_BACK, 0xFFFFFFFFu);

        const GLfloat zero_f[4] = {0, 0, 0, 0};
        const GLint zero_i[4] = {0, 0, 0, 0};
        const GLuint zero_u[4] = {0, 0, 0, 0};
        const GLfloat one = 1.0f;
        switch (kind) {
            case StorageClass::Color:        glClearBufferfv(GL_COLOR, 0, zero_f); break;
            case StorageClass::ColorInt:     glClearBufferiv(GL_COLOR, 0, zero_i); break;
            case StorageClass::ColorUint:    glClearBufferuiv(GL_COLOR, 0, zero_u); break;
            case StorageClass::Depth:        glClearBufferfv(GL_DEPTH, 0, &one); break;
            case StorageClass::Stencil:      glClearBufferiv(GL_STENCIL, 0, zero_i); break;
            case StorageClass::DepthStencil: glClearBufferfi(GL_DEPTH_STENCIL, 0, 1.0f, 0); break;
        }

        if (saved_scissor) {
            glEnable(GL_SCISSOR_TEST);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
        glScissor(saved_scissor_box[0], saved_scissor_box[1], saved_scissor_box[2], saved_scissor_box[3]);
        glColorMask(saved_color_mask[0], saved_color_mask[1], saved_color_mask[2], saved_color_mask[3]);
        glDepthMask(saved_depth_mask);
        glStencilMaskSeparate(GL_FRONT, static_cast<GLuint>(saved_stencil_front));
        glStencilMaskSeparate(GL_BACK, static_cast<GLuint>(saved_stencil_back));
    }

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(saved_fbo));
    glDeleteFramebuffers(1, &fbo);
    if (!shadowed) {
        // 描画に使えない形式を取り付けたときなどのエラーは、アプリに見せない
        for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; ++i) {}
    }
}

void TexImage2D(const Args& a)
{
    GLState* s = S(a);
    const GLenum target = U(a, 0);
    const GLint level = I(a, 1);
    const GLint internal = I(a, 2);
    GLsizei w = 0, h = 0;
    GLenum format, type;
    int src;
    bool dims;
    if (a.Length() >= 9) {
        // (target, level, internalformat, width, height, border, format, type, source[, srcOffset])
        w = I(a, 3); h = I(a, 4); format = U(a, 6); type = U(a, 7); src = 8; dims = true;
    } else {
        // (target, level, internalformat, format, type, source)
        format = U(a, 3); type = U(a, 4); src = 5; dims = false;
    }
    Pixels px;
    if (!ResolvePixels(a, src, s, &w, &h, 1, format, type, dims, &px)) {
        return;
    }
    if (px.from_image) {
        ScopedTightUnpack tight(s);
        glTexImage2D(target, level, internal, w, h, 0, format, type, px.data);
    } else if (!px.data && w > 0 && h > 0 && BytesPerPixel(format, type) &&
               !BufferBound(GL_PIXEL_UNPACK_BUFFER_BINDING)) {
        // 転送元なし (null): WebGL は確保した領域をゼロで初期化する。ES では不定なので
        // ゼロのバッファを渡す。
        ScopedTightUnpack tight(s);
        s->scratch.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * BytesPerPixel(format, type), 0);
        glTexImage2D(target, level, internal, w, h, 0, format, type, s->scratch.data());
    } else {
        glTexImage2D(target, level, internal, w, h, 0, format, type, px.data);
    }
}

void TexSubImage2D(const Args& a)
{
    GLState* s = S(a);
    const GLenum target = U(a, 0);
    const GLint level = I(a, 1);
    const GLint x = I(a, 2), y = I(a, 3);
    GLsizei w = 0, h = 0;
    GLenum format, type;
    int src;
    bool dims;
    if (a.Length() >= 9) {
        // (target, level, xoffset, yoffset, width, height, format, type, source[, srcOffset])
        w = I(a, 4); h = I(a, 5); format = U(a, 6); type = U(a, 7); src = 8; dims = true;
    } else {
        // (target, level, xoffset, yoffset, format, type, source)
        format = U(a, 4); type = U(a, 5); src = 6; dims = false;
    }
    Pixels px;
    if (!ResolvePixels(a, src, s, &w, &h, 1, format, type, dims, &px)) {
        return;
    }
    if (!px.data && !(src < a.Length() && a[src]->IsNumber())) {
        SetError(s, GL_INVALID_VALUE);   // texSubImage は転送元が必須
        return;
    }
    if (px.from_image) {
        ScopedTightUnpack tight(s);
        glTexSubImage2D(target, level, x, y, w, h, format, type, px.data);
    } else {
        glTexSubImage2D(target, level, x, y, w, h, format, type, px.data);
    }
}

// (target, level, internalformat, width, height, depth, border, format, type, source[, srcOffset])
void TexImage3D(const Args& a)
{
    GLState* s = S(a);
    GLsizei w = I(a, 3), h = I(a, 4);
    const GLsizei d = I(a, 5);
    const GLenum format = U(a, 7), type = U(a, 8);
    Pixels px;
    if (!ResolvePixels(a, 9, s, &w, &h, d, format, type, true, &px)) {
        return;
    }
    if (px.from_image) {
        ScopedTightUnpack tight(s);
        glTexImage3D(U(a, 0), I(a, 1), I(a, 2), w, h, 1, 0, format, type, px.data);
    } else {
        glTexImage3D(U(a, 0), I(a, 1), I(a, 2), w, h, d, 0, format, type, px.data);
    }
}

// (target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, source[, srcOffset])
void TexSubImage3D(const Args& a)
{
    GLState* s = S(a);
    GLsizei w = I(a, 5), h = I(a, 6);
    const GLsizei d = I(a, 7);
    const GLenum format = U(a, 8), type = U(a, 9);
    Pixels px;
    if (!ResolvePixels(a, 10, s, &w, &h, d, format, type, true, &px)) {
        return;
    }
    if (!px.data && !(a.Length() > 10 && a[10]->IsNumber())) {
        SetError(s, GL_INVALID_VALUE);
        return;
    }
    if (px.from_image) {
        ScopedTightUnpack tight(s);
        glTexSubImage3D(U(a, 0), I(a, 1), I(a, 2), I(a, 3), I(a, 4), w, h, 1, format, type, px.data);
    } else {
        glTexSubImage3D(U(a, 0), I(a, 1), I(a, 2), I(a, 3), I(a, 4), w, h, d, format, type, px.data);
    }
}

// (x, y, width, height, format, type, dstData[, dstOffset]) / (…, offset: PIXEL_PACK_BUFFER)
void ReadPixels(const Args& a)
{
    GLState* s = S(a);
    const GLint x = I(a, 0), y = I(a, 1);
    const GLsizei w = I(a, 2), h = I(a, 3);
    const GLenum format = U(a, 4), type = U(a, 5);
    // オフセット指定は PIXEL_PACK_BUFFER 束縛時のみ、配列指定は非束縛時のみ (WebGL2 の規則)。
    // 逆の組み合わせをドライバへ渡すと、オフセットが書き込み先ポインタとして扱われる。
    const bool to_buffer = a.Length() > 6 && a[6]->IsNumber();
    if (to_buffer != BufferBound(GL_PIXEL_PACK_BUFFER_BINDING)) {
        SetError(s, GL_INVALID_OPERATION);
        return;
    }
    if (to_buffer) {
        glReadPixels(x, y, w, h, format, type, const_cast<void*>(Offset(a, 6)));
        return;
    }
    Span span = a.Length() > 6 ? GetBytes(a[6]) : Span();
    if (span.ok && a.Length() > 7) {
        span = SubSpan(span, U(a, 7), 0);
    }
    const size_t bpp = BytesPerPixel(format, type);
    const bool plain = !s->pack_row_length && !s->pack_skip_rows && !s->pack_skip_pixels;
    if (!span.ok || !bpp || !plain || w < 0 || h < 0) {
        // 書き込みサイズを保証できない呼び出しはドライバへ渡さない。
        SetError(s, GL_INVALID_OPERATION);
        return;
    }
    if (w == 0 || h == 0) {
        return;
    }
    const size_t stride = AlignUp(static_cast<size_t>(w) * bpp, static_cast<size_t>(s->pack_alignment));
    const size_t need = stride * (static_cast<size_t>(h) - 1) + static_cast<size_t>(w) * bpp;
    if (span.size < need) {
        SetError(s, GL_INVALID_OPERATION);
        return;
    }
    glReadPixels(x, y, w, h, format, type, span.data);
}

// ---- バッファ ---------------------------------------------------------------

// (target, size, usage) / (target, srcData|null, usage[, srcOffset[, length]])
void BufferData(const Args& a)
{
    GLState* s = S(a);
    const GLenum target = U(a, 0);
    const GLenum usage = U(a, 2);
    if (a.Length() < 2 || a[1]->IsNullOrUndefined()) {
        glBufferData(target, 0, nullptr, usage);
        return;
    }
    if (a[1]->IsNumber()) {
        const double size = D(a, 1);
        if (size < 0) {
            SetError(s, GL_INVALID_VALUE);
            return;
        }
        glBufferData(target, static_cast<GLsizeiptr>(size), nullptr, usage);
        return;
    }
    Span span = GetBytes(a[1]);
    if (span.ok && a.Length() > 3) {
        span = SubSpan(span, U(a, 3), U(a, 4));
    }
    if (!span.ok) {
        SetError(s, GL_INVALID_VALUE);
        return;
    }
    glBufferData(target, static_cast<GLsizeiptr>(span.size), span.data, usage);
}

// (target, dstByteOffset, srcData[, srcOffset[, length]])
void BufferSubData(const Args& a)
{
    GLState* s = S(a);
    Span span = a.Length() > 2 ? GetBytes(a[2]) : Span();
    if (span.ok && a.Length() > 3) {
        span = SubSpan(span, U(a, 3), U(a, 4));
    }
    if (!span.ok) {
        SetError(s, GL_INVALID_VALUE);
        return;
    }
    if (span.size) {
        glBufferSubData(U(a, 0), static_cast<GLintptr>(D(a, 1)),
                        static_cast<GLsizeiptr>(span.size), span.data);
    }
}

// (target, srcByteOffset, dstBuffer[, dstOffset[, length]])
// OpenGL ES 3 には glGetBufferSubData が無いので、読み取りマップして複製する。
void GetBufferSubData(const Args& a)
{
    GLState* s = S(a);
    Span span = a.Length() > 2 ? GetBytes(a[2]) : Span();
    if (span.ok && a.Length() > 3) {
        span = SubSpan(span, U(a, 3), U(a, 4));
    }
    if (!span.ok) {
        SetError(s, GL_INVALID_VALUE);
        return;
    }
    if (!span.size) {
        return;
    }
    const GLenum target = U(a, 0);
    // 範囲外は glMapBufferRange が GL_INVALID_VALUE で nullptr を返す。
    const void* mapped = glMapBufferRange(target, static_cast<GLintptr>(D(a, 1)),
                                          static_cast<GLsizeiptr>(span.size), GL_MAP_READ_BIT);
    if (!mapped) {
        return;
    }
    std::memcpy(span.data, mapped, span.size);
    glUnmapBuffer(target);
}

// ---- uniform ----------------------------------------------------------------

// Float32Array (srcOffset / srcLength 付き可) か JS 配列を float 列として取り出す。
bool FloatList(const Args& a, int i, GLState* s, const GLfloat** out, size_t* count)
{
    if (i >= a.Length()) {
        return false;
    }
    v8::Local<v8::Value> v = a[i];
    if (v->IsFloat32Array()) {
        Span span = GetBytes(v);
        span.elem = sizeof(GLfloat);
        if (a.Length() > i + 1) {
            span = SubSpan(span, U(a, i + 1), U(a, i + 2));
            if (!span.ok) {
                SetError(s, GL_INVALID_VALUE);
                return false;
            }
        }
        *out = reinterpret_cast<const GLfloat*>(span.data);
        *count = span.size / sizeof(GLfloat);
        return true;
    }
    if (v->IsArray()) {
        v8::Local<v8::Context> ctx = a.GetIsolate()->GetCurrentContext();
        v8::Local<v8::Array> arr = v.As<v8::Array>();
        const uint32_t n = arr->Length();
        s->fscratch.resize(n);
        for (uint32_t k = 0; k < n; ++k) {
            v8::Local<v8::Value> e;
            s->fscratch[k] = arr->Get(ctx, k).ToLocal(&e)
                ? static_cast<GLfloat>(e->NumberValue(ctx).FromMaybe(0.0)) : 0.0f;
        }
        *out = s->fscratch.data();
        *count = n;
        return true;
    }
    SetError(s, GL_INVALID_VALUE);
    return false;
}

// Int32Array / Uint32Array (srcOffset / srcLength 付き可) か JS 配列を int 列として取り出す。
bool IntList(const Args& a, int i, GLState* s, const GLint** out, size_t* count)
{
    if (i >= a.Length()) {
        return false;
    }
    v8::Local<v8::Value> v = a[i];
    if (v->IsInt32Array() || v->IsUint32Array()) {
        Span span = GetBytes(v);
        span.elem = sizeof(GLint);
        if (a.Length() > i + 1) {
            span = SubSpan(span, U(a, i + 1), U(a, i + 2));
            if (!span.ok) {
                SetError(s, GL_INVALID_VALUE);
                return false;
            }
        }
        *out = reinterpret_cast<const GLint*>(span.data);
        *count = span.size / sizeof(GLint);
        return true;
    }
    if (v->IsArray()) {
        v8::Local<v8::Context> ctx = a.GetIsolate()->GetCurrentContext();
        v8::Local<v8::Array> arr = v.As<v8::Array>();
        const uint32_t n = arr->Length();
        s->iscratch.resize(n);
        for (uint32_t k = 0; k < n; ++k) {
            v8::Local<v8::Value> e;
            s->iscratch[k] = arr->Get(ctx, k).ToLocal(&e) ? e->Int32Value(ctx).FromMaybe(0) : 0;
        }
        *out = s->iscratch.data();
        *count = n;
        return true;
    }
    SetError(s, GL_INVALID_VALUE);
    return false;
}

#define N2D_UNIFORM_FV(components, glfn) \
    [](const Args& a) { \
        const GLfloat* p = nullptr; size_t n = 0; \
        if (FloatList(a, 1, S(a), &p, &n) && n >= (components)) { \
            glfn(Location(a, 0), static_cast<GLsizei>(n / (components)), p); \
        } \
    }

#define N2D_UNIFORM_IV(components, glfn, cast) \
    [](const Args& a) { \
        const GLint* p = nullptr; size_t n = 0; \
        if (IntList(a, 1, S(a), &p, &n) && n >= (components)) { \
            glfn(Location(a, 0), static_cast<GLsizei>(n / (components)), cast(p)); \
        } \
    }

// (location, transpose, data[, srcOffset[, srcLength]])
#define N2D_UNIFORM_MATRIX(components, glfn) \
    [](const Args& a) { \
        const GLfloat* p = nullptr; size_t n = 0; \
        if (FloatList(a, 2, S(a), &p, &n) && n >= (components)) { \
            glfn(Location(a, 0), static_cast<GLsizei>(n / (components)), B(a, 1), p); \
        } \
    }

inline const GLint* AsInt(const GLint* p) { return p; }
inline const GLuint* AsUint(const GLint* p) { return reinterpret_cast<const GLuint*>(p); }

// ---- getter -----------------------------------------------------------------

template <typename ArrayT, typename ElemT>
v8::Local<v8::Value> NewTypedArray(v8::Isolate* isolate, const ElemT* values, size_t count)
{
    v8::Local<v8::ArrayBuffer> buf = v8::ArrayBuffer::New(isolate, count * sizeof(ElemT));
    std::memcpy(buf->Data(), values, count * sizeof(ElemT));
    return ArrayT::New(buf, 0, count);
}

void GetParameter(const Args& a)
{
    v8::Isolate* isolate = a.GetIsolate();
    GLState* s = S(a);
    const GLenum pname = U(a, 0);
    auto ret = a.GetReturnValue();

    switch (pname) {
    // WebGL が固定文字列を返すもの (実ドライバ名は隠す仕様)
    case GL_VERSION:
        ret.Set(Str(isolate, "WebGL 2.0 (Next2D host / OpenGL ES 3)")); return;
    case GL_SHADING_LANGUAGE_VERSION:
        ret.Set(Str(isolate, "WebGL GLSL ES 3.00 (Next2D host)")); return;
    case GL_VENDOR:
    case GL_RENDERER:
        ret.Set(Str(isolate, "Next2D")); return;

    // バインディング側で保持する WebGL 固有状態
    case kUnpackFlipYWebGL:
        ret.Set(s->unpack_flip_y); return;
    case kUnpackPremultiplyAlphaWebGL:
        ret.Set(s->unpack_premultiply); return;
    case kUnpackColorspaceWebGL:
        ret.Set(static_cast<uint32_t>(kBrowserDefaultWebGL)); return;

    // テクスチャ / レンダーバッファの最大サイズ。環境変数 NEXT2D_HOST_MAX_TEXTURE_SIZE で上限を
    // 付けられる (ドライバの値より大きくはならない)。アプリはこの値から作業用の領域の大きさを
    // 決めることがあり (player はアトラスを「最大サイズの半分」四方で確保する)、ドライバが大きな
    // 値を返す環境では、画面の大きさに対して過大なメモリと確保時間を使うことになる。
    case GL_MAX_TEXTURE_SIZE: case GL_MAX_RENDERBUFFER_SIZE: {
        static const GLint limit = [] {
            const char* value = std::getenv("NEXT2D_HOST_MAX_TEXTURE_SIZE");
            return value ? static_cast<GLint>(std::atoi(value)) : 0;
        }();
        GLint v = 0;
        glGetIntegerv(pname, &v);
        ret.Set(limit > 0 ? std::min(v, limit) : v);
        return;
    }

    // 真偽値
    case GL_BLEND: case GL_CULL_FACE: case GL_DEPTH_TEST: case GL_DITHER:
    case GL_POLYGON_OFFSET_FILL: case GL_SAMPLE_ALPHA_TO_COVERAGE: case GL_SAMPLE_COVERAGE:
    case GL_SCISSOR_TEST: case GL_STENCIL_TEST: case GL_DEPTH_WRITEMASK:
    case GL_RASTERIZER_DISCARD: case GL_SAMPLE_COVERAGE_INVERT:
    case GL_TRANSFORM_FEEDBACK_ACTIVE: case GL_TRANSFORM_FEEDBACK_PAUSED: {
        GLboolean v = GL_FALSE;
        glGetBooleanv(pname, &v);
        ret.Set(v == GL_TRUE);
        return;
    }
    case GL_COLOR_WRITEMASK: {
        GLboolean v[4] = {};
        glGetBooleanv(pname, v);
        v8::Local<v8::Array> arr = v8::Array::New(isolate, 4);
        v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
        for (uint32_t k = 0; k < 4; ++k) {
            arr->Set(ctx, k, v8::Boolean::New(isolate, v[k] == GL_TRUE)).Check();
        }
        ret.Set(arr);
        return;
    }

    // TypedArray
    case GL_VIEWPORT: case GL_SCISSOR_BOX: {
        GLint v[4] = {};
        glGetIntegerv(pname, v);
        ret.Set(NewTypedArray<v8::Int32Array, GLint>(isolate, v, 4));
        return;
    }
    case GL_MAX_VIEWPORT_DIMS: {
        GLint v[2] = {};
        glGetIntegerv(pname, v);
        ret.Set(NewTypedArray<v8::Int32Array, GLint>(isolate, v, 2));
        return;
    }
    case GL_ALIASED_LINE_WIDTH_RANGE: case GL_ALIASED_POINT_SIZE_RANGE: case GL_DEPTH_RANGE: {
        GLfloat v[2] = {};
        glGetFloatv(pname, v);
        ret.Set(NewTypedArray<v8::Float32Array, GLfloat>(isolate, v, 2));
        return;
    }
    case GL_COLOR_CLEAR_VALUE: case GL_BLEND_COLOR: {
        GLfloat v[4] = {};
        glGetFloatv(pname, v);
        ret.Set(NewTypedArray<v8::Float32Array, GLfloat>(isolate, v, 4));
        return;
    }

    // 浮動小数
    case GL_DEPTH_CLEAR_VALUE: case GL_LINE_WIDTH: case GL_POLYGON_OFFSET_FACTOR:
    case GL_POLYGON_OFFSET_UNITS: case GL_SAMPLE_COVERAGE_VALUE: case GL_MAX_TEXTURE_LOD_BIAS: {
        GLfloat v = 0;
        glGetFloatv(pname, &v);
        ret.Set(static_cast<double>(v));
        return;
    }

    // 64bit 整数
    case GL_MAX_ELEMENT_INDEX: case GL_MAX_SERVER_WAIT_TIMEOUT: case GL_MAX_UNIFORM_BLOCK_SIZE:
    case GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS:
    case GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS: {
        GLint64 v = 0;
        glGetInteger64v(pname, &v);
        ret.Set(static_cast<double>(v));
        return;
    }

    // 束縛中オブジェクトの取得は未対応 (ラッパーを追跡していない)
    case GL_ARRAY_BUFFER_BINDING: case GL_ELEMENT_ARRAY_BUFFER_BINDING:
    case GL_FRAMEBUFFER_BINDING: case GL_READ_FRAMEBUFFER_BINDING:
    case GL_RENDERBUFFER_BINDING: case GL_TEXTURE_BINDING_2D: case GL_TEXTURE_BINDING_CUBE_MAP:
    case GL_TEXTURE_BINDING_2D_ARRAY: case GL_TEXTURE_BINDING_3D: case GL_CURRENT_PROGRAM:
    case GL_VERTEX_ARRAY_BINDING: case GL_SAMPLER_BINDING: case GL_PIXEL_PACK_BUFFER_BINDING:
    case GL_PIXEL_UNPACK_BUFFER_BINDING: case GL_UNIFORM_BUFFER_BINDING:
    case GL_COPY_READ_BUFFER_BINDING: case GL_COPY_WRITE_BUFFER_BINDING:
    case GL_TRANSFORM_FEEDBACK_BINDING: case GL_TRANSFORM_FEEDBACK_BUFFER_BINDING:
        ret.SetNull();
        return;

    default: {
        // 残りは単一の整数 (未知の pname は GL が INVALID_ENUM を立て、0 が返る)。
        GLint v = 0;
        glGetIntegerv(pname, &v);
        ret.Set(v);
        return;
    }
    }
}

std::string InfoLog(GLuint name, bool program)
{
    GLint length = 0;
    if (program) {
        glGetProgramiv(name, GL_INFO_LOG_LENGTH, &length);
    } else {
        glGetShaderiv(name, GL_INFO_LOG_LENGTH, &length);
    }
    if (length <= 1) {
        return std::string();
    }
    std::string log(static_cast<size_t>(length), '\0');
    GLsizei written = 0;
    if (program) {
        glGetProgramInfoLog(name, length, &written, log.data());
    } else {
        glGetShaderInfoLog(name, length, &written, log.data());
    }
    log.resize(static_cast<size_t>(std::max(written, 0)));
    return log;
}

// getActiveUniform / getActiveAttrib → WebGLActiveInfo {name, size, type}
void ActiveInfo(const Args& a, bool uniform)
{
    v8::Isolate* isolate = a.GetIsolate();
    const GLuint program = Name(a, 0, kProgram);
    GLint max_length = 0;
    glGetProgramiv(program, uniform ? GL_ACTIVE_UNIFORM_MAX_LENGTH : GL_ACTIVE_ATTRIBUTE_MAX_LENGTH,
                   &max_length);
    std::string name(static_cast<size_t>(std::max(max_length, 1)), '\0');
    GLsizei length = 0;
    GLint size = 0;
    GLenum type = 0;
    if (uniform) {
        glGetActiveUniform(program, U(a, 1), static_cast<GLsizei>(name.size()), &length, &size, &type, name.data());
    } else {
        glGetActiveAttrib(program, U(a, 1), static_cast<GLsizei>(name.size()), &length, &size, &type, name.data());
    }
    if (length <= 0) {
        a.GetReturnValue().SetNull();
        return;
    }
    name.resize(static_cast<size_t>(length));
    v8::Local<v8::Object> info = v8::Object::New(isolate);
    SetValue(isolate, info, "name", Str(isolate, name));
    SetValue(isolate, info, "size", v8::Integer::New(isolate, size));
    SetValue(isolate, info, "type", v8::Integer::NewFromUnsigned(isolate, type));
    a.GetReturnValue().Set(info);
}

// drawBuffers / invalidateFramebuffer 用: JS 配列 → GLenum 列
std::vector<GLenum> EnumList(const Args& a, int i)
{
    std::vector<GLenum> out;
    if (i < a.Length() && a[i]->IsArray()) {
        v8::Local<v8::Context> ctx = a.GetIsolate()->GetCurrentContext();
        v8::Local<v8::Array> arr = a[i].As<v8::Array>();
        out.reserve(arr->Length());
        for (uint32_t k = 0; k < arr->Length(); ++k) {
            v8::Local<v8::Value> e;
            out.push_back(arr->Get(ctx, k).ToLocal(&e) ? e->Uint32Value(ctx).FromMaybe(0) : 0);
        }
    }
    return out;
}

#define N2D_GEN(kind, glgen) \
    [](const Args& a) { GLuint n = 0; glgen(1, &n); ReturnName(a, kind, n); }

#define N2D_DELETE(kind, gldelete) \
    [](const Args& a) { \
        GLuint n = Name(a, 0, kind); \
        if (n) { gldelete(1, &n); Invalidate(a, 0); } \
    }

// ---- メソッド表 -------------------------------------------------------------

struct Method {
    const char* name;
    v8::FunctionCallback callback;
};

const Method kMethods[] = {
    // --- 状態
    {"enable", [](const Args& a) {
        if (U(a, 0) == GL_SCISSOR_TEST) { S(a)->scissor_enabled = true; }
        glEnable(U(a, 0));
    }},
    {"disable", [](const Args& a) {
        if (U(a, 0) == GL_SCISSOR_TEST) { S(a)->scissor_enabled = false; }
        glDisable(U(a, 0));
    }},
    {"isEnabled", [](const Args& a) { a.GetReturnValue().Set(glIsEnabled(U(a, 0)) == GL_TRUE); }},
    {"viewport", [](const Args& a) { glViewport(I(a, 0), I(a, 1), I(a, 2), I(a, 3)); }},
    {"scissor", [](const Args& a) {
        GLState* s = S(a);
        s->scissor_box[0] = I(a, 0); s->scissor_box[1] = I(a, 1);
        s->scissor_box[2] = I(a, 2); s->scissor_box[3] = I(a, 3);
        glScissor(I(a, 0), I(a, 1), I(a, 2), I(a, 3));
    }},
    {"clearColor", [](const Args& a) { glClearColor(F(a, 0), F(a, 1), F(a, 2), F(a, 3)); }},
    {"clearDepth", [](const Args& a) { glClearDepthf(F(a, 0)); }},
    {"clearStencil", [](const Args& a) { glClearStencil(I(a, 0)); }},
    {"clear", [](const Args& a) { glClear(U(a, 0)); Touch(S(a)); }},
    {"colorMask", [](const Args& a) {
        GLState* s = S(a);
        for (int i = 0; i < 4; ++i) { s->color_mask[i] = B(a, i); }
        glColorMask(B(a, 0), B(a, 1), B(a, 2), B(a, 3));
    }},
    {"depthMask", [](const Args& a) { S(a)->depth_mask = B(a, 0); glDepthMask(B(a, 0)); }},
    {"depthFunc", [](const Args& a) { glDepthFunc(U(a, 0)); }},
    {"depthRange", [](const Args& a) { glDepthRangef(F(a, 0), F(a, 1)); }},
    {"cullFace", [](const Args& a) { glCullFace(U(a, 0)); }},
    {"frontFace", [](const Args& a) { glFrontFace(U(a, 0)); }},
    {"lineWidth", [](const Args& a) { glLineWidth(F(a, 0)); }},
    {"polygonOffset", [](const Args& a) { glPolygonOffset(F(a, 0), F(a, 1)); }},
    {"sampleCoverage", [](const Args& a) { glSampleCoverage(F(a, 0), B(a, 1)); }},
    {"hint", [](const Args& a) { glHint(U(a, 0), U(a, 1)); }},
    {"blendColor", [](const Args& a) { glBlendColor(F(a, 0), F(a, 1), F(a, 2), F(a, 3)); }},
    {"blendEquation", [](const Args& a) { glBlendEquation(U(a, 0)); }},
    {"blendEquationSeparate", [](const Args& a) { glBlendEquationSeparate(U(a, 0), U(a, 1)); }},
    {"blendFunc", [](const Args& a) { glBlendFunc(U(a, 0), U(a, 1)); }},
    {"blendFuncSeparate", [](const Args& a) { glBlendFuncSeparate(U(a, 0), U(a, 1), U(a, 2), U(a, 3)); }},
    {"stencilFunc", [](const Args& a) { glStencilFunc(U(a, 0), I(a, 1), U(a, 2)); }},
    {"stencilFuncSeparate", [](const Args& a) { glStencilFuncSeparate(U(a, 0), U(a, 1), I(a, 2), U(a, 3)); }},
    {"stencilMask", [](const Args& a) {
        S(a)->stencil_mask_front = U(a, 0);
        S(a)->stencil_mask_back = U(a, 0);
        glStencilMask(U(a, 0));
    }},
    {"stencilMaskSeparate", [](const Args& a) {
        const GLenum face = U(a, 0);
        if (face == GL_FRONT || face == GL_FRONT_AND_BACK) { S(a)->stencil_mask_front = U(a, 1); }
        if (face == GL_BACK || face == GL_FRONT_AND_BACK) { S(a)->stencil_mask_back = U(a, 1); }
        glStencilMaskSeparate(face, U(a, 1));
    }},
    {"stencilOp", [](const Args& a) { glStencilOp(U(a, 0), U(a, 1), U(a, 2)); }},
    {"stencilOpSeparate", [](const Args& a) { glStencilOpSeparate(U(a, 0), U(a, 1), U(a, 2), U(a, 3)); }},
    {"flush", [](const Args&) { glFlush(); }},
    {"finish", [](const Args&) { glFinish(); }},
    {"pixelStorei", [](const Args& a) {
        GLState* s = S(a);
        const GLenum pname = U(a, 0);
        const GLint value = I(a, 1);
        switch (pname) {
        case kUnpackFlipYWebGL:            s->unpack_flip_y = value != 0; return;
        case kUnpackPremultiplyAlphaWebGL: s->unpack_premultiply = value != 0; return;
        case kUnpackColorspaceWebGL:       return;   // 色空間変換は行わない
        case GL_UNPACK_ALIGNMENT:   s->unpack_alignment = value; break;
        case GL_UNPACK_ROW_LENGTH:  s->unpack_row_length = value; break;
        case GL_UNPACK_SKIP_ROWS:   s->unpack_skip_rows = value; break;
        case GL_UNPACK_SKIP_PIXELS: s->unpack_skip_pixels = value; break;
        case GL_PACK_ALIGNMENT:     s->pack_alignment = value; break;
        case GL_PACK_ROW_LENGTH:    s->pack_row_length = value; break;
        case GL_PACK_SKIP_ROWS:     s->pack_skip_rows = value; break;
        case GL_PACK_SKIP_PIXELS:   s->pack_skip_pixels = value; break;
        default: break;
        }
        glPixelStorei(pname, value);
    }},
    {"getError", [](const Args& a) {
        GLState* s = S(a);
        GLenum error = s->synthetic_error;
        if (error != GL_NO_ERROR) {
            s->synthetic_error = GL_NO_ERROR;
        } else {
            error = glGetError();
        }
        a.GetReturnValue().Set(static_cast<uint32_t>(error));
    }},
    {"getParameter", GetParameter},
    {"isContextLost", [](const Args& a) { a.GetReturnValue().Set(false); }},
    {"getExtension", [](const Args& a) { a.GetReturnValue().SetNull(); }},
    {"getSupportedExtensions", [](const Args& a) {
        a.GetReturnValue().Set(v8::Array::New(a.GetIsolate(), 0));
    }},
    {"getContextAttributes", [](const Args& a) {
        v8::Isolate* isolate = a.GetIsolate();
        v8::Local<v8::Object> o = v8::Object::New(isolate);
        SetValue(isolate, o, "alpha", v8::True(isolate));
        SetValue(isolate, o, "antialias", v8::False(isolate));
        SetValue(isolate, o, "depth", v8::False(isolate));
        SetValue(isolate, o, "stencil", v8::True(isolate));
        SetValue(isolate, o, "premultipliedAlpha", v8::True(isolate));
        SetValue(isolate, o, "preserveDrawingBuffer", v8::False(isolate));
        SetValue(isolate, o, "failIfMajorPerformanceCaveat", v8::False(isolate));
        SetValue(isolate, o, "desynchronized", v8::False(isolate));
        SetValue(isolate, o, "powerPreference", Str(isolate, "default"));
        a.GetReturnValue().Set(o);
    }},
    {"getShaderPrecisionFormat", [](const Args& a) {
        v8::Isolate* isolate = a.GetIsolate();
        GLint range[2] = {};
        GLint precision = 0;
        glGetShaderPrecisionFormat(U(a, 0), U(a, 1), range, &precision);
        v8::Local<v8::Object> o = v8::Object::New(isolate);
        SetValue(isolate, o, "rangeMin", v8::Integer::New(isolate, range[0]));
        SetValue(isolate, o, "rangeMax", v8::Integer::New(isolate, range[1]));
        SetValue(isolate, o, "precision", v8::Integer::New(isolate, precision));
        a.GetReturnValue().Set(o);
    }},

    // --- バッファ
    {"createBuffer", N2D_GEN(kBuffer, glGenBuffers)},
    {"deleteBuffer", N2D_DELETE(kBuffer, glDeleteBuffers)},
    {"bindBuffer", [](const Args& a) { glBindBuffer(U(a, 0), Name(a, 1, kBuffer)); }},
    {"bindBufferBase", [](const Args& a) { glBindBufferBase(U(a, 0), U(a, 1), Name(a, 2, kBuffer)); }},
    {"bindBufferRange", [](const Args& a) {
        glBindBufferRange(U(a, 0), U(a, 1), Name(a, 2, kBuffer),
                          static_cast<GLintptr>(D(a, 3)), static_cast<GLsizeiptr>(D(a, 4)));
    }},
    {"bufferData", BufferData},
    {"bufferSubData", BufferSubData},
    {"getBufferSubData", GetBufferSubData},
    {"copyBufferSubData", [](const Args& a) {
        glCopyBufferSubData(U(a, 0), U(a, 1), static_cast<GLintptr>(D(a, 2)),
                            static_cast<GLintptr>(D(a, 3)), static_cast<GLsizeiptr>(D(a, 4)));
    }},
    {"getBufferParameter", [](const Args& a) {
        GLint v = 0;
        glGetBufferParameteriv(U(a, 0), U(a, 1), &v);
        a.GetReturnValue().Set(v);
    }},

    // --- 頂点配列
    {"createVertexArray", N2D_GEN(kVertexArray, glGenVertexArrays)},
    {"deleteVertexArray", N2D_DELETE(kVertexArray, glDeleteVertexArrays)},
    {"bindVertexArray", [](const Args& a) { glBindVertexArray(Name(a, 0, kVertexArray)); }},
    {"enableVertexAttribArray", [](const Args& a) { glEnableVertexAttribArray(U(a, 0)); }},
    {"disableVertexAttribArray", [](const Args& a) { glDisableVertexAttribArray(U(a, 0)); }},
    {"vertexAttribPointer", [](const Args& a) {
        // WebGL は ARRAY_BUFFER 束縛を必須とし、最終引数は常にバッファ内オフセット。
        glVertexAttribPointer(U(a, 0), I(a, 1), U(a, 2), B(a, 3), I(a, 4), Offset(a, 5));
    }},
    {"vertexAttribIPointer", [](const Args& a) {
        glVertexAttribIPointer(U(a, 0), I(a, 1), U(a, 2), I(a, 3), Offset(a, 4));
    }},
    {"vertexAttribDivisor", [](const Args& a) { glVertexAttribDivisor(U(a, 0), U(a, 1)); }},
    {"vertexAttrib1f", [](const Args& a) { glVertexAttrib1f(U(a, 0), F(a, 1)); }},
    {"vertexAttrib2f", [](const Args& a) { glVertexAttrib2f(U(a, 0), F(a, 1), F(a, 2)); }},
    {"vertexAttrib3f", [](const Args& a) { glVertexAttrib3f(U(a, 0), F(a, 1), F(a, 2), F(a, 3)); }},
    {"vertexAttrib4f", [](const Args& a) { glVertexAttrib4f(U(a, 0), F(a, 1), F(a, 2), F(a, 3), F(a, 4)); }},

    // --- 描画
    {"drawArrays", [](const Args& a) { glDrawArrays(U(a, 0), I(a, 1), I(a, 2)); Touch(S(a)); }},
    {"drawArraysInstanced", [](const Args& a) {
        glDrawArraysInstanced(U(a, 0), I(a, 1), I(a, 2), I(a, 3)); Touch(S(a));
    }},
    {"drawElements", [](const Args& a) {
        glDrawElements(U(a, 0), I(a, 1), U(a, 2), Offset(a, 3)); Touch(S(a));
    }},
    {"drawElementsInstanced", [](const Args& a) {
        glDrawElementsInstanced(U(a, 0), I(a, 1), U(a, 2), Offset(a, 3), I(a, 4)); Touch(S(a));
    }},
    {"drawRangeElements", [](const Args& a) {
        glDrawRangeElements(U(a, 0), U(a, 1), U(a, 2), I(a, 3), U(a, 4), Offset(a, 5)); Touch(S(a));
    }},
    {"drawBuffers", [](const Args& a) {
        const std::vector<GLenum> list = EnumList(a, 0);
        glDrawBuffers(static_cast<GLsizei>(list.size()), list.data());
    }},
    {"readBuffer", [](const Args& a) { glReadBuffer(U(a, 0)); }},
    {"clearBufferfv", [](const Args& a) {
        const GLfloat* p = nullptr; size_t n = 0;
        const GLenum buffer = U(a, 0);
        // COLOR は 4 成分、DEPTH は 1 成分を読む。不足した配列は渡さない。
        if (FloatList(a, 2, S(a), &p, &n) && n >= (buffer == GL_COLOR ? 4u : 1u)) {
            glClearBufferfv(buffer, I(a, 1), p); Touch(S(a));
        }
    }},
    {"clearBufferiv", [](const Args& a) {
        const GLint* p = nullptr; size_t n = 0;
        const GLenum buffer = U(a, 0);
        if (IntList(a, 2, S(a), &p, &n) && n >= (buffer == GL_COLOR ? 4u : 1u)) {
            glClearBufferiv(buffer, I(a, 1), p); Touch(S(a));
        }
    }},
    {"clearBufferuiv", [](const Args& a) {
        const GLint* p = nullptr; size_t n = 0;
        if (IntList(a, 2, S(a), &p, &n) && n >= 4) {
            glClearBufferuiv(U(a, 0), I(a, 1), AsUint(p)); Touch(S(a));
        }
    }},
    {"clearBufferfi", [](const Args& a) {
        glClearBufferfi(U(a, 0), I(a, 1), F(a, 2), I(a, 3)); Touch(S(a));
    }},

    // --- テクスチャ
    {"createTexture", N2D_GEN(kTexture, glGenTextures)},
    {"deleteTexture", N2D_DELETE(kTexture, glDeleteTextures)},
    {"activeTexture", [](const Args& a) { glActiveTexture(U(a, 0)); }},
    {"bindTexture", [](const Args& a) { glBindTexture(U(a, 0), Name(a, 1, kTexture)); }},
    {"texParameteri", [](const Args& a) { glTexParameteri(U(a, 0), U(a, 1), I(a, 2)); }},
    {"texParameterf", [](const Args& a) { glTexParameterf(U(a, 0), U(a, 1), F(a, 2)); }},
    {"generateMipmap", [](const Args& a) { glGenerateMipmap(U(a, 0)); }},
    {"texStorage2D", [](const Args& a) {
        glTexStorage2D(U(a, 0), I(a, 1), U(a, 2), I(a, 3), I(a, 4));
        // WebGL の規則どおり全レベルをゼロで初期化する (2D テクスチャのみ)
        if (U(a, 0) == GL_TEXTURE_2D) {
            for (GLint level = 0; level < I(a, 1); ++level) {
                ZeroInitializeStorage(false, level, U(a, 2));
            }
        }
    }},
    {"texStorage3D", [](const Args& a) {
        glTexStorage3D(U(a, 0), I(a, 1), U(a, 2), I(a, 3), I(a, 4), I(a, 5));
    }},
    {"texImage2D", TexImage2D},
    {"texSubImage2D", TexSubImage2D},
    {"texImage3D", TexImage3D},
    {"texSubImage3D", TexSubImage3D},
    {"copyTexImage2D", [](const Args& a) {
        glCopyTexImage2D(U(a, 0), I(a, 1), U(a, 2), I(a, 3), I(a, 4), I(a, 5), I(a, 6), I(a, 7));
    }},
    {"copyTexSubImage2D", [](const Args& a) {
        glCopyTexSubImage2D(U(a, 0), I(a, 1), I(a, 2), I(a, 3), I(a, 4), I(a, 5), I(a, 6), I(a, 7));
    }},
    {"copyTexSubImage3D", [](const Args& a) {
        glCopyTexSubImage3D(U(a, 0), I(a, 1), I(a, 2), I(a, 3), I(a, 4), I(a, 5), I(a, 6), I(a, 7), I(a, 8));
    }},
    {"readPixels", ReadPixels},

    // --- サンプラー
    {"createSampler", N2D_GEN(kSampler, glGenSamplers)},
    {"deleteSampler", N2D_DELETE(kSampler, glDeleteSamplers)},
    {"bindSampler", [](const Args& a) { glBindSampler(U(a, 0), Name(a, 1, kSampler)); }},
    {"samplerParameteri", [](const Args& a) { glSamplerParameteri(Name(a, 0, kSampler), U(a, 1), I(a, 2)); }},
    {"samplerParameterf", [](const Args& a) { glSamplerParameterf(Name(a, 0, kSampler), U(a, 1), F(a, 2)); }},

    // --- フレームバッファ / レンダーバッファ
    {"createFramebuffer", N2D_GEN(kFramebuffer, glGenFramebuffers)},
    {"deleteFramebuffer", N2D_DELETE(kFramebuffer, glDeleteFramebuffers)},
    {"bindFramebuffer", [](const Args& a) {
        GLState* s = S(a);
        const GLenum target = U(a, 0);
        const GLuint name = Name(a, 1, kFramebuffer);
        if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
            s->draw_framebuffer = name;
        }
        // WebGL の null (デフォルトフレームバッファ) は環境の画面用 FBO へ読み替える。
        glBindFramebuffer(target, name ? name : (s->gles ? s->gles->default_framebuffer() : 0));
    }},
    {"checkFramebufferStatus", [](const Args& a) {
        a.GetReturnValue().Set(static_cast<uint32_t>(glCheckFramebufferStatus(U(a, 0))));
    }},
    {"framebufferTexture2D", [](const Args& a) {
        glFramebufferTexture2D(U(a, 0), U(a, 1), U(a, 2), Name(a, 3, kTexture), I(a, 4));
    }},
    {"framebufferTextureLayer", [](const Args& a) {
        glFramebufferTextureLayer(U(a, 0), U(a, 1), Name(a, 2, kTexture), I(a, 3), I(a, 4));
    }},
    {"framebufferRenderbuffer", [](const Args& a) {
        glFramebufferRenderbuffer(U(a, 0), U(a, 1), U(a, 2), Name(a, 3, kRenderbuffer));
    }},
    {"blitFramebuffer", [](const Args& a) {
        glBlitFramebuffer(I(a, 0), I(a, 1), I(a, 2), I(a, 3), I(a, 4), I(a, 5), I(a, 6), I(a, 7),
                          U(a, 8), U(a, 9));
        Touch(S(a));
    }},
    {"invalidateFramebuffer", [](const Args& a) {
        const std::vector<GLenum> list = EnumList(a, 1);
        glInvalidateFramebuffer(U(a, 0), static_cast<GLsizei>(list.size()), list.data());
    }},
    {"createRenderbuffer", N2D_GEN(kRenderbuffer, glGenRenderbuffers)},
    {"deleteRenderbuffer", [](const Args& a) {
        GLuint n = Name(a, 0, kRenderbuffer);
        if (n) {
            S(a)->renderbuffer_storage.erase(n);
            glDeleteRenderbuffers(1, &n);
            Invalidate(a, 0);
        }
    }},
    {"bindRenderbuffer", [](const Args& a) {
        S(a)->bound_renderbuffer = Name(a, 1, kRenderbuffer);
        glBindRenderbuffer(U(a, 0), S(a)->bound_renderbuffer);
    }},
    {"renderbufferStorage", [](const Args& a) {
        // WebGL1 由来の DEPTH_STENCIL は ES3 のサイズ付き形式へ読み替える。
        const GLenum format = U(a, 1);
        const GLenum sized = format == kDepthStencil ? GL_DEPTH24_STENCIL8 : format;
        const GLsizei width = I(a, 2);
        const GLsizei height = I(a, 3);

        // 領域の確保は環境によって重い (開発機で 1 回 20〜70 ms、画面の切り替えで 9 回続けて
        // 650 ms かかるのを確認)。アプリはレンダーバッファを使い回しながら、必要な大きさが変わる
        // たびに確保し直す。そこで、
        //   - 確保するときは 256 の倍数へ切り上げる
        //   - すでに同じ形式で十分な大きさの領域を持っていれば、確保し直さず中身を消すだけにする
        // フレームバッファに付けるバッファ同士の大きさは違っていてよく (OpenGL ES 3)、描画できる
        // 範囲は小さい方で決まるので、大きめの領域を持っていても描画結果は変わらない。
        // 対象はステンシル / デプスだけ (色のバッファは blit の範囲に大きさが関わるのでそのまま)。
        const GLuint bound = S(a)->bound_renderbuffer;
        const StorageClass kind = ClassifyStorage(sized);
        const bool reusable = bound != 0 && width > 0 && height > 0
            && (kind == StorageClass::Stencil || kind == StorageClass::Depth
                || kind == StorageClass::DepthStencil);
        if (!reusable) {
            if (bound != 0) {
                S(a)->renderbuffer_storage.erase(bound);
            }
            glRenderbufferStorage(U(a, 0), sized, width, height);
            ZeroInitializeStorage(true, 0, sized, 0, 0, S(a));
            return;
        }

        GLState::RenderbufferStorage& have = S(a)->renderbuffer_storage[bound];
        // 持っている領域が要求の 4 倍を超えるほど大きいときは、使い回さず作り直す (メモリの無駄を抑える)
        const bool fits = have.format == sized && have.width >= width && have.height >= height
            && static_cast<int64_t>(have.width) * have.height <= static_cast<int64_t>(width) * height * 4 + 65536;
        // 計測用 (環境変数 NEXT2D_HOST_PERF_SYNC): 確保と初期化のそれぞれにかかった時間を分けて出す。
        // glFinish で実行を待つので、通常の計測 (NEXT2D_HOST_PERF) とは分けてある
        // (待ちを入れると、溜まった GPU の処理の分だけ描画が止まる)。
        const bool perf = PerfSyncEnabled();
        if (perf) {
            glFinish();   // ここまでに積まれた命令の分を、確保の時間に含めない
        }
        const auto t0 = std::chrono::steady_clock::now();
        if (!fits) {
            const auto round_up = [](GLsizei v) { return (v + 255) & ~255; };
            const GLsizei w = round_up(width);
            const GLsizei h = round_up(height);
            glRenderbufferStorage(U(a, 0), sized, w, h);
            have.format = sized;
            have.width = w;
            have.height = h;
        }
        if (perf) {
            glFinish();
        }
        const auto t1 = std::chrono::steady_clock::now();
        // 消すのは、アプリが使う範囲 (要求された大きさ) だけ
        ZeroInitializeStorage(true, 0, sized, width, height, S(a));
        if (perf) {
            glFinish();
            const auto t2 = std::chrono::steady_clock::now();
            const auto ms = [](auto from, auto to) {
                return std::chrono::duration_cast<std::chrono::microseconds>(to - from).count() / 1000.0;
            };
            if (ms(t0, t2) >= 5.0) {
                std::cerr << "[HostCore] renderbufferStorage: " << width << "x" << height
                          << " format 0x" << std::hex << sized << std::dec
                          << (fits ? " (reused " : " (allocated ") << have.width << "x" << have.height
                          << "): storage " << ms(t0, t1) << " ms, zero-init " << ms(t1, t2) << " ms" << std::endl;
            }
        }
    }},
    {"renderbufferStorageMultisample", [](const Args& a) {
        const GLenum format = U(a, 2);
        const GLenum sized = format == kDepthStencil ? GL_DEPTH24_STENCIL8 : format;
        if (!PerfSyncEnabled()) {
            glRenderbufferStorageMultisample(U(a, 0), I(a, 1), sized, I(a, 3), I(a, 4));
            ZeroInitializeStorage(true, 0, sized, 0, 0, S(a));
            return;
        }
        // 計測時: 確保と初期化のそれぞれにかかった時間を分けて出す (glFinish で実行を待つ)
        const auto t0 = std::chrono::steady_clock::now();
        glRenderbufferStorageMultisample(U(a, 0), I(a, 1), sized, I(a, 3), I(a, 4));
        glFinish();
        const auto t1 = std::chrono::steady_clock::now();
        ZeroInitializeStorage(true, 0, sized, 0, 0, S(a));
        glFinish();
        const auto t2 = std::chrono::steady_clock::now();
        const auto ms = [](auto from, auto to) {
            return std::chrono::duration_cast<std::chrono::microseconds>(to - from).count() / 1000.0;
        };
        if (ms(t0, t2) >= 5.0) {
            std::cerr << "[HostCore] renderbufferStorageMultisample: " << I(a, 3) << "x" << I(a, 4)
                      << " samples " << I(a, 1) << " format 0x" << std::hex << sized << std::dec
                      << ": storage " << ms(t0, t1) << " ms, zero-init " << ms(t1, t2) << " ms" << std::endl;
        }
    }},

    // --- シェーダー / プログラム
    {"createShader", [](const Args& a) { ReturnName(a, kShader, glCreateShader(U(a, 0))); }},
    {"deleteShader", [](const Args& a) {
        const GLuint n = Name(a, 0, kShader);
        if (n) { S(a)->shaders.erase(n); glDeleteShader(n); Invalidate(a, 0); }
    }},
    {"shaderSource", [](const Args& a) {
        const std::string source = a.Length() > 1 ? v8util::ToStdString(a.GetIsolate(), a[1]) : std::string();
        const char* text = source.c_str();
        const GLint length = static_cast<GLint>(source.size());
        const GLuint shader = Name(a, 0, kShader);
        glShaderSource(shader, 1, &text, &length);
        if (shader) {
            GLState::ShaderInfo& info = S(a)->shaders[shader];
            info.source = source;
            info.compiled = false;
        }
    }},
    {"compileShader", [](const Args& a) {
        // その場ではコンパイルしない (LinkProgramWithCache の説明を参照)
        const GLuint shader = Name(a, 0, kShader);
        if (shader) {
            GLState::ShaderInfo& info = S(a)->shaders[shader];
            info.compile_requested = true;
            info.compiled = false;
        }
    }},
    {"getShaderParameter", [](const Args& a) {
        const GLenum pname = U(a, 1);
        if (pname == GL_COMPILE_STATUS) {
            EnsureShaderCompiled(S(a), Name(a, 0, kShader));
        }
        GLint v = 0;
        glGetShaderiv(Name(a, 0, kShader), pname, &v);
        if (pname == GL_COMPILE_STATUS || pname == GL_DELETE_STATUS) {
            a.GetReturnValue().Set(v != 0);
        } else {
            a.GetReturnValue().Set(v);
        }
    }},
    {"getShaderInfoLog", [](const Args& a) {
        EnsureShaderCompiled(S(a), Name(a, 0, kShader));
        a.GetReturnValue().Set(Str(a.GetIsolate(), InfoLog(Name(a, 0, kShader), false)));
    }},
    {"createProgram", [](const Args& a) { ReturnName(a, kProgram, glCreateProgram()); }},
    {"deleteProgram", [](const Args& a) {
        const GLuint n = Name(a, 0, kProgram);
        if (n) { S(a)->programs.erase(n); glDeleteProgram(n); Invalidate(a, 0); }
    }},
    {"attachShader", [](const Args& a) {
        const GLuint program = Name(a, 0, kProgram);
        const GLuint shader = Name(a, 1, kShader);
        glAttachShader(program, shader);
        if (program && shader) {
            S(a)->programs[program].shaders.push_back(shader);
        }
    }},
    {"detachShader", [](const Args& a) {
        const GLuint program = Name(a, 0, kProgram);
        const GLuint shader = Name(a, 1, kShader);
        glDetachShader(program, shader);
        auto it = S(a)->programs.find(program);
        if (it != S(a)->programs.end()) {
            auto& list = it->second.shaders;
            list.erase(std::remove(list.begin(), list.end(), shader), list.end());
        }
    }},
    {"bindAttribLocation", [](const Args& a) {
        const std::string name = a.Length() > 2 ? v8util::ToStdString(a.GetIsolate(), a[2]) : std::string();
        const GLuint program = Name(a, 0, kProgram);
        glBindAttribLocation(program, U(a, 1), name.c_str());
        if (program) {
            S(a)->programs[program].attributes += std::to_string(U(a, 1)) + "=" + name + ";";
        }
    }},
    {"linkProgram", [](const Args& a) { LinkProgramWithCache(S(a), Name(a, 0, kProgram)); }},
    {"validateProgram", [](const Args& a) { glValidateProgram(Name(a, 0, kProgram)); }},
    {"useProgram", [](const Args& a) { glUseProgram(Name(a, 0, kProgram)); }},
    {"getProgramParameter", [](const Args& a) {
        const GLenum pname = U(a, 1);
        GLint v = 0;
        glGetProgramiv(Name(a, 0, kProgram), pname, &v);
        if (pname == GL_LINK_STATUS || pname == GL_DELETE_STATUS || pname == GL_VALIDATE_STATUS) {
            a.GetReturnValue().Set(v != 0);
        } else {
            a.GetReturnValue().Set(v);
        }
    }},
    {"getProgramInfoLog", [](const Args& a) {
        a.GetReturnValue().Set(Str(a.GetIsolate(), InfoLog(Name(a, 0, kProgram), true)));
    }},
    {"getAttribLocation", [](const Args& a) {
        const std::string name = a.Length() > 1 ? v8util::ToStdString(a.GetIsolate(), a[1]) : std::string();
        a.GetReturnValue().Set(glGetAttribLocation(Name(a, 0, kProgram), name.c_str()));
    }},
    {"getUniformLocation", [](const Args& a) {
        v8::Isolate* isolate = a.GetIsolate();
        const std::string name = a.Length() > 1 ? v8util::ToStdString(isolate, a[1]) : std::string();
        const GLint location = glGetUniformLocation(Name(a, 0, kProgram), name.c_str());
        if (location < 0) {
            a.GetReturnValue().SetNull();
        } else {
            a.GetReturnValue().Set(Wrap(isolate, kUniformLocation, v8::Integer::New(isolate, location)));
        }
    }},
    {"getActiveUniform", [](const Args& a) { ActiveInfo(a, true); }},
    {"getActiveAttrib", [](const Args& a) { ActiveInfo(a, false); }},
    {"getUniformBlockIndex", [](const Args& a) {
        const std::string name = a.Length() > 1 ? v8util::ToStdString(a.GetIsolate(), a[1]) : std::string();
        a.GetReturnValue().Set(static_cast<uint32_t>(glGetUniformBlockIndex(Name(a, 0, kProgram), name.c_str())));
    }},
    {"uniformBlockBinding", [](const Args& a) {
        glUniformBlockBinding(Name(a, 0, kProgram), U(a, 1), U(a, 2));
    }},

    // --- uniform
    {"uniform1f", [](const Args& a) { glUniform1f(Location(a, 0), F(a, 1)); }},
    {"uniform2f", [](const Args& a) { glUniform2f(Location(a, 0), F(a, 1), F(a, 2)); }},
    {"uniform3f", [](const Args& a) { glUniform3f(Location(a, 0), F(a, 1), F(a, 2), F(a, 3)); }},
    {"uniform4f", [](const Args& a) { glUniform4f(Location(a, 0), F(a, 1), F(a, 2), F(a, 3), F(a, 4)); }},
    {"uniform1i", [](const Args& a) { glUniform1i(Location(a, 0), I(a, 1)); }},
    {"uniform2i", [](const Args& a) { glUniform2i(Location(a, 0), I(a, 1), I(a, 2)); }},
    {"uniform3i", [](const Args& a) { glUniform3i(Location(a, 0), I(a, 1), I(a, 2), I(a, 3)); }},
    {"uniform4i", [](const Args& a) { glUniform4i(Location(a, 0), I(a, 1), I(a, 2), I(a, 3), I(a, 4)); }},
    {"uniform1ui", [](const Args& a) { glUniform1ui(Location(a, 0), U(a, 1)); }},
    {"uniform2ui", [](const Args& a) { glUniform2ui(Location(a, 0), U(a, 1), U(a, 2)); }},
    {"uniform3ui", [](const Args& a) { glUniform3ui(Location(a, 0), U(a, 1), U(a, 2), U(a, 3)); }},
    {"uniform4ui", [](const Args& a) { glUniform4ui(Location(a, 0), U(a, 1), U(a, 2), U(a, 3), U(a, 4)); }},
    {"uniform1fv", N2D_UNIFORM_FV(1u, glUniform1fv)},
    {"uniform2fv", N2D_UNIFORM_FV(2u, glUniform2fv)},
    {"uniform3fv", N2D_UNIFORM_FV(3u, glUniform3fv)},
    {"uniform4fv", N2D_UNIFORM_FV(4u, glUniform4fv)},
    {"uniform1iv", N2D_UNIFORM_IV(1u, glUniform1iv, AsInt)},
    {"uniform2iv", N2D_UNIFORM_IV(2u, glUniform2iv, AsInt)},
    {"uniform3iv", N2D_UNIFORM_IV(3u, glUniform3iv, AsInt)},
    {"uniform4iv", N2D_UNIFORM_IV(4u, glUniform4iv, AsInt)},
    {"uniform1uiv", N2D_UNIFORM_IV(1u, glUniform1uiv, AsUint)},
    {"uniform2uiv", N2D_UNIFORM_IV(2u, glUniform2uiv, AsUint)},
    {"uniform3uiv", N2D_UNIFORM_IV(3u, glUniform3uiv, AsUint)},
    {"uniform4uiv", N2D_UNIFORM_IV(4u, glUniform4uiv, AsUint)},
    {"uniformMatrix2fv", N2D_UNIFORM_MATRIX(4u, glUniformMatrix2fv)},
    {"uniformMatrix3fv", N2D_UNIFORM_MATRIX(9u, glUniformMatrix3fv)},
    {"uniformMatrix4fv", N2D_UNIFORM_MATRIX(16u, glUniformMatrix4fv)},

    // --- 同期オブジェクト
    {"fenceSync", [](const Args& a) {
        v8::Isolate* isolate = a.GetIsolate();
        GLsync sync = glFenceSync(U(a, 0), U(a, 1));
        if (!sync) {
            a.GetReturnValue().SetNull();
        } else {
            a.GetReturnValue().Set(Wrap(isolate, kSync, v8::External::New(isolate, sync)));
        }
    }},
    {"isSync", [](const Args& a) {
        GLsync sync = Sync(a, 0);
        a.GetReturnValue().Set(sync && glIsSync(sync) == GL_TRUE);
    }},
    {"deleteSync", [](const Args& a) {
        GLsync sync = Sync(a, 0);
        if (sync) { glDeleteSync(sync); Invalidate(a, 0); }
    }},
    {"clientWaitSync", [](const Args& a) {
        GLsync sync = Sync(a, 0);
        if (!sync) {
            SetError(S(a), GL_INVALID_VALUE);
            a.GetReturnValue().Set(static_cast<uint32_t>(GL_WAIT_FAILED));
            return;
        }
        const double timeout = std::max(D(a, 2), 0.0);
        a.GetReturnValue().Set(static_cast<uint32_t>(
            glClientWaitSync(sync, U(a, 1), static_cast<GLuint64>(timeout))));
    }},
    {"getSyncParameter", [](const Args& a) {
        GLsync sync = Sync(a, 0);
        if (!sync) {
            SetError(S(a), GL_INVALID_VALUE);
            a.GetReturnValue().SetNull();
            return;
        }
        GLint v = 0;
        GLsizei length = 0;
        glGetSynciv(sync, U(a, 1), 1, &length, &v);
        a.GetReturnValue().Set(v);
    }},
};

#undef N2D_GEN
#undef N2D_DELETE
#undef N2D_UNIFORM_FV
#undef N2D_UNIFORM_IV
#undef N2D_UNIFORM_MATRIX

struct Constant {
    const char* name;
    double value;
};

const Constant kConstants[] = {
#include "WebGL2Constants.inc"
};

// ---- 計測 (環境変数 NEXT2D_HOST_PERF) ---------------------------------------
// メソッドごとの呼び出し回数と所要時間を集計する。初めて描くフレームが重いとき、
// シェーダーのコンパイル・テクスチャ転送・描画命令のどれに時間がかかったかを見分けるためのもの。
// 計測しないビルド / 実行では、メソッドは元のコールバックのまま登録される。
constexpr size_t kMethodCount = sizeof(kMethods) / sizeof(kMethods[0]);

struct MethodPerf {
    uint64_t nanoseconds = 0;
    uint32_t count = 0;
};
MethodPerf g_method_perf[kMethodCount];

template <size_t I>
void TimedMethod(const Args& a)
{
    const auto started = std::chrono::steady_clock::now();
    kMethods[I].callback(a);
    g_method_perf[I].nanoseconds += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count());
    ++g_method_perf[I].count;
}

template <size_t... Is>
constexpr std::array<v8::FunctionCallback, sizeof...(Is)> MakeTimedMethods(std::index_sequence<Is...>)
{
    return {{ &TimedMethod<Is>... }};
}
const auto kTimedMethods = MakeTimedMethods(std::make_index_sequence<kMethodCount>{});

bool PerfEnabled()
{
    static const bool enabled = std::getenv("NEXT2D_HOST_PERF") != nullptr;
    return enabled;
}

} // namespace

std::string TakeWebGL2PerfSummary()
{
    if (!PerfEnabled()) {
        return std::string();
    }
    std::vector<size_t> order;
    uint64_t total = 0;
    uint32_t calls = 0;
    for (size_t i = 0; i < kMethodCount; ++i) {
        if (g_method_perf[i].count > 0) {
            order.push_back(i);
            total += g_method_perf[i].nanoseconds;
            calls += g_method_perf[i].count;
        }
    }
    std::sort(order.begin(), order.end(), [](size_t l, size_t r) {
        return g_method_perf[l].nanoseconds > g_method_perf[r].nanoseconds;
    });
    std::string text = "gl " + std::to_string(total / 1000000) + " ms / " + std::to_string(calls) + " calls";
    for (size_t n = 0; n < order.size() && n < 6; ++n) {
        const MethodPerf& perf = g_method_perf[order[n]];
        text += std::string(n == 0 ? ": " : ", ") + kMethods[order[n]].name + " "
              + std::to_string(perf.nanoseconds / 1000000) + " ms x" + std::to_string(perf.count);
    }
    for (MethodPerf& perf : g_method_perf) {
        perf = MethodPerf();
    }
    return text;
}

namespace {

// このスレッドが GL のコンテキストを current にしたか (worker を専用スレッドで動かす構成で使う)
thread_local bool t_gl_current = false;

} // namespace

bool WebGL2CurrentOnThisThread()
{
    return t_gl_current;
}

// 開発用: 環境変数 NEXT2D_HOST_CAPTURE_DIR を設定すると、提示する直前のデフォルトフレームバッファを
// そのディレクトリへ BMP で書き出す (既定は 1 秒に 1 枚まで。NEXT2D_HOST_CAPTURE_MS で間隔を変えられる)。
// ファイル名は cap-<起動からのミリ秒>.bmp。画面を直接見られない環境で、描画結果と操作の自動再生の
// 当たり具合を確かめるためのもの。
void CaptureWebGL2Frame(GlesContext* gles)
{
    static const char* dir = std::getenv("NEXT2D_HOST_CAPTURE_DIR");
    if (!dir || !*dir || !gles) {
        return;
    }
    static const double interval_ms = [] {
        const char* value = std::getenv("NEXT2D_HOST_CAPTURE_MS");
        const double ms = value ? std::atof(value) : 0.0;
        return ms > 0.0 ? ms : 1000.0;
    }();
    static const auto started = std::chrono::steady_clock::now();
    static double last_ms = -1e9;
    const double now_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    if (now_ms - last_ms < interval_ms) {
        return;
    }
    last_ms = now_ms;

    const int w = gles->width();
    const int h = gles->height();
    if (w <= 0 || h <= 0) {
        return;
    }
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    GLint previous = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gles->default_framebuffer());
    // アプリが設定した読み出しの状態 (読み出し先のバッファ、行の詰め方) を外してから読み、元に戻す。
    // 読み出し先のバッファが束縛されたままだと、glReadPixels はメモリではなくそのバッファへ書く。
    GLint pack_buffer = 0;
    GLint pack_alignment = 4;
    GLint pack_row_length = 0;
    GLint pack_skip_rows = 0;
    GLint pack_skip_pixels = 0;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &pack_row_length);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &pack_skip_rows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &pack_skip_pixels);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(pack_buffer));
    glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, pack_row_length);
    glPixelStorei(GL_PACK_SKIP_ROWS, pack_skip_rows);
    glPixelStorei(GL_PACK_SKIP_PIXELS, pack_skip_pixels);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous));

    // 24 bit の BMP (下の行から並ぶので、GL の原点 (左下) のまま書ける)
    const uint32_t row = (static_cast<uint32_t>(w) * 3 + 3) & ~3u;
    const uint32_t image_size = row * static_cast<uint32_t>(h);
    uint8_t header[54] = {};
    const auto put32 = [&header](int at, uint32_t v) {
        header[at] = v & 0xFF; header[at + 1] = (v >> 8) & 0xFF;
        header[at + 2] = (v >> 16) & 0xFF; header[at + 3] = (v >> 24) & 0xFF;
    };
    header[0] = 'B'; header[1] = 'M';
    put32(2, 54 + image_size);
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<uint32_t>(w));
    put32(22, static_cast<uint32_t>(h));
    header[26] = 1;
    header[28] = 24;
    put32(34, image_size);

    std::vector<uint8_t> out(image_size);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = rgba.data() + static_cast<size_t>(y) * w * 4;
        uint8_t* dst = out.data() + static_cast<size_t>(y) * row;
        for (int x = 0; x < w; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 2];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 0];
        }
    }
    const std::string file = std::string(dir) + "/cap-" + std::to_string(static_cast<long long>(now_ms)) + ".bmp";
    std::ofstream ofs(file, std::ios::binary);
    if (ofs) {
        ofs.write(reinterpret_cast<const char*>(header), sizeof(header));
        ofs.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    }
}

void ReleaseWebGL2Thread(HostContext* host)
{
    if (t_gl_current && host && host->gles) {
        host->gles->ReleaseCurrent();
    }
    t_gl_current = false;
}

v8::Local<v8::Object> CreateWebGL2Context(v8::Isolate* isolate, HostContext* host)
{
    if (!host || !host->gles || !host->gles->MakeCurrent()) {
        return v8::Local<v8::Object>();
    }
    t_gl_current = true;

    auto* state = new GLState();
    state->host = host;
    state->gles = host->gles;
    // 状態の控えの初期値 (ここで 1 度だけ GL から読む)
    state->scissor_enabled = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
    glGetIntegerv(GL_SCISSOR_BOX, state->scissor_box);
    glGetBooleanv(GL_COLOR_WRITEMASK, state->color_mask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &state->depth_mask);
    GLint mask = 0;
    glGetIntegerv(GL_STENCIL_WRITEMASK, &mask);
    state->stencil_mask_front = static_cast<GLuint>(mask);
    glGetIntegerv(GL_STENCIL_BACK_WRITEMASK, &mask);
    state->stencil_mask_back = static_cast<GLuint>(mask);

    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    v8::Local<v8::Object> gl = v8::Object::New(isolate);
    v8::Local<v8::External> data = v8::External::New(isolate, state);

    const bool timed = PerfEnabled();
    for (size_t i = 0; i < kMethodCount; ++i) {
        const Method& m = kMethods[i];
        v8::Local<v8::String> name = v8::String::NewFromUtf8(
            isolate, m.name, v8::NewStringType::kInternalized).ToLocalChecked();
        v8::Local<v8::Function> fn = v8::Function::New(
            ctx, timed ? kTimedMethods[i] : m.callback, data).ToLocalChecked();
        fn->SetName(name);
        gl->Set(ctx, name, fn).Check();
    }
    for (const Constant& c : kConstants) {
        v8::Local<v8::String> name = v8::String::NewFromUtf8(
            isolate, c.name, v8::NewStringType::kInternalized).ToLocalChecked();
        gl->Set(ctx, name, v8::Number::New(isolate, c.value)).Check();
    }

    SetValue(isolate, gl, "drawingBufferWidth", v8::Integer::New(isolate, host->gles->width()));
    SetValue(isolate, gl, "drawingBufferHeight", v8::Integer::New(isolate, host->gles->height()));
    SetValue(isolate, gl, "drawingBufferColorSpace", Str(isolate, "srgb"));
    SetValue(isolate, gl, "__isWebGL2", v8::True(isolate));

    // state は意図的に解放しない。メソッドは data 経由で state を参照し、
    // `const f = gl.clear` のように取り出した関数は gl 本体より長生きし得るため、
    // gl の GC に合わせて解放すると解放後参照になる。コンテキストはアプリにつき 1 つで、
    // state は数十バイト + スクラッチバッファなので、プロセス寿命で保持して問題ない。

    // WebGL の初期状態に合わせる。
    glBindFramebuffer(GL_FRAMEBUFFER, host->gles->default_framebuffer());
    glViewport(0, 0, host->gles->width(), host->gles->height());

    // WebGL の描画バッファは、書き込み時にリニア → sRGB の変換を行わない
    // (シェーダが出力した値がそのまま表示用の値になる)。一方、デフォルトフレームバッファが
    // sRGB 形式の環境では、OpenGL ES はこの変換を既定で行うため、色が二重に変換されて
    // 全体が白っぽくなる。GL_FRAMEBUFFER_SRGB (EXT_sRGB_write_control) で止める。
    // 拡張が無い環境では INVALID_ENUM になるだけなので、エラーは読み捨てる。
    constexpr GLenum kFramebufferSrgb = 0x8DB9;
    for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; ++i) {}
    glDisable(kFramebufferSrgb);
    for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; ++i) {}

    return gl;
}

} // namespace next2d
