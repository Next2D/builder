#include "WicDecoder.h"

#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace next2d {

// WIC で任意フォーマットの画像を RGBA8 にデコードする。
bool DecodeImageWithWIC(const std::vector<uint8_t>& input, DecodedImage& out)
{
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        return false;
    }

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) {
        return false;
    }
    if (FAILED(stream->InitializeFromMemory(
            const_cast<uint8_t*>(input.data()), static_cast<DWORD>(input.size())))) {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(
            stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) {
        return false;
    }
    if (FAILED(converter->Initialize(
            frame.Get(), GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        return false;
    }

    UINT w = 0, h = 0;
    converter->GetSize(&w, &h);
    out.width = w;
    out.height = h;
    out.rgba.resize(static_cast<size_t>(w) * h * 4);

    const UINT stride = w * 4;
    if (FAILED(converter->CopyPixels(
            nullptr, stride, static_cast<UINT>(out.rgba.size()), out.rgba.data()))) {
        return false;
    }
    return true;
}

} // namespace next2d
