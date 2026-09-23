# third_party ライセンス

コンソール (Game Core OS) には WIC / Media Foundation / DirectWrite が存在しないため、
画像・音声デコードはヘッダオンリーのポータブルデコーダを第一経路として使用する。

| ファイル | 提供元 | ライセンス |
|---|---|---|
| stb_image.h | https://github.com/nothings/stb | public domain (Unlicense) / MIT のデュアル |
| stb_truetype.h | https://github.com/nothings/stb | public domain (Unlicense) / MIT のデュアル |
| stb_vorbis.c | https://github.com/nothings/stb | public domain (Unlicense) / MIT のデュアル |
| dr_mp3.h | https://github.com/mackron/dr_libs | public domain (Unlicense) / MIT-0 のデュアル |
| dr_wav.h | https://github.com/mackron/dr_libs | public domain (Unlicense) / MIT-0 のデュアル |

いずれも各ファイル末尾に原文のライセンス条文が含まれる。

## ローカル修正 / Local patches

同梱の stb には以下の修正を適用している。更新時は再適用の要否を確認し、
`tests/stb_safety_test.cpp` と既存のプラットフォームテストを実行する。

The vendored stb files include these local patches. When updating upstream code,
check whether they are still needed and run `tests/stb_safety_test.cpp` and the
existing platform tests.

| ファイル / File | 修正 / Patch |
|---|---|
| stb_image.h | メモリサイズを乗算前に拡張し、16bit変換・GIFフレーム追加時のオーバーフローを検証。GIFの過去フレーム参照をバッファ内に修正。 / Widen operands before size multiplication, validate overflow in 16-bit conversion and GIF frame growth, and keep GIF previous-frame references inside the buffer. |
| stb_truetype.h | ビットマップの確保・初期化サイズを `size_t` で乗算。 / Multiply bitmap allocation and initialization sizes in `size_t`. |
| stb_vorbis.c | ポインタ加算前に入力サイズとシーク位置を比較。 / Check seek offsets against the input size before pointer arithmetic. |
