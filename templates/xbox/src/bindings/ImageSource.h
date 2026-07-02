// 画像ソース(ImageBitmap / OffscreenCanvas・Canvas の 2D 内容)の RGBA ピクセルへ
// 統一アクセスするための基盤。queue.copyExternalImageToTexture / Canvas2D.drawImage /
// transferToImageBitmap が参照する。
#pragma once

#include "../platform/ImageTypes.h"

#include <v8.h>
#include <cstdint>
#include <vector>

namespace next2d {

// ImageBitmap(JS) をラップして返す (内部フィールド0に External<DecodedImage>, __isImageBitmap=true)。
v8::Local<v8::Object> WrapImageBitmap(v8::Isolate* isolate, DecodedImage* image);

// 任意の画像ソース(ImageBitmap / '2d' コンテキストを持つ canvas / OffscreenCanvas) から
// RGBA ピクセルを取得する。成功時 true。out_rgba は呼び出し中のみ有効な参照。
bool GetImageSourcePixels(v8::Isolate* isolate, v8::Local<v8::Value> source,
                          const uint8_t** out_rgba, uint32_t* out_width, uint32_t* out_height);

// Canvas2D コンテキストオブジェクトから RGBA を取得する (Canvas2D.cpp が実装)。
bool GetCanvas2DPixels(v8::Local<v8::Object> context2d,
                       const uint8_t** out_rgba, uint32_t* out_width, uint32_t* out_height);

} // namespace next2d
