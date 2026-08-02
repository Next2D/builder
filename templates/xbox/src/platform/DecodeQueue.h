// DecodeQueue: リソースデコード用のバックグラウンドスレッドプール。
//
// 画像 (WIC) / 音声 (Media Foundation) のデコードはメインスレッドで同期実行すると
// 画面遷移時に 1 秒超のフレームヒッチになる (--perf 計測で特定)。work を
// プールスレッドへ逃がし、complete をメインループの Pump() で実行することで
// ヒッチを解消し、複数リソースの並列デコードでロード時間も短縮する。
// スレッドは Xbox で許可されている (禁止は動的コード生成のみ)。
//
// 契約:
//   - work    : プールスレッドで実行される。V8 / JS ヒープに触れてはならない。
//               (WIC/MF は per-call 実装で COM はスレッド毎に MTA 初期化済み)
//   - complete: work 完了後、メインスレッドの Pump() から実行される。V8 可。
//               (v8::Global を捕捉してよい。Pump は isolate/コンテキストスコープ外で
//                呼ばれるため、complete 側で HandleScope/Context::Scope を張ること)
//   - Shutdown: 全スレッドを join し、未実行の work / complete を破棄する。
//               捕捉された v8::Global の破棄があるため Isolate 破棄前に呼ぶこと。
#pragma once

#include <functional>

namespace next2d::decodequeue {

void Submit(std::function<void()> work, std::function<void()> complete);
void Pump();
void Shutdown();

} // namespace next2d::decodequeue
