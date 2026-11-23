// ShapeAccelerator: 図形の「経路の組み立て」「塗りのメッシュの生成」「線の外周の計算」のネイティブ実装。
//
// Next2D Player (WebGL) は、キャッシュの無い図形を描くたびに、描画命令 (Float32Array) を 1 つずつ
// 解読して経路を組み立て、三角形のメッシュに変換する。どれも単純な数値計算だが、JIT が使えない
// 環境ではインタプリタで実行されるので、図形の多い画面の初回描画の時間の大半を占める。
// 同じ計算をここで行い、player は (この機能があれば) 経路の命令の並びをまとめて渡す。
//
// グローバルの __next2dShape に次の関数を置く。player 側の対応する処理と同じ結果を返す
// (計算の順序と、32 ビットへ丸める位置を合わせてある):
//   path(commands, index, scaleSquared) -> number
//       commands[index] から続く経路の命令 (MOVE_TO / LINE_TO / CURVE_TO / CUBIC / ARC / CLOSE_PATH)
//       をまとめて処理し、経路でない命令の位置 (または終端) を返す。
//   fillSize() -> number
//       いまの経路を塗りのメッシュにしたときの要素数 (0 なら塗るものが無い)。
//   fill(target, offset, a, b, c, d, tx, ty, red, green, blue, alpha) -> number
//       メッシュを target[offset..] へ書き、頂点数を返す。経路は空になる。
//   exportPath(stroke) -> Float64Array
//       経路を [本数, 要素数, x, y, 制御点か, ..., 要素数, ...] で返す (ここで扱わない処理用)。
//       経路は空になる。
//   reset()
//       経路を空にする。
//   strokeSize(thickness, joints, caps, pixelScale) -> number
//       いまの経路を「線の外周をなぞる経路」に置き換え、それを塗りのメッシュにしたときの要素数を
//       返す (0 なら描く線が無い)。続けて fill() を呼ぶと、線のメッシュが得られる。
//       thickness は線の太さの半分、joints は 0 = bevel / 1 = miter / 2 = round、
//       caps は 1 = round / 2 = square。
//
// __next2dShape.webgpu には、WebGPU 版の player と同じ規則の実装を置く (経路の規則とメッシュの
// 形式が WebGL 版と違う)。塗りは経路を消さず、reset() を呼ぶまで何度でも取り出せる:
//   path(commands, index, pixelScale) -> number
//   fillSize() -> number            メッシュの要素数。経路が 1 本も無ければ -1
//   fill(target) -> number          メッシュ (頂点あたり 4 要素) を target の先頭から書き、頂点数を返す
//   exportPath(stroke) -> Float64Array
//   reset()
#include "Bindings.h"

#include "ShapeAcceleratorCore.h"
#include "v8/V8Util.h"

namespace next2d {

namespace {

using shape::PathState;

// 描画は 1 つのスレッド (レンダラ) で行うので、状態はスレッドごとに持つ
thread_local PathState t_path;

// 引数の Float32Array の先頭と要素数を得る
bool GetFloat32(v8::Local<v8::Value> value, float** data, size_t* length)
{
    if (!value->IsFloat32Array()) {
        return false;
    }
    v8::Local<v8::Float32Array> array = value.As<v8::Float32Array>();
    *data = reinterpret_cast<float*>(
        static_cast<uint8_t*>(array->Buffer()->Data()) + array->ByteOffset());
    *length = array->Length();
    return true;
}

inline double NumberArg(const v8::FunctionCallbackInfo<v8::Value>& args, int index)
{
    return args[index]->NumberValue(args.GetIsolate()->GetCurrentContext()).FromMaybe(0.0);
}

void Path(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    float* commands = nullptr;
    size_t length = 0;
    if (args.Length() < 3 || !GetFloat32(args[0], &commands, &length)) {
        args.GetReturnValue().Set(-1);
        return;
    }
    const size_t index = static_cast<size_t>(NumberArg(args, 1));
    const double scale_squared = NumberArg(args, 2);
    args.GetReturnValue().Set(static_cast<double>(
        shape::FeedPath(t_path, commands, length, index, scale_squared)));
}

void FillSize(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    args.GetReturnValue().Set(static_cast<double>(shape::FillSize(t_path)));
}

void Fill(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    PathState& s = t_path;
    float* target = nullptr;
    size_t target_length = 0;
    if (args.Length() < 12 || !GetFloat32(args[0], &target, &target_length)) {
        s.current.clear();
        s.vertices.clear();
        args.GetReturnValue().Set(0);
        return;
    }
    const size_t offset = static_cast<size_t>(NumberArg(args, 1));
    const size_t count = shape::FillMesh(s, target, target_length, offset,
        static_cast<float>(NumberArg(args, 2)), static_cast<float>(NumberArg(args, 3)),
        static_cast<float>(NumberArg(args, 4)), static_cast<float>(NumberArg(args, 5)),
        static_cast<float>(NumberArg(args, 6)), static_cast<float>(NumberArg(args, 7)),
        static_cast<float>(NumberArg(args, 8)), static_cast<float>(NumberArg(args, 9)),
        static_cast<float>(NumberArg(args, 10)), static_cast<float>(NumberArg(args, 11)));
    args.GetReturnValue().Set(static_cast<double>(count));
}

void ExportPath(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    PathState& s = t_path;
    const bool stroke = args.Length() > 0 && args[0]->BooleanValue(isolate);
    shape::FinalizePaths(s, stroke ? 6 : 9);

    size_t total = 1;
    for (const std::vector<double>& path : s.vertices) {
        total += 1 + path.size();
    }
    v8::Local<v8::ArrayBuffer> buffer = v8::ArrayBuffer::New(isolate, total * sizeof(double));
    double* out = static_cast<double*>(buffer->Data());
    size_t at = 0;
    out[at++] = static_cast<double>(s.vertices.size());
    for (const std::vector<double>& path : s.vertices) {
        out[at++] = static_cast<double>(path.size());
        std::memcpy(out + at, path.data(), path.size() * sizeof(double));
        at += path.size();
    }
    s.vertices.clear();
    args.GetReturnValue().Set(v8::Float64Array::New(buffer, 0, total));
}

void Reset(const v8::FunctionCallbackInfo<v8::Value>&)
{
    t_path.current.clear();
    t_path.vertices.clear();
}

// 線の種類の番号 (0 / 1 / 2)。それ以外の値は「何も足さない」として扱う
inline int StyleArg(const v8::FunctionCallbackInfo<v8::Value>& args, int index)
{
    const double value = NumberArg(args, index);
    return value == 0.0 ? 0 : value == 1.0 ? 1 : value == 2.0 ? 2 : -1;
}

void StrokeSize(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    if (args.Length() < 4) {
        t_path.current.clear();
        t_path.vertices.clear();
        args.GetReturnValue().Set(0);
        return;
    }
    args.GetReturnValue().Set(static_cast<double>(shape::StrokeSize(
        t_path, NumberArg(args, 0), StyleArg(args, 1), StyleArg(args, 2), NumberArg(args, 3))));
}

// ---- WebGPU 版 ----------------------------------------------------------------
thread_local shape::gpu::PathState t_gpu_path;

void GpuPath(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    float* commands = nullptr;
    size_t length = 0;
    if (args.Length() < 3 || !GetFloat32(args[0], &commands, &length)) {
        args.GetReturnValue().Set(-1);
        return;
    }
    const size_t index = static_cast<size_t>(NumberArg(args, 1));
    const double pixel_scale = NumberArg(args, 2);
    args.GetReturnValue().Set(static_cast<double>(
        shape::gpu::FeedPath(t_gpu_path, commands, length, index, pixel_scale)));
}

void GpuFillSize(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    if (!shape::gpu::HasFillPath(t_gpu_path)) {
        args.GetReturnValue().Set(-1);
        return;
    }
    args.GetReturnValue().Set(static_cast<double>(shape::gpu::FillSize(t_gpu_path)));
}

void GpuFill(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    float* target = nullptr;
    size_t target_length = 0;
    if (args.Length() < 1 || !GetFloat32(args[0], &target, &target_length)) {
        args.GetReturnValue().Set(0);
        return;
    }
    args.GetReturnValue().Set(static_cast<double>(
        shape::gpu::FillMesh(t_gpu_path, target, target_length)));
}

void GpuExportPath(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    const shape::gpu::PathState& s = t_gpu_path;
    const bool stroke = args.Length() > 0 && args[0]->BooleanValue(isolate);
    const bool uses_current = shape::gpu::UsesCurrent(s, stroke);

    size_t total = 1;
    for (const std::vector<double>& path : s.vertices) {
        total += 1 + path.size();
    }
    if (uses_current) {
        total += 1 + s.current.size();
    }
    v8::Local<v8::ArrayBuffer> buffer = v8::ArrayBuffer::New(isolate, total * sizeof(double));
    double* out = static_cast<double*>(buffer->Data());
    size_t at = 0;
    out[at++] = static_cast<double>(s.vertices.size() + (uses_current ? 1 : 0));
    auto write = [&](const std::vector<double>& path) {
        out[at++] = static_cast<double>(path.size());
        std::memcpy(out + at, path.data(), path.size() * sizeof(double));
        at += path.size();
    };
    for (const std::vector<double>& path : s.vertices) {
        write(path);
    }
    if (uses_current) {
        write(s.current);
    }
    args.GetReturnValue().Set(v8::Float64Array::New(buffer, 0, total));
}

void GpuReset(const v8::FunctionCallbackInfo<v8::Value>&)
{
    shape::gpu::Reset(t_gpu_path);
}

} // namespace

void InstallShapeAccelerator(v8::Isolate* isolate, v8::Local<v8::Object> global)
{
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    v8::Local<v8::Object> object = v8::Object::New(isolate);
    v8util::SetMethod(isolate, object, "path", Path);
    v8util::SetMethod(isolate, object, "fillSize", FillSize);
    v8util::SetMethod(isolate, object, "fill", Fill);
    v8util::SetMethod(isolate, object, "exportPath", ExportPath);
    v8util::SetMethod(isolate, object, "reset", Reset);
    v8util::SetMethod(isolate, object, "strokeSize", StrokeSize);

    v8::Local<v8::Object> gpu = v8::Object::New(isolate);
    v8util::SetMethod(isolate, gpu, "path", GpuPath);
    v8util::SetMethod(isolate, gpu, "fillSize", GpuFillSize);
    v8util::SetMethod(isolate, gpu, "fill", GpuFill);
    v8util::SetMethod(isolate, gpu, "exportPath", GpuExportPath);
    v8util::SetMethod(isolate, gpu, "reset", GpuReset);
    object->Set(context, v8util::Str(isolate, "webgpu"), gpu).Check();

    global->Set(context, v8util::Str(isolate, "__next2dShape"), object).Check();
}

} // namespace next2d
