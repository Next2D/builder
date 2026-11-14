/* next2d_host_core: ホスト共通コア (V8 + ブラウザ相当バインディング + WebGL2) の C 窓口。
 *
 * コアを 1 つのライブラリにまとめ、プラットフォーム側 (ウィンドウ / GL コンテキスト /
 * 入力) とは C の関数と POD だけでやり取りする。境界に C++ の型や CRT のオブジェクトを
 * 通さないので、コアとプラットフォーム側で C ランタイムの種類が違っていてもよい。
 *
 * 役割分担:
 *   プラットフォーム側 … ウィンドウと OpenGL ES 3 コンテキストを作って current にし、
 *                         GL 関数のアドレス解決・画面提示・入力をコールバック/関数呼び出しで渡す。
 *   コア               … JS エンジン、DOM/Canvas/Audio 等のバインディング、WebGL2 → GL 変換、
 *                         アプリ (Next2D) の読み込みとフレーム進行。
 *
 * スレッド: すべての関数を、GL コンテキストを current にした同一スレッドから呼ぶこと。
 */
#ifndef NEXT2D_HOST_CORE_H_
#define NEXT2D_HOST_CORE_H_

#include <stdint.h>

#if defined(_WIN32)
#  if defined(NEXT2D_HOST_CORE_BUILD)
#    define NEXT2D_HOST_CORE_API __declspec(dllexport)
#  else
#    define NEXT2D_HOST_CORE_API __declspec(dllimport)
#  endif
#else
#  define NEXT2D_HOST_CORE_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define NEXT2D_HOST_CORE_API_VERSION 1u

typedef struct Next2DHost Next2DHost;

typedef struct Next2DHostConfig {
    /* 必ず NEXT2D_HOST_CORE_API_VERSION を入れる (不一致なら create は失敗する)。 */
    uint32_t api_version;

    /* デフォルトフレームバッファのピクセルサイズと devicePixelRatio。 */
    int32_t width;
    int32_t height;
    double  device_pixel_ratio;

    /* アプリ資材のディレクトリ (index.html がある場所)。UTF-8。 */
    const char* assets_dir;
    /* ホストスクリプト (bootstrap.js) のディレクトリ。UTF-8。 */
    const char* host_script_dir;
    /* 追加フォント (*.ttf / *.otf) のディレクトリ。不要なら NULL。 */
    const char* fonts_dir;

    /* 以下のコールバックへそのまま渡される任意の値。 */
    void* user;

    /* GL 関数名 (例 "glClear") からアドレスを返す。必須。 */
    void* (*gl_get_proc_address)(const char* name, void* user);
    /* デフォルトフレームバッファを画面へ提示する。必須。 */
    void (*gl_swap_buffers)(void* user);
    /* コンテキストをこのスレッドで current にする。常に current なら NULL でよい。
     * 成功時に 0 以外を返す。 */
    int32_t (*gl_make_current)(void* user);
    /* bindFramebuffer(target, null) の束縛先。通常 0。 */
    uint32_t default_framebuffer;
} Next2DHostConfig;

/* ポインタイベントの種類。 */
enum {
    NEXT2D_POINTER_DOWN = 0,
    NEXT2D_POINTER_MOVE = 1,
    NEXT2D_POINTER_UP   = 2
};

/* W3C Gamepad API の standard mapping (17 ボタン / 4 軸)。 */
typedef struct Next2DGamepadState {
    int32_t connected;
    float   axes[4];       /* LX, LY, RX, RY (-1..1、Y は下が正) */
    float   buttons[17];   /* 押下量 0..1 */
} Next2DGamepadState;

/* コアを生成し、bootstrap.js とアプリ本体を読み込む。失敗時 NULL。
 * 失敗理由は標準エラー出力に出る。 */
NEXT2D_HOST_CORE_API Next2DHost* next2d_host_create(const Next2DHostConfig* config);

/* 1 フレーム進める (タイマー、requestAnimationFrame、worker、描画、提示)。
 * 続行なら 1、終了すべきなら 0 を返す。 */
NEXT2D_HOST_CORE_API int32_t next2d_host_frame(Next2DHost* host);

/* ポインタ入力 (マウス / タッチ)。x, y は左上原点のピクセル座標。
 * pointer_type は "mouse" / "touch" など。NULL なら "mouse"。 */
NEXT2D_HOST_CORE_API void next2d_host_pointer_event(
    Next2DHost* host, int32_t kind, double x, double y,
    int32_t button, int32_t pointer_id, const char* pointer_type);

/* キー入力。key_code は DOM の keyCode 相当。key は DOM の key 相当 (NULL 可)。 */
NEXT2D_HOST_CORE_API void next2d_host_key_event(
    Next2DHost* host, int32_t down, int32_t key_code, const char* key, int32_t repeat);

/* index 番 (0..3) のゲームパッド状態を書き込む。 */
NEXT2D_HOST_CORE_API void next2d_host_set_gamepad(
    Next2DHost* host, int32_t index, const Next2DGamepadState* state);

/* コアを破棄する。 */
NEXT2D_HOST_CORE_API void next2d_host_destroy(Next2DHost* host);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NEXT2D_HOST_CORE_H_ */
