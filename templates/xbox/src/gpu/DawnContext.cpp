#include "DawnContext.h"

#include <Windows.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace next2d {

namespace {

// GUI 実行では stderr が見えないため、GPU の致命的エラー/検証エラーを
// next2d-error.log にも残す。描画が黒くなる/表示されない等の原因(検証エラー・
// device lost・リソース枯渇)を掴む唯一の手段。氾濫防止に総数を制限する。
void AppendGpuLog(const std::string& line)
{
    static int count = 0;
    if (count >= 60) {
        return;
    }
    ++count;
    std::ofstream ofs("next2d-error.log", std::ios::app);
    if (ofs) {
        ofs << line << std::endl;
    }
}

void HandleDeviceLost(const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message)
{
    const std::string line = "[Dawn] Device lost (" + std::to_string(static_cast<int>(reason))
        + "): " + std::string(message.data, message.length);
    std::cerr << line << std::endl;
    AppendGpuLog(line);
}

void HandleUncapturedError(const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message)
{
    const std::string line = "[Dawn] Uncaptured error (" + std::to_string(static_cast<int>(type))
        + "): " + std::string(message.data, message.length);
    std::cerr << line << std::endl;
    AppendGpuLog(line);
}

// --- GPU Blob キャッシュ (シェーダ/パイプラインのコンパイル結果) -------------
// D3D12 の HLSL コンパイル (FXC) はパイプライン初回生成時に走り、起動直後や
// 新しいエフェクト初出時のフレームヒッチの原因になる。Dawn の BlobCache を
// %LOCALAPPDATA%\Next2D\gpucache に永続化し、2 回目以降の起動で再利用する
// (V8 バイトコードキャッシュの GPU 版)。
// 呼び出し規約 (dawn/native/BlobCache.cpp LoadInternal で検証済み):
//   load は 2 段階 — まず value=nullptr でサイズ問い合わせ (戻り値 0 = miss)、
//   次に確保済みバッファへの読み込み (戻り値がサイズと不一致なら miss 扱い)。
// キーには Dawn がアダプタ/ドライバ情報を織り込むため、環境が変われば自然に
// 別エントリになる。あらゆる失敗は 0 / 何もしない (graceful degrade)。

uint64_t Fnv1a(const void* data, size_t size, uint64_t hash = 1469598103934665603ull)
{
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= p[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

const std::filesystem::path& GpuCacheDir()
{
    static const std::filesystem::path dir = []() -> std::filesystem::path {
        const char* base = std::getenv("LOCALAPPDATA");
        if (!base || !*base) {
            return {};
        }
        std::error_code ec;
        std::filesystem::path d = std::filesystem::path(base) / "Next2D" / "gpucache";
        std::filesystem::create_directories(d, ec);
        if (ec) {
            return {};
        }
        // ドライバ/Dawn 更新でキーが変わると旧 blob は孤児になる。
        // 30 日アクセスの無い .blob を掃除する (load ヒット時に touch する LRU)。
        const auto now = std::filesystem::file_time_type::clock::now();
        std::error_code iter_ec;
        for (const auto& entry : std::filesystem::directory_iterator(d, iter_ec)) {
            std::error_code e2;
            if (entry.path().extension() != ".blob") {
                continue;
            }
            const auto t = std::filesystem::last_write_time(entry.path(), e2);
            if (!e2 && now - t > std::chrono::hours(24 * 30)) {
                std::filesystem::remove(entry.path(), e2);
            }
        }
        return d;
    }();
    return dir;
}

std::filesystem::path GpuCacheFileFor(const void* key, size_t key_size)
{
    const std::filesystem::path& dir = GpuCacheDir();
    if (dir.empty()) {
        return {};
    }
    char buf[17] = {};
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(Fnv1a(key, key_size)));
    return dir / (std::string(buf) + ".blob");
}

size_t LoadGpuCacheData(const void* key, size_t key_size,
                        void* value, size_t value_size, void*)
{
    const std::filesystem::path file = GpuCacheFileFor(key, key_size);
    if (file.empty()) {
        return 0;
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec || size == 0) {
        return 0;
    }
    if (value == nullptr) {
        // 1 段階目: サイズ問い合わせ
        return static_cast<size_t>(size);
    }
    if (value_size < size) {
        return 0;
    }
    std::ifstream ifs(file, std::ios::binary);
    if (!ifs || !ifs.read(static_cast<char*>(value), static_cast<std::streamsize>(size))) {
        return 0;
    }
    // 使用時に touch して 30 日掃除を LRU として機能させる
    std::filesystem::last_write_time(
        file, std::filesystem::file_time_type::clock::now(), ec);
    return static_cast<size_t>(size);
}

void StoreGpuCacheData(const void* key, size_t key_size,
                       const void* value, size_t value_size, void*)
{
    if (!value || value_size == 0) {
        return;
    }
    const std::filesystem::path file = GpuCacheFileFor(key, key_size);
    if (file.empty()) {
        return;
    }
    std::ofstream ofs(file, std::ios::binary | std::ios::trunc);
    if (ofs) {
        ofs.write(static_cast<const char*>(value),
                  static_cast<std::streamsize>(value_size));
    }
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

    // GPU Blob キャッシュを接続する (シェーダコンパイル結果の永続化)。
    // 関数ポインタは Dawn が device 生成時に BlobCache へコピーするため、
    // この記述子自体は RequestDevice 完了までの生存で足りる。
    wgpu::DawnCacheDeviceDescriptor cache_desc = {};
    cache_desc.loadDataFunction  = LoadGpuCacheData;
    cache_desc.storeDataFunction = StoreGpuCacheData;
    device_desc.nextInChain = &cache_desc;

#ifdef NDEBUG
    // Release では Dawn の API 検証 (skip_validation) と robustness (OOB クランプ) を
    // 無効化する。両方とも全描画コマンドのエンコードで毎フレーム CPU を消費する
    // 安全網で、検証済みタイトルの出荷ビルドでは Chrome も同様に外す定番最適化。
    // Debug ビルドでは有効のままにして検証エラーを検出できるようにする
    // (エラーは HandleUncapturedError -> next2d-error.log へ出る)。
    // toggle 名は Dawn 本体 src/dawn/native/Toggles.cpp 定義 (いずれも ToggleStage::Device)。
    static const char* kReleaseToggles[] = {"skip_validation", "disable_robustness"};
    wgpu::DawnTogglesDescriptor device_toggles = {};
    device_toggles.enabledToggles     = kReleaseToggles;
    device_toggles.enabledToggleCount = 2;
    cache_desc.nextInChain = &device_toggles;  // device -> cache -> toggles
#endif

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
    // RenderAttachment のみ。CopySrc を足すと swapchain の flip 提示最適化が無効化され
    // 提示ごとにコピーが挟まって重くなるため、描画確認が済んだ今は付けない
    // (以前は DebugProbeSurface の黒画面調査用に付けていた)。
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
        surface_.Present();
    } else {
        ++present_skipped_;
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
