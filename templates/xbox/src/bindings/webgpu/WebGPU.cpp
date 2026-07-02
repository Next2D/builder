// WebGPU (navigator.gpu) を Dawn へ橋渡しするバインディング本体。
// WebGPUCommon.h の «実装範囲» コメントを参照。
#include "bindings/Bindings.h"
#include "bindings/webgpu/WebGPUCommon.h"
#include "bindings/webgpu/WebGPUEnums.h"

#include "HostContext.h"
#include "gpu/DawnContext.h"
#include "bindings/ImageSource.h"

#include <cstring>
#include <map>
#include <vector>

namespace next2d {
namespace webgpu {

using v8util::Str;
using v8util::SetMethod;
using v8util::SetValue;

// 各 wgpu 型に対応する ObjectTemplate 群 (単一 isolate 前提)。
struct Templates {
    v8::Global<v8::ObjectTemplate> adapter, device, queue, buffer, texture,
        textureView, sampler, shaderModule, bindGroupLayout, bindGroup,
        pipelineLayout, renderPipeline, commandEncoder, renderPass,
        commandBuffer, canvasContext;
};

static std::map<v8::Isolate*, Templates> g_templates;

static Templates& Tmpl(v8::Isolate* isolate)
{
    return g_templates[isolate];
}

template <typename T>
static v8::Local<v8::Object> WrapWith(v8::Isolate* isolate,
                                      v8::Global<v8::ObjectTemplate>& g, T handle)
{
    return Wrap<T>(isolate, g.Get(isolate), std::move(handle));
}

// メソッド付与済みのラップ (前方宣言。定義は device セクション付近)
static v8::Local<v8::Object> WrapBuffer(v8::Isolate*, wgpu::Buffer);
static v8::Local<v8::Object> WrapTexture(v8::Isolate*, wgpu::Texture);
static v8::Local<v8::Object> WrapTextureView(v8::Isolate*, wgpu::TextureView);
static v8::Local<v8::Object> WrapEncoder(v8::Isolate*, wgpu::CommandEncoder);
static v8::Local<v8::Object> WrapPipeline(v8::Isolate*, wgpu::RenderPipeline);

// ---------------------------------------------------------------------------
// GPUBuffer
// ---------------------------------------------------------------------------
static void Buffer_GetMappedRange(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    wgpu::Buffer& buffer = Unwrap<wgpu::Buffer>(args.This());

    uint64_t offset = args.Length() > 0 && args[0]->IsNumber()
        ? static_cast<uint64_t>(args[0].As<v8::Number>()->Value()) : 0;
    uint64_t size = args.Length() > 1 && args[1]->IsNumber()
        ? static_cast<uint64_t>(args[1].As<v8::Number>()->Value())
        : (buffer.GetSize() - offset);

    void* ptr = buffer.GetMappedRange(offset, static_cast<size_t>(size));
    if (!ptr) {
        args.GetReturnValue().Set(v8::ArrayBuffer::New(isolate, 0));
        return;
    }
    // マップ領域を指す非所有 ArrayBuffer (unmap まで有効)
    std::unique_ptr<v8::BackingStore> store = v8::ArrayBuffer::NewBackingStore(
        ptr, static_cast<size_t>(size),
        [](void*, size_t, void*) {}, nullptr);
    args.GetReturnValue().Set(v8::ArrayBuffer::New(isolate, std::move(store)));
}

static void Buffer_Unmap(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    Unwrap<wgpu::Buffer>(args.This()).Unmap();
}

static void Buffer_Destroy(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    Unwrap<wgpu::Buffer>(args.This()).Destroy();
}

static void Buffer_MapAsync(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    wgpu::Buffer& buffer = Unwrap<wgpu::Buffer>(args.This());

    wgpu::MapMode mode = static_cast<wgpu::MapMode>(
        args.Length() > 0 ? static_cast<uint32_t>(args[0].As<v8::Number>()->Value()) : 0);
    uint64_t offset = args.Length() > 1 && args[1]->IsNumber()
        ? static_cast<uint64_t>(args[1].As<v8::Number>()->Value()) : 0;
    uint64_t size = args.Length() > 2 && args[2]->IsNumber()
        ? static_cast<uint64_t>(args[2].As<v8::Number>()->Value())
        : (buffer.GetSize() - offset);

    auto resolver = v8::Promise::Resolver::New(ctx).ToLocalChecked();
    auto* persistent = new v8::Global<v8::Promise::Resolver>(isolate, resolver);
    args.GetReturnValue().Set(resolver->GetPromise());

    buffer.MapAsync(mode, offset, static_cast<size_t>(size),
        wgpu::CallbackMode::AllowProcessEvents,
        [isolate, persistent](wgpu::MapAsyncStatus status, wgpu::StringView) {
            v8::HandleScope scope(isolate);
            v8::Local<v8::Context> c = isolate->GetCurrentContext();
            auto r = persistent->Get(isolate);
            if (status == wgpu::MapAsyncStatus::Success) {
                r->Resolve(c, v8::Undefined(isolate)).Check();
            } else {
                r->Reject(c, v8::Exception::Error(v8util::Str(isolate, "mapAsync failed"))).Check();
            }
            persistent->Reset();
            delete persistent;
        });
}

// ---------------------------------------------------------------------------
// GPUTexture
// ---------------------------------------------------------------------------
static void Texture_CreateView(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    wgpu::Texture& texture = Unwrap<wgpu::Texture>(args.This());

    wgpu::TextureViewDescriptor desc = {};
    if (args.Length() > 0 && args[0]->IsObject()) {
        v8::Local<v8::Object> o = args[0].As<v8::Object>();
        if (HasProp(isolate, o, "format")) {
            desc.format = ToTextureFormat(webgpu::Str(isolate, o, "format"));
        }
        if (HasProp(isolate, o, "dimension")) {
            desc.dimension = ToTextureViewDimension(webgpu::Str(isolate, o, "dimension"));
        }
        if (HasProp(isolate, o, "baseMipLevel")) desc.baseMipLevel = U32(isolate, o, "baseMipLevel");
        if (HasProp(isolate, o, "mipLevelCount")) desc.mipLevelCount = U32(isolate, o, "mipLevelCount");
        if (HasProp(isolate, o, "baseArrayLayer")) desc.baseArrayLayer = U32(isolate, o, "baseArrayLayer");
        if (HasProp(isolate, o, "arrayLayerCount")) desc.arrayLayerCount = U32(isolate, o, "arrayLayerCount");
    }
    args.GetReturnValue().Set(WrapTextureView(isolate, texture.CreateView(&desc)));
}

static void Texture_Destroy(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    Unwrap<wgpu::Texture>(args.This()).Destroy();
}

// ---------------------------------------------------------------------------
// GPUQueue
// ---------------------------------------------------------------------------
static bool CopyBytesFrom(v8::Local<v8::Value> value, const void** data, size_t* length)
{
    if (value->IsArrayBuffer()) {
        auto ab = value.As<v8::ArrayBuffer>();
        auto store = ab->GetBackingStore();
        *data = store->Data();
        *length = store->ByteLength();
        return true;
    }
    if (value->IsArrayBufferView()) {
        auto view = value.As<v8::ArrayBufferView>();
        auto ab = view->Buffer();
        auto store = ab->GetBackingStore();
        *data = static_cast<uint8_t*>(store->Data()) + view->ByteOffset();
        *length = view->ByteLength();
        return true;
    }
    return false;
}

static void Queue_WriteBuffer(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    wgpu::Queue& queue = Unwrap<wgpu::Queue>(args.This());

    wgpu::Buffer& buffer = Unwrap<wgpu::Buffer>(args[0].As<v8::Object>());
    uint64_t buffer_offset = static_cast<uint64_t>(args[1].As<v8::Number>()->Value());

    const void* data = nullptr;
    size_t length = 0;
    if (!CopyBytesFrom(args[2], &data, &length)) {
        v8util::ThrowTypeError(isolate, "writeBuffer: data must be ArrayBuffer(View)");
        return;
    }
    // 任意の dataOffset/size (要素数ではなくバイト) — 簡略化のため未対応時は全体
    uint64_t data_offset = args.Length() > 3 && args[3]->IsNumber()
        ? static_cast<uint64_t>(args[3].As<v8::Number>()->Value()) : 0;
    size_t write_size = args.Length() > 4 && args[4]->IsNumber()
        ? static_cast<size_t>(args[4].As<v8::Number>()->Value())
        : (length - data_offset);

    queue.WriteBuffer(buffer, buffer_offset,
                      static_cast<const uint8_t*>(data) + data_offset, write_size);
}

static wgpu::Extent3D ParseExtent(v8::Isolate* isolate, v8::Local<v8::Value> value)
{
    wgpu::Extent3D extent = {1, 1, 1};
    if (value->IsArray()) {
        auto arr = value.As<v8::Array>();
        v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
        auto get = [&](uint32_t i, uint32_t fb) -> uint32_t {
            v8::Local<v8::Value> v;
            return (i < arr->Length() && arr->Get(ctx, i).ToLocal(&v) && v->IsNumber())
                ? static_cast<uint32_t>(v.As<v8::Number>()->Value()) : fb;
        };
        extent.width = get(0, 1);
        extent.height = get(1, 1);
        extent.depthOrArrayLayers = get(2, 1);
    } else if (value->IsObject()) {
        auto o = value.As<v8::Object>();
        extent.width = U32(isolate, o, "width", 1);
        extent.height = U32(isolate, o, "height", 1);
        extent.depthOrArrayLayers = U32(isolate, o, "depthOrArrayLayers", 1);
    }
    return extent;
}

static void Queue_WriteTexture(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    wgpu::Queue& queue = Unwrap<wgpu::Queue>(args.This());

    // destination: { texture, mipLevel?, origin? }
    v8::Local<v8::Object> dest = args[0].As<v8::Object>();
    wgpu::TexelCopyTextureInfo copy = {};
    copy.texture = Unwrap<wgpu::Texture>(Prop(isolate, dest, "texture").As<v8::Object>());
    copy.mipLevel = U32(isolate, dest, "mipLevel", 0);

    const void* data = nullptr;
    size_t length = 0;
    CopyBytesFrom(args[1], &data, &length);

    // dataLayout: { offset?, bytesPerRow, rowsPerImage? }
    v8::Local<v8::Object> layout = args[2].As<v8::Object>();
    wgpu::TexelCopyBufferLayout data_layout = {};
    data_layout.offset = U64(isolate, layout, "offset", 0);
    data_layout.bytesPerRow = U32(isolate, layout, "bytesPerRow", 0);
    data_layout.rowsPerImage = U32(isolate, layout, "rowsPerImage", wgpu::kCopyStrideUndefined);

    wgpu::Extent3D size = ParseExtent(isolate, args[3]);

    queue.WriteTexture(&copy, data, length, &data_layout, &size);
}

// queue.copyExternalImageToTexture(source, dest, copySize)
// source={source: ImageBitmap|canvas, flipY}, dest={texture, premultipliedAlpha, origin?}
// Dawn の対応 API を使わず、ソースの RGBA を writeTexture で転送する(flipY/premultiply をCPU適用)。
static void Queue_CopyExternalImageToTexture(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    wgpu::Queue& queue = Unwrap<wgpu::Queue>(args.This());

    v8::Local<v8::Object> src = args[0].As<v8::Object>();
    v8::Local<v8::Object> dst = args[1].As<v8::Object>();

    const uint8_t* rgba = nullptr;
    uint32_t sw = 0, sh = 0;
    if (!GetImageSourcePixels(isolate, Prop(isolate, src, "source"), &rgba, &sw, &sh) || !rgba) {
        return;
    }
    const bool flip_y = Bool(isolate, src, "flipY", false);
    const bool premultiply = Bool(isolate, dst, "premultipliedAlpha", false);

    wgpu::Extent3D size = ParseExtent(isolate, args[2]);
    const uint32_t w = size.width ? size.width : sw;
    const uint32_t h = size.height ? size.height : sh;

    // flipY / premultiply を適用したコピーを作る
    std::vector<uint8_t> buffer(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t sy = flip_y ? (h - 1 - y) : y;
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* s = &rgba[(static_cast<size_t>(std::min(sy, sh - 1)) * sw + std::min(x, sw - 1)) * 4];
            uint8_t* d = &buffer[(static_cast<size_t>(y) * w + x) * 4];
            if (premultiply) {
                const double a = s[3] / 255.0;
                d[0] = static_cast<uint8_t>(s[0] * a);
                d[1] = static_cast<uint8_t>(s[1] * a);
                d[2] = static_cast<uint8_t>(s[2] * a);
            } else {
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
            }
            d[3] = s[3];
        }
    }

    wgpu::TexelCopyTextureInfo copy = {};
    copy.texture = Unwrap<wgpu::Texture>(Prop(isolate, dst, "texture").As<v8::Object>());
    wgpu::TexelCopyBufferLayout layout = {};
    layout.bytesPerRow = w * 4;
    layout.rowsPerImage = h;
    wgpu::Extent3D extent = { w, h, 1 };
    queue.WriteTexture(&copy, buffer.data(), buffer.size(), &layout, &extent);
}

static void Queue_Submit(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    wgpu::Queue& queue = Unwrap<wgpu::Queue>(args.This());

    std::vector<wgpu::CommandBuffer> buffers;
    if (args.Length() > 0 && args[0]->IsArray()) {
        auto arr = args[0].As<v8::Array>();
        v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
        for (uint32_t i = 0; i < arr->Length(); ++i) {
            v8::Local<v8::Value> v;
            if (arr->Get(ctx, i).ToLocal(&v) && v->IsObject()) {
                buffers.push_back(Unwrap<wgpu::CommandBuffer>(v.As<v8::Object>()));
            }
        }
    }
    queue.Submit(buffers.size(), buffers.data());
}

// ---------------------------------------------------------------------------
// GPURenderPassEncoder
// ---------------------------------------------------------------------------
static void Pass_SetPipeline(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    Unwrap<wgpu::RenderPassEncoder>(args.This()).SetPipeline(
        Unwrap<wgpu::RenderPipeline>(args[0].As<v8::Object>()));
}

static void Pass_SetBindGroup(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    auto& pass = Unwrap<wgpu::RenderPassEncoder>(args.This());
    uint32_t index = static_cast<uint32_t>(args[0].As<v8::Number>()->Value());
    if (args.Length() < 2 || !args[1]->IsObject()) {
        pass.SetBindGroup(index, nullptr);
        return;
    }
    auto& group = Unwrap<wgpu::BindGroup>(args[1].As<v8::Object>());

    // dynamicOffsets: hasDynamicOffset=true のレイアウトを使う塗り(fill_dynamic)で必須。
    // 3引数目に Array<number> / Uint32Array で渡る。
    std::vector<uint32_t> offsets;
    if (args.Length() > 2 && !args[2]->IsNullOrUndefined()) {
        v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
        if (args[2]->IsArray()) {
            auto arr = args[2].As<v8::Array>();
            offsets.reserve(arr->Length());
            for (uint32_t i = 0; i < arr->Length(); ++i) {
                v8::Local<v8::Value> v;
                if (arr->Get(ctx, i).ToLocal(&v) && v->IsNumber())
                    offsets.push_back(static_cast<uint32_t>(v.As<v8::Number>()->Value()));
            }
        } else if (args[2]->IsUint32Array()) {
            auto ta = args[2].As<v8::Uint32Array>();
            offsets.resize(ta->Length());
            ta->CopyContents(offsets.data(), offsets.size() * sizeof(uint32_t));
        }
    }
    pass.SetBindGroup(index, group,
                      static_cast<uint32_t>(offsets.size()),
                      offsets.empty() ? nullptr : offsets.data());
}

static void Pass_SetStencilReference(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    auto& pass = Unwrap<wgpu::RenderPassEncoder>(args.This());
    uint32_t reference = (args.Length() > 0 && args[0]->IsNumber())
        ? static_cast<uint32_t>(args[0].As<v8::Number>()->Value()) : 0;
    pass.SetStencilReference(reference);
}

static void Pass_SetBlendConstant(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    auto& pass = Unwrap<wgpu::RenderPassEncoder>(args.This());
    wgpu::Color color = {};
    if (args.Length() > 0 && args[0]->IsObject()) {
        auto o = args[0].As<v8::Object>();
        color = { F64(isolate, o, "r"), F64(isolate, o, "g"),
                  F64(isolate, o, "b"), F64(isolate, o, "a") };
    } else if (args.Length() > 0 && args[0]->IsArray()) {
        v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
        auto c = args[0].As<v8::Array>();
        auto g = [&](uint32_t k) { v8::Local<v8::Value> x;
            return (c->Get(ctx, k).ToLocal(&x) && x->IsNumber()) ? x.As<v8::Number>()->Value() : 0.0; };
        color = { g(0), g(1), g(2), g(3) };
    }
    pass.SetBlendConstant(&color);
}

static void Pass_SetVertexBuffer(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    auto& pass = Unwrap<wgpu::RenderPassEncoder>(args.This());
    uint32_t slot = static_cast<uint32_t>(args[0].As<v8::Number>()->Value());
    auto& buffer = Unwrap<wgpu::Buffer>(args[1].As<v8::Object>());
    uint64_t offset = args.Length() > 2 && args[2]->IsNumber()
        ? static_cast<uint64_t>(args[2].As<v8::Number>()->Value()) : 0;
    uint64_t size = args.Length() > 3 && args[3]->IsNumber()
        ? static_cast<uint64_t>(args[3].As<v8::Number>()->Value()) : wgpu::kWholeSize;
    (void) isolate;
    pass.SetVertexBuffer(slot, buffer, offset, size);
}

static void Pass_SetIndexBuffer(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    auto& pass = Unwrap<wgpu::RenderPassEncoder>(args.This());
    auto& buffer = Unwrap<wgpu::Buffer>(args[0].As<v8::Object>());
    wgpu::IndexFormat format = ToIndexFormat(v8util::ToStdString(isolate, args[1]));
    uint64_t offset = args.Length() > 2 && args[2]->IsNumber()
        ? static_cast<uint64_t>(args[2].As<v8::Number>()->Value()) : 0;
    uint64_t size = args.Length() > 3 && args[3]->IsNumber()
        ? static_cast<uint64_t>(args[3].As<v8::Number>()->Value()) : wgpu::kWholeSize;
    pass.SetIndexBuffer(buffer, format, offset, size);
}

static void Pass_SetViewport(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    auto& pass = Unwrap<wgpu::RenderPassEncoder>(args.This());
    auto n = [&](int i) { return static_cast<float>(args[i].As<v8::Number>()->Value()); };
    pass.SetViewport(n(0), n(1), n(2), n(3), n(4), n(5));
}

static void Pass_SetScissorRect(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    auto& pass = Unwrap<wgpu::RenderPassEncoder>(args.This());
    auto n = [&](int i) { return static_cast<uint32_t>(args[i].As<v8::Number>()->Value()); };
    pass.SetScissorRect(n(0), n(1), n(2), n(3));
}

static void Pass_Draw(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    auto& pass = Unwrap<wgpu::RenderPassEncoder>(args.This());
    auto n = [&](int i, uint32_t fb) {
        return (i < args.Length() && args[i]->IsNumber())
            ? static_cast<uint32_t>(args[i].As<v8::Number>()->Value()) : fb; };
    pass.Draw(n(0, 0), n(1, 1), n(2, 0), n(3, 0));
}

static void Pass_DrawIndexed(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    auto& pass = Unwrap<wgpu::RenderPassEncoder>(args.This());
    auto n = [&](int i, uint32_t fb) {
        return (i < args.Length() && args[i]->IsNumber())
            ? static_cast<uint32_t>(args[i].As<v8::Number>()->Value()) : fb; };
    pass.DrawIndexed(n(0, 0), n(1, 1), n(2, 0), n(3, 0), n(4, 0));
}

static void Pass_DrawIndirect(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    auto& pass = Unwrap<wgpu::RenderPassEncoder>(args.This());
    auto& buffer = Unwrap<wgpu::Buffer>(args[0].As<v8::Object>());
    uint64_t offset = args.Length() > 1 && args[1]->IsNumber()
        ? static_cast<uint64_t>(args[1].As<v8::Number>()->Value()) : 0;
    pass.DrawIndirect(buffer, offset);
}

static void Pass_End(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    Unwrap<wgpu::RenderPassEncoder>(args.This()).End();
}

static v8::Local<v8::Object> WrapRenderPass(v8::Isolate* isolate, wgpu::RenderPassEncoder pass)
{
    v8::Local<v8::Object> obj = WrapWith(isolate, Tmpl(isolate).renderPass, std::move(pass));
    SetMethod(isolate, obj, "setPipeline", Pass_SetPipeline);
    SetMethod(isolate, obj, "setBindGroup", Pass_SetBindGroup);
    SetMethod(isolate, obj, "setVertexBuffer", Pass_SetVertexBuffer);
    SetMethod(isolate, obj, "setIndexBuffer", Pass_SetIndexBuffer);
    SetMethod(isolate, obj, "setViewport", Pass_SetViewport);
    SetMethod(isolate, obj, "setScissorRect", Pass_SetScissorRect);
    SetMethod(isolate, obj, "setStencilReference", Pass_SetStencilReference);
    SetMethod(isolate, obj, "setBlendConstant", Pass_SetBlendConstant);
    SetMethod(isolate, obj, "draw", Pass_Draw);
    SetMethod(isolate, obj, "drawIndexed", Pass_DrawIndexed);
    SetMethod(isolate, obj, "drawIndirect", Pass_DrawIndirect);
    SetMethod(isolate, obj, "end", Pass_End);
    return obj;
}

// ---------------------------------------------------------------------------
// GPUCommandEncoder
// ---------------------------------------------------------------------------
static void Encoder_BeginRenderPass(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    auto& encoder = Unwrap<wgpu::CommandEncoder>(args.This());
    v8::Local<v8::Object> desc = args[0].As<v8::Object>();

    std::vector<wgpu::RenderPassColorAttachment> color_attachments;
    v8::Local<v8::Value> ca = Prop(isolate, desc, "colorAttachments");
    if (ca->IsArray()) {
        auto arr = ca.As<v8::Array>();
        for (uint32_t i = 0; i < arr->Length(); ++i) {
            v8::Local<v8::Value> v;
            if (!arr->Get(ctx, i).ToLocal(&v) || !v->IsObject()) continue;
            auto o = v.As<v8::Object>();
            wgpu::RenderPassColorAttachment att = {};
            att.view = Unwrap<wgpu::TextureView>(Prop(isolate, o, "view").As<v8::Object>());
            att.loadOp = ToLoadOp(webgpu::Str(isolate, o, "loadOp"));
            att.storeOp = ToStoreOp(webgpu::Str(isolate, o, "storeOp"));
            att.depthSlice = wgpu::kDepthSliceUndefined;
            v8::Local<v8::Value> cv = Prop(isolate, o, "clearValue");
            if (cv->IsArray()) {
                auto c = cv.As<v8::Array>();
                auto g = [&](uint32_t k) { v8::Local<v8::Value> x;
                    return (c->Get(ctx, k).ToLocal(&x) && x->IsNumber()) ? x.As<v8::Number>()->Value() : 0.0; };
                att.clearValue = { g(0), g(1), g(2), g(3) };
            } else if (cv->IsObject()) {
                auto c = cv.As<v8::Object>();
                att.clearValue = { F64(isolate, c, "r"), F64(isolate, c, "g"),
                                   F64(isolate, c, "b"), F64(isolate, c, "a") };
            }
            color_attachments.push_back(att);
        }
    }

    wgpu::RenderPassDescriptor pass_desc = {};
    pass_desc.colorAttachmentCount = color_attachments.size();
    pass_desc.colorAttachments = color_attachments.data();

    // depthStencilAttachment
    wgpu::RenderPassDepthStencilAttachment depth = {};
    v8::Local<v8::Value> ds = Prop(isolate, desc, "depthStencilAttachment");
    if (ds->IsObject()) {
        auto o = ds.As<v8::Object>();
        depth.view = Unwrap<wgpu::TextureView>(Prop(isolate, o, "view").As<v8::Object>());
        if (HasProp(isolate, o, "depthLoadOp")) {
            depth.depthLoadOp = ToLoadOp(webgpu::Str(isolate, o, "depthLoadOp"));
            depth.depthStoreOp = ToStoreOp(webgpu::Str(isolate, o, "depthStoreOp"));
            depth.depthClearValue = static_cast<float>(F64(isolate, o, "depthClearValue", 1.0));
        }
        // stencil アスペクト (マスク描画で使用)
        if (HasProp(isolate, o, "stencilLoadOp")) {
            depth.stencilLoadOp = ToLoadOp(webgpu::Str(isolate, o, "stencilLoadOp"));
            depth.stencilStoreOp = ToStoreOp(webgpu::Str(isolate, o, "stencilStoreOp"));
            depth.stencilClearValue = U32(isolate, o, "stencilClearValue", 0);
        }
        if (Bool(isolate, o, "depthReadOnly", false))
            depth.depthReadOnly = true;
        if (Bool(isolate, o, "stencilReadOnly", false))
            depth.stencilReadOnly = true;
        pass_desc.depthStencilAttachment = &depth;
    }

    args.GetReturnValue().Set(
        WrapRenderPass(isolate, encoder.BeginRenderPass(&pass_desc)));
}

static void Encoder_Finish(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    args.GetReturnValue().Set(
        WrapWith(isolate, Tmpl(isolate).commandBuffer,
                 Unwrap<wgpu::CommandEncoder>(args.This()).Finish()));
}

static void Encoder_CopyBufferToBuffer(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    auto& encoder = Unwrap<wgpu::CommandEncoder>(args.This());
    auto& src = Unwrap<wgpu::Buffer>(args[0].As<v8::Object>());
    uint64_t src_off = static_cast<uint64_t>(args[1].As<v8::Number>()->Value());
    auto& dst = Unwrap<wgpu::Buffer>(args[2].As<v8::Object>());
    uint64_t dst_off = static_cast<uint64_t>(args[3].As<v8::Number>()->Value());
    uint64_t size = static_cast<uint64_t>(args[4].As<v8::Number>()->Value());
    encoder.CopyBufferToBuffer(src, src_off, dst, dst_off, size);
}

// {texture, mipLevel?, origin?} を wgpu::TexelCopyTextureInfo へ
static wgpu::TexelCopyTextureInfo ParseTexelCopyTexture(v8::Isolate* isolate, v8::Local<v8::Object> o)
{
    wgpu::TexelCopyTextureInfo info = {};
    info.texture = Unwrap<wgpu::Texture>(Prop(isolate, o, "texture").As<v8::Object>());
    info.mipLevel = U32(isolate, o, "mipLevel", 0);
    v8::Local<v8::Value> origin = Prop(isolate, o, "origin");
    if (origin->IsObject() || origin->IsArray()) {
        wgpu::Extent3D e = ParseExtent(isolate, origin);
        info.origin = { e.width, e.height, e.depthOrArrayLayers };
    }
    return info;
}

static void Encoder_CopyTextureToTexture(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    auto& encoder = Unwrap<wgpu::CommandEncoder>(args.This());
    wgpu::TexelCopyTextureInfo src = ParseTexelCopyTexture(isolate, args[0].As<v8::Object>());
    wgpu::TexelCopyTextureInfo dst = ParseTexelCopyTexture(isolate, args[1].As<v8::Object>());
    wgpu::Extent3D size = ParseExtent(isolate, args[2]);
    encoder.CopyTextureToTexture(&src, &dst, &size);
}

static void Encoder_CopyTextureToBuffer(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    auto& encoder = Unwrap<wgpu::CommandEncoder>(args.This());
    wgpu::TexelCopyTextureInfo src = ParseTexelCopyTexture(isolate, args[0].As<v8::Object>());

    v8::Local<v8::Object> d = args[1].As<v8::Object>();
    wgpu::TexelCopyBufferInfo dst = {};
    dst.buffer = Unwrap<wgpu::Buffer>(Prop(isolate, d, "buffer").As<v8::Object>());
    dst.layout.offset = U64(isolate, d, "offset", 0);
    dst.layout.bytesPerRow = U32(isolate, d, "bytesPerRow", 0);
    dst.layout.rowsPerImage = U32(isolate, d, "rowsPerImage", wgpu::kCopyStrideUndefined);

    wgpu::Extent3D size = ParseExtent(isolate, args[2]);
    encoder.CopyTextureToBuffer(&src, &dst, &size);
}

// ---------------------------------------------------------------------------
// GPUCanvasContext
// ---------------------------------------------------------------------------
static void Ctx_Configure(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    DawnContext* gpu = HostContext::From(isolate)->gpu;
    // device は既に DawnContext が保持。フォーマット/サイズのみ反映する。
    if (args.Length() > 0 && args[0]->IsObject()) {
        // width/height は canvas サイズを使用 (configure の size は任意)
        gpu->Configure(gpu->width(), gpu->height());
    }
}

static void Ctx_GetCurrentTexture(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    DawnContext* gpu = HostContext::From(isolate)->gpu;
    args.GetReturnValue().Set(WrapTexture(isolate, gpu->GetCurrentTexture()));
}

static void Ctx_GetConfiguredFormat(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    // getPreferredCanvasFormat 相当。DawnContext の surface フォーマットを返す。
    v8::Isolate* isolate = args.GetIsolate();
    DawnContext* gpu = HostContext::From(isolate)->gpu;
    const char* fmt = (gpu->format() == wgpu::TextureFormat::RGBA8Unorm)
        ? "rgba8unorm" : "bgra8unorm";
    args.GetReturnValue().Set(v8util::Str(isolate, fmt));
}

// タグ付きラップ (bind group で resource 種別を判定するため)
static v8::Local<v8::Object> WrapTextureView(v8::Isolate* isolate, wgpu::TextureView view)
{
    auto obj = WrapWith(isolate, Tmpl(isolate).textureView, std::move(view));
    SetValue(isolate, obj, "__gpuType", Str(isolate, "textureView"));
    return obj;
}

static v8::Local<v8::Object> WrapSampler(v8::Isolate* isolate, wgpu::Sampler sampler)
{
    auto obj = WrapWith(isolate, Tmpl(isolate).sampler, std::move(sampler));
    SetValue(isolate, obj, "__gpuType", Str(isolate, "sampler"));
    return obj;
}

// ---------------------------------------------------------------------------
// GPUDevice.createBuffer
// ---------------------------------------------------------------------------
static void Device_CreateBuffer(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    auto& device = Unwrap<wgpu::Device>(args.This());
    v8::Local<v8::Object> d = args[0].As<v8::Object>();

    wgpu::BufferDescriptor desc = {};
    desc.size = U64(isolate, d, "size", 0);
    desc.usage = static_cast<wgpu::BufferUsage>(U32(isolate, d, "usage", 0));
    desc.mappedAtCreation = Bool(isolate, d, "mappedAtCreation", false);

    args.GetReturnValue().Set(WrapBuffer(isolate, device.CreateBuffer(&desc)));
}

// ---------------------------------------------------------------------------
// GPUDevice.createTexture
// ---------------------------------------------------------------------------
static void Device_CreateTexture(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    auto& device = Unwrap<wgpu::Device>(args.This());
    v8::Local<v8::Object> d = args[0].As<v8::Object>();

    wgpu::TextureDescriptor desc = {};
    desc.size = ParseExtent(isolate, Prop(isolate, d, "size"));
    desc.format = ToTextureFormat(webgpu::Str(isolate, d, "format"));
    desc.usage = static_cast<wgpu::TextureUsage>(U32(isolate, d, "usage", 0));
    desc.dimension = HasProp(isolate, d, "dimension")
        ? ToTextureDimension(webgpu::Str(isolate, d, "dimension"))
        : wgpu::TextureDimension::e2D;
    desc.mipLevelCount = U32(isolate, d, "mipLevelCount", 1);
    desc.sampleCount = U32(isolate, d, "sampleCount", 1);

    args.GetReturnValue().Set(WrapTexture(isolate, device.CreateTexture(&desc)));
}

// ---------------------------------------------------------------------------
// GPUDevice.createSampler
// ---------------------------------------------------------------------------
static void Device_CreateSampler(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    auto& device = Unwrap<wgpu::Device>(args.This());

    wgpu::SamplerDescriptor desc = {};
    if (args.Length() > 0 && args[0]->IsObject()) {
        auto d = args[0].As<v8::Object>();
        desc.addressModeU = ToAddressMode(webgpu::Str(isolate, d, "addressModeU"));
        desc.addressModeV = ToAddressMode(webgpu::Str(isolate, d, "addressModeV"));
        desc.addressModeW = ToAddressMode(webgpu::Str(isolate, d, "addressModeW"));
        desc.magFilter = ToFilterMode(webgpu::Str(isolate, d, "magFilter"));
        desc.minFilter = ToFilterMode(webgpu::Str(isolate, d, "minFilter"));
        desc.mipmapFilter = ToMipmapFilterMode(webgpu::Str(isolate, d, "mipmapFilter"));
        if (HasProp(isolate, d, "lodMinClamp")) desc.lodMinClamp = static_cast<float>(F64(isolate, d, "lodMinClamp"));
        if (HasProp(isolate, d, "lodMaxClamp")) desc.lodMaxClamp = static_cast<float>(F64(isolate, d, "lodMaxClamp", 32.f));
        if (HasProp(isolate, d, "maxAnisotropy")) desc.maxAnisotropy = static_cast<uint16_t>(U32(isolate, d, "maxAnisotropy", 1));
    }
    args.GetReturnValue().Set(WrapSampler(isolate, device.CreateSampler(&desc)));
}

// ---------------------------------------------------------------------------
// GPUDevice.createShaderModule
// ---------------------------------------------------------------------------
static void Device_CreateShaderModule(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    auto& device = Unwrap<wgpu::Device>(args.This());
    v8::Local<v8::Object> d = args[0].As<v8::Object>();

    const std::string code = webgpu::Str(isolate, d, "code");

    wgpu::ShaderSourceWGSL wgsl = {};
    wgsl.code = wgpu::StringView(code.c_str(), code.size());

    wgpu::ShaderModuleDescriptor desc = {};
    desc.nextInChain = &wgsl;

    args.GetReturnValue().Set(
        WrapWith(isolate, Tmpl(isolate).shaderModule, device.CreateShaderModule(&desc)));
}

// ---------------------------------------------------------------------------
// GPUDevice.createBindGroupLayout
// ---------------------------------------------------------------------------
static void Device_CreateBindGroupLayout(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    auto& device = Unwrap<wgpu::Device>(args.This());
    v8::Local<v8::Object> d = args[0].As<v8::Object>();

    std::vector<wgpu::BindGroupLayoutEntry> entries;
    v8::Local<v8::Value> ev = Prop(isolate, d, "entries");
    if (ev->IsArray()) {
        auto arr = ev.As<v8::Array>();
        for (uint32_t i = 0; i < arr->Length(); ++i) {
            v8::Local<v8::Value> v;
            if (!arr->Get(ctx, i).ToLocal(&v) || !v->IsObject()) continue;
            auto o = v.As<v8::Object>();
            wgpu::BindGroupLayoutEntry entry = {};
            entry.binding = U32(isolate, o, "binding", 0);
            entry.visibility = static_cast<wgpu::ShaderStage>(U32(isolate, o, "visibility", 0));

            if (HasProp(isolate, o, "buffer")) {
                auto b = Prop(isolate, o, "buffer").As<v8::Object>();
                entry.buffer.type = ToBufferBindingType(webgpu::Str(isolate, b, "type"));
                entry.buffer.hasDynamicOffset = Bool(isolate, b, "hasDynamicOffset", false);
                entry.buffer.minBindingSize = U64(isolate, b, "minBindingSize", 0);
            } else if (HasProp(isolate, o, "sampler")) {
                auto s = Prop(isolate, o, "sampler").As<v8::Object>();
                entry.sampler.type = ToSamplerBindingType(webgpu::Str(isolate, s, "type"));
            } else if (HasProp(isolate, o, "texture")) {
                auto t = Prop(isolate, o, "texture").As<v8::Object>();
                entry.texture.sampleType = ToTextureSampleType(webgpu::Str(isolate, t, "sampleType"));
                entry.texture.viewDimension = HasProp(isolate, t, "viewDimension")
                    ? ToTextureViewDimension(webgpu::Str(isolate, t, "viewDimension"))
                    : wgpu::TextureViewDimension::e2D;
                entry.texture.multisampled = Bool(isolate, t, "multisampled", false);
            }
            // «EXTEND» storageTexture / externalTexture は必要に応じて追加
            entries.push_back(entry);
        }
    }

    wgpu::BindGroupLayoutDescriptor desc = {};
    desc.entryCount = entries.size();
    desc.entries = entries.data();

    args.GetReturnValue().Set(
        WrapWith(isolate, Tmpl(isolate).bindGroupLayout, device.CreateBindGroupLayout(&desc)));
}

// ---------------------------------------------------------------------------
// GPUDevice.createBindGroup
// ---------------------------------------------------------------------------
static void Device_CreateBindGroup(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    auto& device = Unwrap<wgpu::Device>(args.This());
    v8::Local<v8::Object> d = args[0].As<v8::Object>();

    std::vector<wgpu::BindGroupEntry> entries;
    v8::Local<v8::Value> ev = Prop(isolate, d, "entries");
    if (ev->IsArray()) {
        auto arr = ev.As<v8::Array>();
        for (uint32_t i = 0; i < arr->Length(); ++i) {
            v8::Local<v8::Value> v;
            if (!arr->Get(ctx, i).ToLocal(&v) || !v->IsObject()) continue;
            auto o = v.As<v8::Object>();
            wgpu::BindGroupEntry entry = {};
            entry.binding = U32(isolate, o, "binding", 0);

            v8::Local<v8::Value> res = Prop(isolate, o, "resource");
            if (res->IsObject()) {
                auto ro = res.As<v8::Object>();
                if (HasProp(isolate, ro, "buffer")) {
                    entry.buffer = Unwrap<wgpu::Buffer>(Prop(isolate, ro, "buffer").As<v8::Object>());
                    entry.offset = U64(isolate, ro, "offset", 0);
                    if (HasProp(isolate, ro, "size")) entry.size = U64(isolate, ro, "size", 0);
                    else entry.size = wgpu::kWholeSize;
                } else {
                    const std::string type = webgpu::Str(isolate, ro, "__gpuType");
                    if (type == "sampler") {
                        entry.sampler = Unwrap<wgpu::Sampler>(ro);
                    } else {
                        entry.textureView = Unwrap<wgpu::TextureView>(ro);
                    }
                }
            }
            entries.push_back(entry);
        }
    }

    wgpu::BindGroupDescriptor desc = {};
    desc.layout = Unwrap<wgpu::BindGroupLayout>(Prop(isolate, d, "layout").As<v8::Object>());
    desc.entryCount = entries.size();
    desc.entries = entries.data();

    args.GetReturnValue().Set(
        WrapWith(isolate, Tmpl(isolate).bindGroup, device.CreateBindGroup(&desc)));
}

// ---------------------------------------------------------------------------
// GPUDevice.createPipelineLayout
// ---------------------------------------------------------------------------
static void Device_CreatePipelineLayout(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    auto& device = Unwrap<wgpu::Device>(args.This());
    v8::Local<v8::Object> d = args[0].As<v8::Object>();

    std::vector<wgpu::BindGroupLayout> layouts;
    v8::Local<v8::Value> lv = Prop(isolate, d, "bindGroupLayouts");
    if (lv->IsArray()) {
        auto arr = lv.As<v8::Array>();
        for (uint32_t i = 0; i < arr->Length(); ++i) {
            v8::Local<v8::Value> v;
            if (arr->Get(ctx, i).ToLocal(&v) && v->IsObject()) {
                layouts.push_back(Unwrap<wgpu::BindGroupLayout>(v.As<v8::Object>()));
            }
        }
    }

    wgpu::PipelineLayoutDescriptor desc = {};
    desc.bindGroupLayoutCount = layouts.size();
    desc.bindGroupLayouts = layouts.data();

    args.GetReturnValue().Set(
        WrapWith(isolate, Tmpl(isolate).pipelineLayout, device.CreatePipelineLayout(&desc)));
}

// ---------------------------------------------------------------------------
// GPUDevice.createRenderPipeline
// ---------------------------------------------------------------------------
// ディスクリプタ内で参照する一時ストレージ (CreateRenderPipeline は同期コピーするため
// 呼び出しが終わるまでこれらの vector を生存させる)。
struct RenderPipelineScratch {
    std::vector<std::vector<wgpu::VertexAttribute>> attributes;
    std::vector<wgpu::VertexBufferLayout> vertex_buffers;
    std::vector<wgpu::ColorTargetState> targets;
    std::vector<wgpu::BlendState> blends;
    std::string vs_entry, fs_entry;
};

static void Device_CreateRenderPipeline(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    auto& device = Unwrap<wgpu::Device>(args.This());
    v8::Local<v8::Object> d = args[0].As<v8::Object>();

    RenderPipelineScratch scratch;
    wgpu::RenderPipelineDescriptor desc = {};

    // layout: 'auto' or GPUPipelineLayout
    v8::Local<v8::Value> layout = Prop(isolate, d, "layout");
    if (layout->IsObject()) {
        desc.layout = Unwrap<wgpu::PipelineLayout>(layout.As<v8::Object>());
    } // 'auto' の場合は nullptr のまま

    // vertex
    v8::Local<v8::Object> vertex = Prop(isolate, d, "vertex").As<v8::Object>();
    desc.vertex.module = Unwrap<wgpu::ShaderModule>(Prop(isolate, vertex, "module").As<v8::Object>());
    scratch.vs_entry = webgpu::Str(isolate, vertex, "entryPoint");
    if (!scratch.vs_entry.empty()) {
        desc.vertex.entryPoint = wgpu::StringView(scratch.vs_entry.c_str(), scratch.vs_entry.size());
    }

    v8::Local<v8::Value> vbs = Prop(isolate, vertex, "buffers");
    if (vbs->IsArray()) {
        auto arr = vbs.As<v8::Array>();
        for (uint32_t i = 0; i < arr->Length(); ++i) {
            v8::Local<v8::Value> v;
            if (!arr->Get(ctx, i).ToLocal(&v) || !v->IsObject()) continue;
            auto o = v.As<v8::Object>();

            std::vector<wgpu::VertexAttribute> attrs;
            v8::Local<v8::Value> av = Prop(isolate, o, "attributes");
            if (av->IsArray()) {
                auto aarr = av.As<v8::Array>();
                for (uint32_t j = 0; j < aarr->Length(); ++j) {
                    v8::Local<v8::Value> a;
                    if (!aarr->Get(ctx, j).ToLocal(&a) || !a->IsObject()) continue;
                    auto ao = a.As<v8::Object>();
                    wgpu::VertexAttribute attr = {};
                    attr.format = ToVertexFormat(webgpu::Str(isolate, ao, "format"));
                    attr.offset = U64(isolate, ao, "offset", 0);
                    attr.shaderLocation = U32(isolate, ao, "shaderLocation", 0);
                    attrs.push_back(attr);
                }
            }
            scratch.attributes.push_back(std::move(attrs));

            wgpu::VertexBufferLayout vbl = {};
            vbl.arrayStride = U64(isolate, o, "arrayStride", 0);
            vbl.stepMode = HasProp(isolate, o, "stepMode")
                ? ToStepMode(webgpu::Str(isolate, o, "stepMode")) : wgpu::VertexStepMode::Vertex;
            vbl.attributeCount = scratch.attributes.back().size();
            vbl.attributes = scratch.attributes.back().data();
            scratch.vertex_buffers.push_back(vbl);
        }
        desc.vertex.bufferCount = scratch.vertex_buffers.size();
        desc.vertex.buffers = scratch.vertex_buffers.data();
    }

    // primitive
    v8::Local<v8::Value> prim = Prop(isolate, d, "primitive");
    if (prim->IsObject()) {
        auto p = prim.As<v8::Object>();
        desc.primitive.topology = ToTopology(webgpu::Str(isolate, p, "topology"));
        if (HasProp(isolate, p, "stripIndexFormat"))
            desc.primitive.stripIndexFormat = ToIndexFormat(webgpu::Str(isolate, p, "stripIndexFormat"));
        if (HasProp(isolate, p, "frontFace"))
            desc.primitive.frontFace = ToFrontFace(webgpu::Str(isolate, p, "frontFace"));
        if (HasProp(isolate, p, "cullMode"))
            desc.primitive.cullMode = ToCullMode(webgpu::Str(isolate, p, "cullMode"));
    }

    // depthStencil (マスク処理は stencil8 + stencilFront/Back を使う)
    wgpu::DepthStencilState depth = {};
    v8::Local<v8::Value> dsv = Prop(isolate, d, "depthStencil");
    if (dsv->IsObject()) {
        auto ds = dsv.As<v8::Object>();
        depth.format = ToTextureFormat(webgpu::Str(isolate, ds, "format"));

        // depth アスペクトを持つフォーマットのときのみ depthWriteEnabled/depthCompare を設定する。
        // stencil8 (depth なし) では両者を未設定(Undefined)のままにしないと検証エラーになる。
        const bool has_depth = depth.format == wgpu::TextureFormat::Depth16Unorm
            || depth.format == wgpu::TextureFormat::Depth24Plus
            || depth.format == wgpu::TextureFormat::Depth24PlusStencil8
            || depth.format == wgpu::TextureFormat::Depth32Float
            || depth.format == wgpu::TextureFormat::Depth32FloatStencil8;
        if (has_depth) {
            depth.depthWriteEnabled = Bool(isolate, ds, "depthWriteEnabled", false)
                ? wgpu::OptionalBool::True : wgpu::OptionalBool::False;
            depth.depthCompare = HasProp(isolate, ds, "depthCompare")
                ? ToCompareFunction(webgpu::Str(isolate, ds, "depthCompare"))
                : wgpu::CompareFunction::Always;
        }

        auto parseFace = [&](const char* key, wgpu::StencilFaceState& face) {
            v8::Local<v8::Value> fv = Prop(isolate, ds, key);
            if (!fv->IsObject()) return;
            auto f = fv.As<v8::Object>();
            if (HasProp(isolate, f, "compare"))
                face.compare = ToCompareFunction(webgpu::Str(isolate, f, "compare"));
            if (HasProp(isolate, f, "failOp"))
                face.failOp = ToStencilOperation(webgpu::Str(isolate, f, "failOp"));
            if (HasProp(isolate, f, "depthFailOp"))
                face.depthFailOp = ToStencilOperation(webgpu::Str(isolate, f, "depthFailOp"));
            if (HasProp(isolate, f, "passOp"))
                face.passOp = ToStencilOperation(webgpu::Str(isolate, f, "passOp"));
        };
        parseFace("stencilFront", depth.stencilFront);
        parseFace("stencilBack", depth.stencilBack);

        if (HasProp(isolate, ds, "stencilReadMask"))
            depth.stencilReadMask = U32(isolate, ds, "stencilReadMask", 0xFFFFFFFF);
        if (HasProp(isolate, ds, "stencilWriteMask"))
            depth.stencilWriteMask = U32(isolate, ds, "stencilWriteMask", 0xFFFFFFFF);

        desc.depthStencil = &depth;
    }

    // multisample
    v8::Local<v8::Value> ms = Prop(isolate, d, "multisample");
    if (ms->IsObject()) {
        auto m = ms.As<v8::Object>();
        desc.multisample.count = U32(isolate, m, "count", 1);
    }

    // fragment
    wgpu::FragmentState fragment = {};
    v8::Local<v8::Value> fv = Prop(isolate, d, "fragment");
    if (fv->IsObject()) {
        auto f = fv.As<v8::Object>();
        fragment.module = Unwrap<wgpu::ShaderModule>(Prop(isolate, f, "module").As<v8::Object>());
        scratch.fs_entry = webgpu::Str(isolate, f, "entryPoint");
        if (!scratch.fs_entry.empty()) {
            fragment.entryPoint = wgpu::StringView(scratch.fs_entry.c_str(), scratch.fs_entry.size());
        }

        v8::Local<v8::Value> tv = Prop(isolate, f, "targets");
        if (tv->IsArray()) {
            auto arr = tv.As<v8::Array>();
            scratch.blends.resize(arr->Length());
            for (uint32_t i = 0; i < arr->Length(); ++i) {
                v8::Local<v8::Value> v;
                if (!arr->Get(ctx, i).ToLocal(&v) || !v->IsObject()) { scratch.targets.push_back({}); continue; }
                auto o = v.As<v8::Object>();
                wgpu::ColorTargetState target = {};
                target.format = ToTextureFormat(webgpu::Str(isolate, o, "format"));
                if (HasProp(isolate, o, "writeMask"))
                    target.writeMask = static_cast<wgpu::ColorWriteMask>(U32(isolate, o, "writeMask", 0xF));

                v8::Local<v8::Value> bv = Prop(isolate, o, "blend");
                if (bv->IsObject()) {
                    auto b = bv.As<v8::Object>();
                    wgpu::BlendState& blend = scratch.blends[i];
                    auto color = Prop(isolate, b, "color").As<v8::Object>();
                    auto alpha = Prop(isolate, b, "alpha").As<v8::Object>();
                    blend.color.srcFactor = ToBlendFactor(webgpu::Str(isolate, color, "srcFactor"));
                    blend.color.dstFactor = ToBlendFactor(webgpu::Str(isolate, color, "dstFactor"));
                    blend.color.operation = ToBlendOperation(webgpu::Str(isolate, color, "operation"));
                    blend.alpha.srcFactor = ToBlendFactor(webgpu::Str(isolate, alpha, "srcFactor"));
                    blend.alpha.dstFactor = ToBlendFactor(webgpu::Str(isolate, alpha, "dstFactor"));
                    blend.alpha.operation = ToBlendOperation(webgpu::Str(isolate, alpha, "operation"));
                    target.blend = &blend;
                }
                scratch.targets.push_back(target);
            }
            fragment.targetCount = scratch.targets.size();
            fragment.targets = scratch.targets.data();
        }
        desc.fragment = &fragment;
    }

    args.GetReturnValue().Set(WrapPipeline(isolate, device.CreateRenderPipeline(&desc)));
}

static void Device_CreateCommandEncoder(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    auto& device = Unwrap<wgpu::Device>(args.This());
    args.GetReturnValue().Set(WrapEncoder(isolate, device.CreateCommandEncoder()));
}

static void Pipeline_GetBindGroupLayout(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    auto& pipeline = Unwrap<wgpu::RenderPipeline>(args.This());
    uint32_t index = args.Length() > 0 && args[0]->IsNumber()
        ? static_cast<uint32_t>(args[0].As<v8::Number>()->Value()) : 0;
    args.GetReturnValue().Set(
        WrapWith(isolate, Tmpl(isolate).bindGroupLayout, pipeline.GetBindGroupLayout(index)));
}

// ---------------------------------------------------------------------------
// メソッド付与済みラップ (前方宣言の定義)
// ---------------------------------------------------------------------------
static v8::Local<v8::Object> WrapBuffer(v8::Isolate* isolate, wgpu::Buffer buffer)
{
    auto obj = WrapWith(isolate, Tmpl(isolate).buffer, std::move(buffer));
    SetValue(isolate, obj, "size", v8::Number::New(isolate,
        static_cast<double>(Unwrap<wgpu::Buffer>(obj).GetSize())));
    SetMethod(isolate, obj, "getMappedRange", Buffer_GetMappedRange);
    SetMethod(isolate, obj, "unmap", Buffer_Unmap);
    SetMethod(isolate, obj, "mapAsync", Buffer_MapAsync);
    SetMethod(isolate, obj, "destroy", Buffer_Destroy);
    return obj;
}

static v8::Local<v8::Object> WrapTexture(v8::Isolate* isolate, wgpu::Texture texture)
{
    auto obj = WrapWith(isolate, Tmpl(isolate).texture, std::move(texture));
    auto& t = Unwrap<wgpu::Texture>(obj);
    SetValue(isolate, obj, "width", v8::Integer::NewFromUnsigned(isolate, t.GetWidth()));
    SetValue(isolate, obj, "height", v8::Integer::NewFromUnsigned(isolate, t.GetHeight()));
    SetMethod(isolate, obj, "createView", Texture_CreateView);
    SetMethod(isolate, obj, "destroy", Texture_Destroy);
    return obj;
}

static v8::Local<v8::Object> WrapEncoder(v8::Isolate* isolate, wgpu::CommandEncoder encoder)
{
    auto obj = WrapWith(isolate, Tmpl(isolate).commandEncoder, std::move(encoder));
    SetMethod(isolate, obj, "beginRenderPass", Encoder_BeginRenderPass);
    SetMethod(isolate, obj, "finish", Encoder_Finish);
    SetMethod(isolate, obj, "copyBufferToBuffer", Encoder_CopyBufferToBuffer);
    SetMethod(isolate, obj, "copyTextureToTexture", Encoder_CopyTextureToTexture);
    SetMethod(isolate, obj, "copyTextureToBuffer", Encoder_CopyTextureToBuffer);
    return obj;
}

static v8::Local<v8::Object> WrapPipeline(v8::Isolate* isolate, wgpu::RenderPipeline pipeline)
{
    auto obj = WrapWith(isolate, Tmpl(isolate).renderPipeline, std::move(pipeline));
    SetMethod(isolate, obj, "getBindGroupLayout", Pipeline_GetBindGroupLayout);
    return obj;
}

// ---------------------------------------------------------------------------
// GPUDevice / GPUQueue / GPUAdapter のラップ
// ---------------------------------------------------------------------------
static v8::Local<v8::Object> WrapQueue(v8::Isolate* isolate, wgpu::Queue queue)
{
    auto obj = WrapWith(isolate, Tmpl(isolate).queue, std::move(queue));
    SetMethod(isolate, obj, "submit", Queue_Submit);
    SetMethod(isolate, obj, "writeBuffer", Queue_WriteBuffer);
    SetMethod(isolate, obj, "writeTexture", Queue_WriteTexture);
    SetMethod(isolate, obj, "copyExternalImageToTexture", Queue_CopyExternalImageToTexture);
    return obj;
}

static v8::Local<v8::Object> WrapDevice(v8::Isolate* isolate, wgpu::Device device)
{
    auto obj = WrapWith(isolate, Tmpl(isolate).device, device);
    SetMethod(isolate, obj, "createBuffer", Device_CreateBuffer);
    SetMethod(isolate, obj, "createTexture", Device_CreateTexture);
    SetMethod(isolate, obj, "createSampler", Device_CreateSampler);
    SetMethod(isolate, obj, "createShaderModule", Device_CreateShaderModule);
    SetMethod(isolate, obj, "createBindGroupLayout", Device_CreateBindGroupLayout);
    SetMethod(isolate, obj, "createBindGroup", Device_CreateBindGroup);
    SetMethod(isolate, obj, "createPipelineLayout", Device_CreatePipelineLayout);
    SetMethod(isolate, obj, "createRenderPipeline", Device_CreateRenderPipeline);
    SetMethod(isolate, obj, "createCommandEncoder", Device_CreateCommandEncoder);
    SetValue(isolate, obj, "queue", WrapQueue(isolate, device.GetQueue()));
    SetValue(isolate, obj, "features", v8::Object::New(isolate));

    // limits: player は maxTextureDimension2D で描画最大サイズを決めるため実値を返す。
    // GetLimits の戻り値型は Dawn バージョン差があるため値でガードする(0 なら保証最小値)。
    v8::Local<v8::Object> limits = v8::Object::New(isolate);
    wgpu::Limits l = {};
    device.GetLimits(&l);
    auto orDefault = [](uint32_t v, uint32_t fb) { return v ? v : fb; };
    SetValue(isolate, limits, "maxTextureDimension1D", v8::Integer::NewFromUnsigned(isolate, orDefault(l.maxTextureDimension1D, 8192)));
    SetValue(isolate, limits, "maxTextureDimension2D", v8::Integer::NewFromUnsigned(isolate, orDefault(l.maxTextureDimension2D, 8192)));
    SetValue(isolate, limits, "maxTextureDimension3D", v8::Integer::NewFromUnsigned(isolate, orDefault(l.maxTextureDimension3D, 2048)));
    SetValue(isolate, limits, "maxBindGroups", v8::Integer::NewFromUnsigned(isolate, orDefault(l.maxBindGroups, 4)));
    SetValue(isolate, limits, "maxBufferSize", v8::Number::New(isolate, static_cast<double>(l.maxBufferSize ? l.maxBufferSize : (256u << 20))));
    SetValue(isolate, limits, "maxVertexBuffers", v8::Integer::NewFromUnsigned(isolate, orDefault(l.maxVertexBuffers, 8)));
    SetValue(isolate, limits, "maxVertexAttributes", v8::Integer::NewFromUnsigned(isolate, orDefault(l.maxVertexAttributes, 16)));
    SetValue(isolate, limits, "maxColorAttachments", v8::Integer::NewFromUnsigned(isolate, orDefault(l.maxColorAttachments, 8)));
    SetValue(isolate, obj, "limits", limits);
    return obj;
}

// requestDevice() -> Promise<GPUDevice>
static void Adapter_RequestDevice(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    DawnContext* gpu = HostContext::From(isolate)->gpu;

    auto resolver = v8::Promise::Resolver::New(ctx).ToLocalChecked();
    args.GetReturnValue().Set(resolver->GetPromise());

    // DawnContext が単一 device を保持。取得済みならそれをラップする。
    if (!gpu->ready() && !gpu->AcquireDevice()) {
        resolver->Reject(ctx, v8::Exception::Error(Str(isolate, "requestDevice failed"))).Check();
        return;
    }
    resolver->Resolve(ctx, WrapDevice(isolate, gpu->device())).Check();
}

// navigator.gpu.requestAdapter() -> Promise<GPUAdapter>
static void GPU_RequestAdapter(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    DawnContext* gpu = HostContext::From(isolate)->gpu;

    auto resolver = v8::Promise::Resolver::New(ctx).ToLocalChecked();
    args.GetReturnValue().Set(resolver->GetPromise());

    if (!gpu->ready() && !gpu->AcquireDevice()) {
        resolver->Resolve(ctx, v8::Null(isolate)).Check();
        return;
    }

    auto adapter = WrapWith(isolate, Tmpl(isolate).adapter, gpu->adapter());
    SetMethod(isolate, adapter, "requestDevice", Adapter_RequestDevice);
    SetValue(isolate, adapter, "features", v8::Object::New(isolate));
    SetValue(isolate, adapter, "limits", v8::Object::New(isolate));
    resolver->Resolve(ctx, adapter).Check();
}

static void GPU_GetPreferredCanvasFormat(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    Ctx_GetConfiguredFormat(args);
}

// ---------------------------------------------------------------------------
// テンプレート初期化 + 定数 + navigator.gpu の設置
// ---------------------------------------------------------------------------
static void InitTemplates(v8::Isolate* isolate)
{
    Templates& t = Tmpl(isolate);
    auto make = [&](v8::Global<v8::ObjectTemplate>& g) {
        g.Reset(isolate, HandleTemplate(isolate));
    };
    make(t.adapter); make(t.device); make(t.queue); make(t.buffer); make(t.texture);
    make(t.textureView); make(t.sampler); make(t.shaderModule); make(t.bindGroupLayout);
    make(t.bindGroup); make(t.pipelineLayout); make(t.renderPipeline); make(t.commandEncoder);
    make(t.renderPass); make(t.commandBuffer); make(t.canvasContext);
}

static void SetFlag(v8::Isolate* isolate, v8::Local<v8::Object> obj, const char* k, uint32_t v)
{
    SetValue(isolate, obj, k, v8::Integer::NewFromUnsigned(isolate, v));
}

} // namespace webgpu

// Bindings.h から呼ばれる公開関数
void InstallWebGPU(v8::Isolate* isolate, v8::Local<v8::Object> global, HostContext* host)
{
    using namespace next2d::webgpu;
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

    InitTemplates(isolate);

    // navigator.gpu は WebGPU バックエンド時のみ公開する。
    // WebGL2 バックエンド(Switch 等)では navigator.gpu を出さず、player を
    // canvas.getContext('webgl2') 経路へフォールバックさせる。
    if (host->backend == GraphicsBackend::WebGPU) {
        // navigator は DomShims が生成する。存在しなければ作る。
        v8::Local<v8::Value> nav_val;
        v8::Local<v8::Object> navigator;
        if (global->Get(ctx, Str(isolate, "navigator")).ToLocal(&nav_val) && nav_val->IsObject()) {
            navigator = nav_val.As<v8::Object>();
        } else {
            navigator = v8::Object::New(isolate);
            SetValue(isolate, global, "navigator", navigator);
        }

        v8::Local<v8::Object> gpu = v8::Object::New(isolate);
        SetMethod(isolate, gpu, "requestAdapter", GPU_RequestAdapter);
        SetMethod(isolate, gpu, "getPreferredCanvasFormat", GPU_GetPreferredCanvasFormat);
        SetValue(isolate, navigator, "gpu", gpu);
    }

    // 使用フラグ定数群 (ブラウザが提供する GPU*Usage 等)
    auto usage = v8::Object::New(isolate);
    SetFlag(isolate, usage, "MAP_READ", 0x0001);
    SetFlag(isolate, usage, "MAP_WRITE", 0x0002);
    SetFlag(isolate, usage, "COPY_SRC", 0x0004);
    SetFlag(isolate, usage, "COPY_DST", 0x0008);
    SetFlag(isolate, usage, "INDEX", 0x0010);
    SetFlag(isolate, usage, "VERTEX", 0x0020);
    SetFlag(isolate, usage, "UNIFORM", 0x0040);
    SetFlag(isolate, usage, "STORAGE", 0x0080);
    SetFlag(isolate, usage, "INDIRECT", 0x0100);
    SetFlag(isolate, usage, "QUERY_RESOLVE", 0x0200);
    SetValue(isolate, global, "GPUBufferUsage", usage);

    auto tex_usage = v8::Object::New(isolate);
    SetFlag(isolate, tex_usage, "COPY_SRC", 0x01);
    SetFlag(isolate, tex_usage, "COPY_DST", 0x02);
    SetFlag(isolate, tex_usage, "TEXTURE_BINDING", 0x04);
    SetFlag(isolate, tex_usage, "STORAGE_BINDING", 0x08);
    SetFlag(isolate, tex_usage, "RENDER_ATTACHMENT", 0x10);
    SetValue(isolate, global, "GPUTextureUsage", tex_usage);

    auto stage = v8::Object::New(isolate);
    SetFlag(isolate, stage, "VERTEX", 0x1);
    SetFlag(isolate, stage, "FRAGMENT", 0x2);
    SetFlag(isolate, stage, "COMPUTE", 0x4);
    SetValue(isolate, global, "GPUShaderStage", stage);

    auto color_write = v8::Object::New(isolate);
    SetFlag(isolate, color_write, "RED", 0x1);
    SetFlag(isolate, color_write, "GREEN", 0x2);
    SetFlag(isolate, color_write, "BLUE", 0x4);
    SetFlag(isolate, color_write, "ALPHA", 0x8);
    SetFlag(isolate, color_write, "ALL", 0xF);
    SetValue(isolate, global, "GPUColorWrite", color_write);

    auto map_mode = v8::Object::New(isolate);
    SetFlag(isolate, map_mode, "READ", 0x1);
    SetFlag(isolate, map_mode, "WRITE", 0x2);
    SetValue(isolate, global, "GPUMapMode", map_mode);
}

// Canvas.cpp から呼ばれる公開関数: canvas.getContext('webgpu') が返す
// GPUCanvasContext を生成する (DawnContext のサーフェスを参照)。
v8::Local<v8::Object> CreateWebGPUCanvasContext(v8::Isolate* isolate, HostContext* /*host*/)
{
    using namespace next2d::webgpu;
    v8::Local<v8::Object> context = v8::Object::New(isolate);
    SetMethod(isolate, context, "configure", Ctx_Configure);
    SetMethod(isolate, context, "unconfigure", [](const v8::FunctionCallbackInfo<v8::Value>&) {});
    SetMethod(isolate, context, "getCurrentTexture", Ctx_GetCurrentTexture);
    return context;
}

} // namespace next2d
