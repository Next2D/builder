# third_party ライセンス

コンソール (Game Core OS) には WIC / Media Foundation / DirectWrite が存在しないため、
画像・音声デコードはヘッダオンリーのポータブルデコーダを第一経路として使用する。

| ファイル | 提供元 | ライセンス |
|---|---|---|
| stb_image.h | https://github.com/nothings/stb | public domain (Unlicense) / MIT のデュアル |
| stb_vorbis.c | https://github.com/nothings/stb | public domain (Unlicense) / MIT のデュアル |
| dr_mp3.h | https://github.com/mackron/dr_libs | public domain (Unlicense) / MIT-0 のデュアル |
| dr_wav.h | https://github.com/mackron/dr_libs | public domain (Unlicense) / MIT-0 のデュアル |

いずれも各ファイル末尾に原文のライセンス条文が含まれる。
