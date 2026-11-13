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
#include "../../v8/V8Util.h"

#include <GLES3/gl3.h>

#include <algorithm>
#include <cstring>
#include <string>
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

    // バインディング側の検証で検出したエラー。getError が GL のエラーより先に返す。
    GLenum synthetic_error   = GL_NO_ERROR;

    std::vector<uint8_t> scratch;
    std::vector<GLfloat> fscratch;
    std::vector<GLint>   iscratch;
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
    static v8::Isolate* owner = nullptr;
    static v8::Eternal<v8::ObjectTemplate> tmpl;
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
    {"enable", [](const Args& a) { glEnable(U(a, 0)); }},
    {"disable", [](const Args& a) { glDisable(U(a, 0)); }},
    {"isEnabled", [](const Args& a) { a.GetReturnValue().Set(glIsEnabled(U(a, 0)) == GL_TRUE); }},
    {"viewport", [](const Args& a) { glViewport(I(a, 0), I(a, 1), I(a, 2), I(a, 3)); }},
    {"scissor", [](const Args& a) { glScissor(I(a, 0), I(a, 1), I(a, 2), I(a, 3)); }},
    {"clearColor", [](const Args& a) { glClearColor(F(a, 0), F(a, 1), F(a, 2), F(a, 3)); }},
    {"clearDepth", [](const Args& a) { glClearDepthf(F(a, 0)); }},
    {"clearStencil", [](const Args& a) { glClearStencil(I(a, 0)); }},
    {"clear", [](const Args& a) { glClear(U(a, 0)); Touch(S(a)); }},
    {"colorMask", [](const Args& a) { glColorMask(B(a, 0), B(a, 1), B(a, 2), B(a, 3)); }},
    {"depthMask", [](const Args& a) { glDepthMask(B(a, 0)); }},
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
    {"stencilMask", [](const Args& a) { glStencilMask(U(a, 0)); }},
    {"stencilMaskSeparate", [](const Args& a) { glStencilMaskSeparate(U(a, 0), U(a, 1)); }},
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
    {"texStorage2D", [](const Args& a) { glTexStorage2D(U(a, 0), I(a, 1), U(a, 2), I(a, 3), I(a, 4)); }},
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
    {"deleteRenderbuffer", N2D_DELETE(kRenderbuffer, glDeleteRenderbuffers)},
    {"bindRenderbuffer", [](const Args& a) { glBindRenderbuffer(U(a, 0), Name(a, 1, kRenderbuffer)); }},
    {"renderbufferStorage", [](const Args& a) {
        // WebGL1 由来の DEPTH_STENCIL は ES3 のサイズ付き形式へ読み替える。
        const GLenum format = U(a, 1);
        glRenderbufferStorage(U(a, 0), format == kDepthStencil ? GL_DEPTH24_STENCIL8 : format,
                              I(a, 2), I(a, 3));
    }},
    {"renderbufferStorageMultisample", [](const Args& a) {
        const GLenum format = U(a, 2);
        glRenderbufferStorageMultisample(U(a, 0), I(a, 1),
                                         format == kDepthStencil ? GL_DEPTH24_STENCIL8 : format,
                                         I(a, 3), I(a, 4));
    }},

    // --- シェーダー / プログラム
    {"createShader", [](const Args& a) { ReturnName(a, kShader, glCreateShader(U(a, 0))); }},
    {"deleteShader", [](const Args& a) {
        const GLuint n = Name(a, 0, kShader);
        if (n) { glDeleteShader(n); Invalidate(a, 0); }
    }},
    {"shaderSource", [](const Args& a) {
        const std::string source = a.Length() > 1 ? v8util::ToStdString(a.GetIsolate(), a[1]) : std::string();
        const char* text = source.c_str();
        const GLint length = static_cast<GLint>(source.size());
        glShaderSource(Name(a, 0, kShader), 1, &text, &length);
    }},
    {"compileShader", [](const Args& a) { glCompileShader(Name(a, 0, kShader)); }},
    {"getShaderParameter", [](const Args& a) {
        const GLenum pname = U(a, 1);
        GLint v = 0;
        glGetShaderiv(Name(a, 0, kShader), pname, &v);
        if (pname == GL_COMPILE_STATUS || pname == GL_DELETE_STATUS) {
            a.GetReturnValue().Set(v != 0);
        } else {
            a.GetReturnValue().Set(v);
        }
    }},
    {"getShaderInfoLog", [](const Args& a) {
        a.GetReturnValue().Set(Str(a.GetIsolate(), InfoLog(Name(a, 0, kShader), false)));
    }},
    {"createProgram", [](const Args& a) { ReturnName(a, kProgram, glCreateProgram()); }},
    {"deleteProgram", [](const Args& a) {
        const GLuint n = Name(a, 0, kProgram);
        if (n) { glDeleteProgram(n); Invalidate(a, 0); }
    }},
    {"attachShader", [](const Args& a) { glAttachShader(Name(a, 0, kProgram), Name(a, 1, kShader)); }},
    {"detachShader", [](const Args& a) { glDetachShader(Name(a, 0, kProgram), Name(a, 1, kShader)); }},
    {"bindAttribLocation", [](const Args& a) {
        const std::string name = a.Length() > 2 ? v8util::ToStdString(a.GetIsolate(), a[2]) : std::string();
        glBindAttribLocation(Name(a, 0, kProgram), U(a, 1), name.c_str());
    }},
    {"linkProgram", [](const Args& a) { glLinkProgram(Name(a, 0, kProgram)); }},
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

} // namespace

v8::Local<v8::Object> CreateWebGL2Context(v8::Isolate* isolate, HostContext* host)
{
    if (!host || !host->gles || !host->gles->MakeCurrent()) {
        return v8::Local<v8::Object>();
    }

    auto* state = new GLState();
    state->host = host;
    state->gles = host->gles;

    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    v8::Local<v8::Object> gl = v8::Object::New(isolate);
    v8::Local<v8::External> data = v8::External::New(isolate, state);

    for (const Method& m : kMethods) {
        v8::Local<v8::String> name = v8::String::NewFromUtf8(
            isolate, m.name, v8::NewStringType::kInternalized).ToLocalChecked();
        v8::Local<v8::Function> fn = v8::Function::New(ctx, m.callback, data).ToLocalChecked();
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

    return gl;
}

} // namespace next2d
