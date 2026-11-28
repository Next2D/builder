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
 * next2d_host_set_render_thread を使う構成では、GL のコンテキストはコアが作る描画用の
 * スレッドへ移る (その関数の説明を参照)。
 */
#ifndef NEXT2D_HOST_CORE_H_
#define NEXT2D_HOST_CORE_H_

#include <stddef.h>
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

#define NEXT2D_HOST_CORE_API_VERSION 3u

typedef struct Next2DHost Next2DHost;

typedef struct Next2DHostConfig {
    /* 必ず NEXT2D_HOST_CORE_API_VERSION を入れる (不一致なら create は失敗する)。 */
    uint32_t api_version;

    /* デフォルトフレームバッファのピクセルサイズ。 */
    int32_t width;
    int32_t height;
    /* devicePixelRatio。アプリから見たビューポート (innerWidth / innerHeight、CSS ピクセル) は
     * width / height をこの値で割ったものになる。等倍なら 1.0。 */
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

    /* localStorage / indexedDB の保存先ディレクトリ。UTF-8。NULL なら OS の既定
     * (既定の保存先が無い OS では保存されず、終了すると消える)。 */
    const char* storage_dir;

    /* storage_dir のファイルへ書き込んだ直後に呼ばれる。書いた内容を確定する操作が必要な
     * 保存先を使う場合に、その合図として使う。name は保存したストレージの名前
     * (例 "localStorage")。next2d_host_frame を呼んだスレッドから呼ばれる。不要なら NULL。
     * next2d_host_set_cache_dir の保存先へ書いたときも、name を "cache" として呼ばれる。 */
    void (*storage_saved)(const char* name, void* user);
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

/* ポインタ入力 (マウス / タッチ)。x, y は左上原点の、フレームバッファのピクセル座標
 * (コアが devicePixelRatio で割って DOM イベントの座標にする)。
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

/* デフォルトフレームバッファのピクセルサイズが変わったことを伝える (表示先の解像度が
 * 切り替わったときなど)。アプリには、ブラウザのウィンドウサイズ変更と同じ resize として届く。 */
NEXT2D_HOST_CORE_API void next2d_host_resize(Next2DHost* host, int32_t width, int32_t height);

/* 再生中の音声を合成して out (32bit float、インターリーブ、frames × channels 個) へ書き込む。
 * コアが自分で音声を出力しない構成 (Windows 以外) で、埋め込み側が音声出力へ渡す PCM を
 * 取り出すために使う。この関数だけは、音声出力のスレッドなど他のスレッドから呼んでよい
 * (ただし next2d_host_destroy と同時に呼ばないこと)。
 * 合成した場合 1。コアが自分で出力する構成 (Windows) では何も書かず 0 を返す。
 * 一度も呼ばれない間、音声は「再生開始と同時に終了」として扱われる。 */
NEXT2D_HOST_CORE_API int32_t next2d_host_render_audio(
    Next2DHost* host, float* out, int32_t frames, int32_t channels, int32_t sample_rate);

/* フォント (TTF / OTF のバイト列) を登録する。コアはシステムフォントを使わないので、
 * アプリが文字を描くには 1 つ以上のフォントが必要。next2d_host_create の前に呼ぶこと。
 * name は CSS の font-family と照合する名前 (例 "Arial")。太字のフォントは名前に
 * "Bold" を含める (例 "Arial Bold")。指定と一致するフォントが無ければ最初に登録した
 * フォントで描く。データはコピーされる。成功時 1、不正なフォントなら 0。 */
NEXT2D_HOST_CORE_API int32_t next2d_host_register_font(
    const char* name, const uint8_t* data, size_t size);

/* 負荷の状態が変わったときに呼ばれる関数を登録する (不要なら呼ばなくてよい)。
 *   loading = 1 … 資材を読み込んでいる、または処理の重いフレームが出ている (起動直後、画面の切り替えなど)
 *   loading = 0 … 軽いフレームが続いて落ち着いた
 * 一時的に CPU の性能を上げる仕組みを持つ環境で、その切り替えの合図に使える。登録した時点の状態で
 * すぐ 1 回呼ばれ、以後は状態が変わるたびに next2d_host_frame を呼んだスレッドから呼ばれる。
 * 起動直後は loading = 1 から始まる。 */
NEXT2D_HOST_CORE_API void next2d_host_set_load_callback(
    Next2DHost* host, void (*callback)(int32_t loading, void* user), void* user);

/* 描画 (アプリのレンダラ) を、コアが作る専用のスレッドで動かす (不要なら呼ばなくてよい)。
 * アプリの処理と描画が別々のスレッドで同時に進むので、CPU のコアが複数ある環境で 1 フレームに
 * 使える時間が増える。next2d_host_create の前に呼ぶこと。
 *
 * この構成では GL のコンテキストを 1 つのスレッドから別のスレッドへ移すので、そのための
 * 関数を渡す:
 *   make_current    … 呼び出したスレッドでコンテキストを current にする。成功時に 0 以外を返す
 *   release_current … 呼び出したスレッドでの current を外す
 * next2d_host_create は、GL の関数を解決したあと release_current を呼ぶ (呼び出したスレッドから)。
 * 以後、描画用のスレッドが make_current を呼び、GL の呼び出しと提示 (gl_swap_buffers) は
 * すべてそのスレッドから行う。next2d_host_destroy は描画用のスレッドの終了を待ち、その
 * スレッドは終了時に release_current を呼ぶ。next2d_host_create を呼ぶスレッドは、create の
 * 後は GL を呼ばないこと。
 * NULL を渡すと元に戻る (描画も next2d_host_frame を呼んだスレッドで行う)。 */
NEXT2D_HOST_CORE_API void next2d_host_set_render_thread(
    int32_t (*make_current)(void* user), void (*release_current)(void* user), void* user);

/* 作り直せるデータを、次の起動へ持ち越すための保存先のディレクトリ (UTF-8) を設定する
 * (不要なら呼ばなくてよい)。next2d_host_create の前に呼ぶこと。
 * いまは、シェーダのリンク結果を保存する。リンクは環境によって 1 つ数十〜百数十ミリ秒かかり、
 * 新しい表現を初めて描く瞬間に画面が止まる原因になる。保存してあれば、2 回目以降の起動では
 * リンクせずに読み込む。
 * 保存したデータは消えても動作に影響しない (無ければ作り直す) ので、ゲームの保存データ
 * (storage_dir) とは別の、消えてよいデータ用の場所を渡すこと。大きさの目安は、プログラム 1 つに
 * つき数十 KB で、合計 1〜2 MB 程度。
 * 設定しない場合、Windows では %LOCALAPPDATA%\Next2D\cache を使い、それ以外の環境では保存しない。
 * NULL か空文字列を渡すと既定に戻る。 */
NEXT2D_HOST_CORE_API void next2d_host_set_cache_dir(const char* directory);

/* アプリに見せる表示言語 (navigator.language / navigator.languages) を BCP 47 の言語タグ
 * (例 "ja", "en-US", "zh-Hans") で設定する (不要なら呼ばなくてよい)。next2d_host_create の前に
 * 呼ぶこと。本体の言語設定に合わせてゲームの文言を切り替えるために使う。
 * NULL か空文字列を渡すと既定 ("en-US") に戻る。 */
NEXT2D_HOST_CORE_API void next2d_host_set_language(const char* language);

/* コアを破棄する。 */
NEXT2D_HOST_CORE_API void next2d_host_destroy(Next2DHost* host);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NEXT2D_HOST_CORE_H_ */
