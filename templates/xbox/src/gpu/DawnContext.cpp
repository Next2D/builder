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

    // surface の優先フォーマットを取得
    wgpu::SurfaceCapabilities caps = {};
    surface_.GetCapabilities(adapter_, &caps);
    if (caps.formatCount > 0) {
        format_ = caps.formats[0];
    }

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
    return surface_texture.texture;
}

void DawnContext::Present()
{
    // 未構成 (GPU 無し環境等) では no-op。毎フレームの検証エラーを避ける
    if (surface_ && configured_) {
        surface_.Present();
    }
}

void DawnContext::Tick()
{
    if (instance_) {
        instance_.ProcessEvents();
    }
}

} // namespace next2d
