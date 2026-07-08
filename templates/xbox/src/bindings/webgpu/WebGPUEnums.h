// WebGPU の文字列 enum を wgpu:: enum へ変換するヘルパー。
// 2D レンダラ(Next2D)が使用する頻出値を網羅する。未知値は既定へフォールバックする。
// «EXTEND» 追加フォーマット/enum が必要な場合はここに追記する。
#pragma once

#include <webgpu/webgpu_cpp.h>
#include <string>
#include <string_view>

namespace next2d::webgpu {

inline wgpu::TextureFormat ToTextureFormat(std::string_view s)
{
    using F = wgpu::TextureFormat;
    if (s == "rgba8unorm")            return F::RGBA8Unorm;
    if (s == "rgba8unorm-srgb")       return F::RGBA8UnormSrgb;
    if (s == "bgra8unorm")            return F::BGRA8Unorm;
    if (s == "bgra8unorm-srgb")       return F::BGRA8UnormSrgb;
    if (s == "rgba8snorm")            return F::RGBA8Snorm;
    if (s == "rgba8uint")             return F::RGBA8Uint;
    if (s == "rgba8sint")             return F::RGBA8Sint;
    if (s == "r8unorm")               return F::R8Unorm;
    if (s == "r8snorm")               return F::R8Snorm;
    if (s == "r8uint")                return F::R8Uint;
    if (s == "r8sint")                return F::R8Sint;
    if (s == "rg8unorm")              return F::RG8Unorm;
    if (s == "rg8snorm")              return F::RG8Snorm;
    if (s == "rg8uint")               return F::RG8Uint;
    if (s == "rg8sint")               return F::RG8Sint;
    if (s == "r16uint")               return F::R16Uint;
    if (s == "r16sint")               return F::R16Sint;
    if (s == "r16float")              return F::R16Float;
    if (s == "rg16uint")              return F::RG16Uint;
    if (s == "rg16sint")              return F::RG16Sint;
    if (s == "rg16float")             return F::RG16Float;
    if (s == "rgba16uint")            return F::RGBA16Uint;
    if (s == "rgba16sint")            return F::RGBA16Sint;
    if (s == "rgba16float")           return F::RGBA16Float;
    if (s == "r32uint")               return F::R32Uint;
    if (s == "r32sint")               return F::R32Sint;
    if (s == "r32float")              return F::R32Float;
    if (s == "rg32uint")              return F::RG32Uint;
    if (s == "rg32sint")              return F::RG32Sint;
    if (s == "rg32float")             return F::RG32Float;
    if (s == "rgba32uint")            return F::RGBA32Uint;
    if (s == "rgba32sint")            return F::RGBA32Sint;
    if (s == "rgba32float")           return F::RGBA32Float;
    if (s == "rgb10a2unorm")          return F::RGB10A2Unorm;
    if (s == "rg11b10ufloat")         return F::RG11B10Ufloat;
    if (s == "depth16unorm")          return F::Depth16Unorm;
    if (s == "depth24plus")           return F::Depth24Plus;
    if (s == "depth24plus-stencil8")  return F::Depth24PlusStencil8;
    if (s == "depth32float")          return F::Depth32Float;
    if (s == "depth32float-stencil8") return F::Depth32FloatStencil8;
    if (s == "stencil8")              return F::Stencil8;
    return F::RGBA8Unorm;
}

inline wgpu::StorageTextureAccess ToStorageTextureAccess(std::string_view s)
{
    using A = wgpu::StorageTextureAccess;
    if (s == "write-only") return A::WriteOnly;
    if (s == "read-only")  return A::ReadOnly;
    if (s == "read-write") return A::ReadWrite;
    return A::WriteOnly;
}

inline wgpu::VertexFormat ToVertexFormat(std::string_view s)
{
    using V = wgpu::VertexFormat;
    if (s == "float32")    return V::Float32;
    if (s == "float32x2")  return V::Float32x2;
    if (s == "float32x3")  return V::Float32x3;
    if (s == "float32x4")  return V::Float32x4;
    if (s == "uint32")     return V::Uint32;
    if (s == "uint32x2")   return V::Uint32x2;
    if (s == "uint32x3")   return V::Uint32x3;
    if (s == "uint32x4")   return V::Uint32x4;
    if (s == "sint32")     return V::Sint32;
    if (s == "sint32x2")   return V::Sint32x2;
    if (s == "uint8x2")    return V::Uint8x2;
    if (s == "uint8x4")    return V::Uint8x4;
    if (s == "unorm8x2")   return V::Unorm8x2;
    if (s == "unorm8x4")   return V::Unorm8x4;
    if (s == "uint16x2")   return V::Uint16x2;
    if (s == "uint16x4")   return V::Uint16x4;
    if (s == "unorm16x2")  return V::Unorm16x2;
    if (s == "unorm16x4")  return V::Unorm16x4;
    if (s == "float16x2")  return V::Float16x2;
    if (s == "float16x4")  return V::Float16x4;
    return V::Float32;
}

inline wgpu::PrimitiveTopology ToTopology(std::string_view s)
{
    using T = wgpu::PrimitiveTopology;
    if (s == "point-list")     return T::PointList;
    if (s == "line-list")      return T::LineList;
    if (s == "line-strip")     return T::LineStrip;
    if (s == "triangle-list")  return T::TriangleList;
    if (s == "triangle-strip") return T::TriangleStrip;
    return T::TriangleList;
}

inline wgpu::IndexFormat ToIndexFormat(std::string_view s)
{
    if (s == "uint16") return wgpu::IndexFormat::Uint16;
    if (s == "uint32") return wgpu::IndexFormat::Uint32;
    return wgpu::IndexFormat::Undefined;
}

inline wgpu::BlendFactor ToBlendFactor(std::string_view s)
{
    using B = wgpu::BlendFactor;
    if (s == "zero")                return B::Zero;
    if (s == "one")                 return B::One;
    if (s == "src")                 return B::Src;
    if (s == "one-minus-src")       return B::OneMinusSrc;
    if (s == "src-alpha")           return B::SrcAlpha;
    if (s == "one-minus-src-alpha") return B::OneMinusSrcAlpha;
    if (s == "dst")                 return B::Dst;
    if (s == "one-minus-dst")       return B::OneMinusDst;
    if (s == "dst-alpha")           return B::DstAlpha;
    if (s == "one-minus-dst-alpha") return B::OneMinusDstAlpha;
    if (s == "src-alpha-saturated") return B::SrcAlphaSaturated;
    if (s == "constant")            return B::Constant;
    if (s == "one-minus-constant")  return B::OneMinusConstant;
    return B::One;
}

inline wgpu::BlendOperation ToBlendOperation(std::string_view s)
{
    using O = wgpu::BlendOperation;
    if (s == "add")              return O::Add;
    if (s == "subtract")         return O::Subtract;
    if (s == "reverse-subtract") return O::ReverseSubtract;
    if (s == "min")              return O::Min;
    if (s == "max")              return O::Max;
    return O::Add;
}

inline wgpu::LoadOp ToLoadOp(std::string_view s)
{
    if (s == "load")  return wgpu::LoadOp::Load;
    if (s == "clear") return wgpu::LoadOp::Clear;
    return wgpu::LoadOp::Clear;
}

inline wgpu::StoreOp ToStoreOp(std::string_view s)
{
    if (s == "store")   return wgpu::StoreOp::Store;
    if (s == "discard") return wgpu::StoreOp::Discard;
    return wgpu::StoreOp::Store;
}

inline wgpu::CompareFunction ToCompareFunction(std::string_view s)
{
    using C = wgpu::CompareFunction;
    if (s == "never")         return C::Never;
    if (s == "less")          return C::Less;
    if (s == "equal")         return C::Equal;
    if (s == "less-equal")    return C::LessEqual;
    if (s == "greater")       return C::Greater;
    if (s == "not-equal")     return C::NotEqual;
    if (s == "greater-equal") return C::GreaterEqual;
    if (s == "always")        return C::Always;
    return C::Always;
}

inline wgpu::StencilOperation ToStencilOperation(std::string_view s)
{
    using O = wgpu::StencilOperation;
    if (s == "keep")            return O::Keep;
    if (s == "zero")            return O::Zero;
    if (s == "replace")         return O::Replace;
    if (s == "invert")          return O::Invert;
    if (s == "increment-clamp") return O::IncrementClamp;
    if (s == "decrement-clamp") return O::DecrementClamp;
    if (s == "increment-wrap")  return O::IncrementWrap;
    if (s == "decrement-wrap")  return O::DecrementWrap;
    return O::Keep;
}

inline wgpu::CullMode ToCullMode(std::string_view s)
{
    if (s == "none")  return wgpu::CullMode::None;
    if (s == "front") return wgpu::CullMode::Front;
    if (s == "back")  return wgpu::CullMode::Back;
    return wgpu::CullMode::None;
}

inline wgpu::FrontFace ToFrontFace(std::string_view s)
{
    if (s == "ccw") return wgpu::FrontFace::CCW;
    if (s == "cw")  return wgpu::FrontFace::CW;
    return wgpu::FrontFace::CCW;
}

inline wgpu::AddressMode ToAddressMode(std::string_view s)
{
    if (s == "clamp-to-edge") return wgpu::AddressMode::ClampToEdge;
    if (s == "repeat")        return wgpu::AddressMode::Repeat;
    if (s == "mirror-repeat") return wgpu::AddressMode::MirrorRepeat;
    return wgpu::AddressMode::ClampToEdge;
}

inline wgpu::FilterMode ToFilterMode(std::string_view s)
{
    if (s == "nearest") return wgpu::FilterMode::Nearest;
    if (s == "linear")  return wgpu::FilterMode::Linear;
    return wgpu::FilterMode::Nearest;
}

inline wgpu::MipmapFilterMode ToMipmapFilterMode(std::string_view s)
{
    if (s == "nearest") return wgpu::MipmapFilterMode::Nearest;
    if (s == "linear")  return wgpu::MipmapFilterMode::Linear;
    return wgpu::MipmapFilterMode::Nearest;
}

inline wgpu::VertexStepMode ToStepMode(std::string_view s)
{
    if (s == "vertex")   return wgpu::VertexStepMode::Vertex;
    if (s == "instance") return wgpu::VertexStepMode::Instance;
    return wgpu::VertexStepMode::Vertex;
}

inline wgpu::TextureDimension ToTextureDimension(std::string_view s)
{
    if (s == "1d") return wgpu::TextureDimension::e1D;
    if (s == "2d") return wgpu::TextureDimension::e2D;
    if (s == "3d") return wgpu::TextureDimension::e3D;
    return wgpu::TextureDimension::e2D;
}

inline wgpu::TextureViewDimension ToTextureViewDimension(std::string_view s)
{
    using D = wgpu::TextureViewDimension;
    if (s == "1d")         return D::e1D;
    if (s == "2d")         return D::e2D;
    if (s == "2d-array")   return D::e2DArray;
    if (s == "cube")       return D::Cube;
    if (s == "cube-array") return D::CubeArray;
    if (s == "3d")         return D::e3D;
    return D::Undefined;
}

inline wgpu::BufferBindingType ToBufferBindingType(std::string_view s)
{
    using B = wgpu::BufferBindingType;
    if (s == "uniform")           return B::Uniform;
    if (s == "storage")           return B::Storage;
    if (s == "read-only-storage") return B::ReadOnlyStorage;
    return B::Uniform;
}

inline wgpu::SamplerBindingType ToSamplerBindingType(std::string_view s)
{
    using S = wgpu::SamplerBindingType;
    if (s == "filtering")     return S::Filtering;
    if (s == "non-filtering") return S::NonFiltering;
    if (s == "comparison")    return S::Comparison;
    return S::Filtering;
}

inline wgpu::TextureSampleType ToTextureSampleType(std::string_view s)
{
    using T = wgpu::TextureSampleType;
    if (s == "float")               return T::Float;
    if (s == "unfilterable-float")  return T::UnfilterableFloat;
    if (s == "depth")               return T::Depth;
    if (s == "sint")                return T::Sint;
    if (s == "uint")                return T::Uint;
    return T::Float;
}

} // namespace next2d::webgpu
