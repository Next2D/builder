#include "DawnContext.h"

#include <Windows.h>
#include <iostream>

namespace next2d {

namespace {

void HandleDeviceLost(const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message)
{
    std::cerr << "[Dawn] Device lost (" << static_cast<int>(reason) << "): "
              << std::string(message.data, message.length) << std::endl;
}

void HandleUncapturedError(const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message)
{
    std::cerr << "[Dawn] Uncaptured error (" << static_cast<int>(type) << "): "
              << std::string(message.data, message.length) << std::endl;
}

} // namespace

DawnContext::~DawnContext() = default;

bool DawnContext::Initialize(HWND hwnd, uint32_t width, uint32_t height)
{
    hwnd_   = hwnd;
    width_  = width;
    height_ = height;

    // タイムドウェイト非依存のコールバックを扱えるよう ProcessEvents を有効化
    wgpu::InstanceDescriptor instance_desc = {};
    static const wgpu::InstanceFeatureName kFeatures[] = {
        wgpu::InstanceFeatureName::TimedWaitAny,
    };
    instance_desc.requiredFeatureCount = 1;
    instance_desc.requiredFeatures = kFeatures;

    instance_ = wgpu::CreateInstance(&instance_desc);
    if (!instance_) {
        std::cerr << "[Dawn] Failed to create instance" << std::endl;
        return false;
    }

    // HWND から surface を生成 (D3D12)
    wgpu::SurfaceSourceWindowsHWND hwnd_source = {};
    hwnd_source.hinstance = GetModuleHandle(nullptr);
    hwnd_source.hwnd = hwnd_;

    wgpu::SurfaceDescriptor surface_desc = {};
    surface_desc.nextInChain = &hwnd_source;

    surface_ = instance_.CreateSurface(&surface_desc);
    if (!surface_) {
        std::cerr << "[Dawn] Failed to create surface" << std::endl;
        return false;
    }

    return true;
}

bool DawnContext::AcquireDevice()
{
    if (device_) {
        return true;
    }

    // Adapter を要求 (D3D12 / HighPerformance)
    wgpu::RequestAdapterOptions adapter_opts = {};
    adapter_opts.backendType = wgpu::BackendType::D3D12;
    adapter_opts.powerPreference = wgpu::PowerPreference::HighPerformance;
    adapter_opts.compatibleSurface = surface_;

    wgpu::Future adapter_future = instance_.RequestAdapter(
        &adapter_opts,
        wgpu::CallbackMode::AllowProcessEvents,
        [this](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter,
               wgpu::StringView message) {
            if (status != wgpu::RequestAdapterStatus::Success) {
                std::cerr << "[Dawn] RequestAdapter failed: "
                          << std::string(message.data, message.length) << std::endl;
                return;
            }
            adapter_ = std::move(adapter);
        });

    instance_.WaitAny(adapter_future, UINT64_MAX);
    if (!adapter_) {
        return false;
    }

    // Device を要求
    wgpu::DeviceDescriptor device_desc = {};
    device_desc.SetDeviceLostCallback(wgpu::CallbackMode::AllowProcessEvents, HandleDeviceLost);
    device_desc.SetUncapturedErrorCallback(HandleUncapturedError);

    wgpu::Future device_future = adapter_.RequestDevice(
        &device_desc,
        wgpu::CallbackMode::AllowProcessEvents,
        [this](wgpu::RequestDeviceStatus status, wgpu::Device device,
               wgpu::StringView message) {
            if (status != wgpu::RequestDeviceStatus::Success) {
                std::cerr << "[Dawn] RequestDevice failed: "
                          << std::string(message.data, message.length) << std::endl;
                return;
            }
            device_ = std::move(device);
        });

    instance_.WaitAny(device_future, UINT64_MAX);
    if (!device_) {
        return false;
    }

    // surface の優先フォーマットと CopySrc 可否 (デバッグ読み戻し用) を取得
    wgpu::SurfaceCapabilities caps = {};
    surface_.GetCapabilities(adapter_, &caps);
    if (caps.formatCount > 0) {
        format_ = caps.formats[0];
    }
    can_copy_src_ = (caps.usages & wgpu::TextureUsage::CopySrc) == wgpu::TextureUsage::CopySrc;

    Configure(width_, height_);
    return true;
}

void DawnContext::Configure(uint32_t width, uint32_t height)
{
    if (!device_ || width == 0 || height == 0) {
        return;
    }

    width_  = width;
    height_ = height;

    wgpu::SurfaceConfiguration config = {};
    config.device      = device_;
    config.format      = format_;
    config.usage       = wgpu::TextureUsage::RenderAttachment;
    if (can_copy_src_) {
        // 黒画面デバッグ用の surface 読み戻し (DebugProbeSurface) を可能にする
        config.usage |= wgpu::TextureUsage::CopySrc;
    }
    config.width       = width_;
    config.height      = height_;
    config.presentMode = wgpu::PresentMode::Fifo;
    config.alphaMode   = wgpu::CompositeAlphaMode::Opaque;

    surface_.Configure(&config);
    configured_ = true;
}

wgpu::Texture DawnContext::GetCurrentTexture()
{
    if (!configured_) {
        return nullptr;
    }
    wgpu::SurfaceTexture surface_texture = {};
    surface_.GetCurrentTexture(&surface_texture);
    frame_texture_acquired_ = true;
    current_texture_ = surface_texture.texture;
    return surface_texture.texture;
}

// 提示直前の surface 中央行を読み戻し、非黒ピクセル数をログする (黒画面調査用)。
// CI では目視できないため、これが「画面に何が出ているか」の唯一の証拠になる。
void DawnContext::DebugProbeSurface()
{
    if (!can_copy_src_ || !current_texture_ || width_ == 0) {
        std::cerr << "[GPU] surface probe unavailable (CopySrc="
                  << (can_copy_src_ ? "yes" : "no") << ")" << std::endl;
        return;
    }

    const uint32_t row_bytes = width_ * 4;
    wgpu::BufferDescriptor buf_desc = {};
    buf_desc.size  = (row_bytes + 3) & ~3u;
    buf_desc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
    wgpu::Buffer buffer = device_.CreateBuffer(&buf_desc);

    wgpu::TexelCopyTextureInfo src = {};
    src.texture = current_texture_;
    src.origin  = { 0, height_ / 2, 0 };

    wgpu::TexelCopyBufferInfo dst = {};
    dst.buffer = buffer;
    dst.layout.offset = 0;
    dst.layout.bytesPerRow = wgpu::kCopyStrideUndefined;   // 1 行コピーでは不要

    wgpu::Extent3D extent = { width_, 1, 1 };

    wgpu::CommandEncoder encoder = device_.CreateCommandEncoder();
    encoder.CopyTextureToBuffer(&src, &dst, &extent);
    wgpu::CommandBuffer commands = encoder.Finish();
    device_.GetQueue().Submit(1, &commands);

    bool done = false;
    wgpu::Future future = buffer.MapAsync(wgpu::MapMode::Read, 0, buf_desc.size,
        wgpu::CallbackMode::AllowProcessEvents,
        [&done](wgpu::MapAsyncStatus, wgpu::StringView) { done = true; });
    // GPU 完了待ちは TimedWaitAny で行う (ProcessEvents の空スピンでは
    // GPU がフレーム描画中のとき完了前にループ上限へ達する)
    instance_.WaitAny(future, 2'000'000'000);   // 2s
    if (!done) {
        std::cerr << "[GPU] surface probe: map timeout" << std::endl;
        return;
    }

    const auto* px = static_cast<const uint8_t*>(buffer.GetConstMappedRange(0, buf_desc.size));
    uint32_t non_black = 0;
    for (uint32_t x = 0; px && x < width_; ++x) {
        const uint8_t* p = px + static_cast<size_t>(x) * 4;
        if (p[0] || p[1] || p[2]) {
            ++non_black;
        }
    }
    std::cerr << "[GPU] surface probe (present #" << present_count_
              << "): non-black " << non_black << "/" << width_ << " px";
    if (px && width_ > 1) {
        const uint8_t* c = px + static_cast<size_t>(width_ / 2) * 4;
        std::cerr << ", center=(" << +c[0] << "," << +c[1] << "," << +c[2] << "," << +c[3] << ")";
    }
    std::cerr << std::endl;
    buffer.Unmap();
}

void DawnContext::Present()
{
    // 提示するのは「未構成でない」かつ「今フレーム GetCurrentTexture 済み」のときのみ。
    // 取得なしで Present すると Dawn の検証エラーになる (描画しないフレームは提示しない)。
    if (surface_ && configured_ && frame_texture_acquired_) {
        ++present_count_;
        if (present_count_ <= 3 || present_count_ == 60 || present_count_ % 300 == 0) {
            DebugProbeSurface();
        }
        surface_.Present();
    } else {
        ++present_skipped_;
        if (present_skipped_ <= 3 || present_skipped_ % 300 == 0) {
            std::cerr << "[GPU] present skipped #" << present_skipped_
                      << " (configured=" << configured_
                      << " acquired=" << frame_texture_acquired_ << ")" << std::endl;
        }
    }
    frame_texture_acquired_ = false;
    current_texture_ = nullptr;
}

void DawnContext::Tick()
{
    if (instance_) {
        instance_.ProcessEvents();
    }
}

} // namespace next2d
