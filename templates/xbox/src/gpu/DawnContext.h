// DawnContext: Dawn(WebGPU) の Instance / Adapter / Device / Surface を管理する。
//
// GDK ウィンドウ(HWND) から D3D12 バックエンドの surface を生成し、
// スワップチェーンとして提示する。JS 側の navigator.gpu / canvas.getContext('webgpu')
// はこのコンテキストが保持する Device / Surface を参照する。
#pragma once

#include <webgpu/webgpu_cpp.h>

#include <cstdint>
#include <functional>

struct HWND__;
using HWND = HWND__*;

namespace next2d {

class DawnContext {
public:
    DawnContext() = default;
    ~DawnContext();

    // Instance と HWND 由来 surface を生成する。
    bool Initialize(HWND hwnd, uint32_t width, uint32_t height);

    // Adapter/Device を同期的に取得する (内部で ProcessEvents をポンプして待つ)。
    // navigator.gpu.requestAdapter/requestDevice からも利用する。
    bool AcquireDevice();

    // surface をデバイスに紐付けて構成する (canvas サイズ変更時に再呼び出し)。
    void Configure(uint32_t width, uint32_t height);

    // 今フレームのバックバッファテクスチャを取得する。
    wgpu::Texture GetCurrentTexture();

    // 提示する。
    void Present();

    // Dawn の非同期イベント(コールバック)を進める。毎フレーム呼ぶ。
    void Tick();

    const wgpu::Instance& instance() const { return instance_; }
    const wgpu::Adapter&  adapter()  const { return adapter_; }
    const wgpu::Device&   device()   const { return device_; }
    const wgpu::Surface&  surface()  const { return surface_; }
    wgpu::TextureFormat   format()   const { return format_; }

    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }
    bool ready() const { return device_ != nullptr; }

private:
    wgpu::Instance instance_ = nullptr;
    wgpu::Adapter  adapter_  = nullptr;
    wgpu::Device   device_   = nullptr;
    wgpu::Surface  surface_  = nullptr;
    wgpu::TextureFormat format_ = wgpu::TextureFormat::BGRA8Unorm;

    HWND     hwnd_   = nullptr;
    uint32_t width_  = 0;
    uint32_t height_ = 0;

    // surface が Configure 済みか。未構成のまま Present すると Dawn の
    // 検証エラーが毎フレーム出るため、Present/GetCurrentTexture をゲートする。
    bool     configured_ = false;
};

} // namespace next2d
