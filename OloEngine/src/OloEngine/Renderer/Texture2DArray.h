#pragma once

#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

namespace OloEngine
{
    enum class Texture2DArrayFormat
    {
        DEPTH_COMPONENT32F,
        RGBA8,
        RGBA16F,
        RGBA32F,
        // Appended (issue #715) — the terrain VT compressed cache pair.
        // RGBA32UI: 128-bit unsigned integer. Integer textures cannot linear-
        // filter, so the backend forces NEAREST min/mag; one texel is bit-
        // compatible with one 16-byte BC7 block (the staging side of the
        // block-copy).
        RGBA32UI,
        // BC7: block-compressed RGBA, LINEAR variant deliberately (VT cache
        // texels are transcoded data, not sRGB-authored colour). Populated only
        // GPU-side via image copy — no SetLayerData, no GenerateMipmaps.
        BC7
    };

    struct Texture2DArraySpecification
    {
        u32 Width = 1024;
        u32 Height = 1024;
        u32 Layers = 1;
        Texture2DArrayFormat Format = Texture2DArrayFormat::DEPTH_COMPONENT32F;
        bool DepthComparisonMode = false; // Enable hardware shadow comparison (sampler2DArrayShadow)
        bool GenerateMipmaps = false;     // Allocate mipmap levels (for color texture arrays)

        // Tile the layers instead of clamping them (issue #969). The default is
        // CLAMP_TO_EDGE, which is right for every array that existed first —
        // shadow cascades, terrain material layers, the VT cache — because each
        // layer is a bounded image and sampling past its edge is a bug.
        //
        // A layer that is PERIODIC BY CONSTRUCTION needs the opposite: the FFT
        // ocean's cascade fields tile seamlessly over their own patch size, and
        // the water shader addresses them with an unbounded worldXZ/L, so
        // clamping renders one tile and then smears its border across the rest
        // of the sea. Honoured by both backends (the Vulkan side registers the
        // address mode the heap mints its sampler from), and only meaningful
        // for colour formats — depth arrays keep their bordered clamp.
        bool RepeatWrap = false;
    };

    class Texture2DArray : public RefCounted
    {
      public:
        virtual ~Texture2DArray() = default;

        [[nodiscard]] virtual u32 GetWidth() const = 0;
        [[nodiscard]] virtual u32 GetHeight() const = 0;
        [[nodiscard]] virtual u32 GetLayers() const = 0;
        [[nodiscard]] virtual u32 GetRendererID() const = 0;

        // Generation-checked identity, minted by RHI::ResourceRegistry
        // (issue #691). Sibling of GetRendererID during the
        // migration: that one hands out the raw backend name and is deleted once
        // every caller has moved. Turning a handle back into a native object is
        // Platform/<Backend>/'s business.
        [[nodiscard]] virtual RHI::ResourceHandle GetRHIHandle() const = 0;
        [[nodiscard]] virtual const Texture2DArraySpecification& GetSpecification() const = 0;

        virtual void Bind(u32 slot) const = 0;

        // Upload pixel data to a specific layer (for building texture arrays from
        // individual images). Client data is NATIVE per format: RGBA8 = u8x4,
        // RGBA16F = halves, RGBA32F = f32x4, RGBA32UI = u32x4; width × height
        // texels. Depth and block-compressed formats have no upload path here —
        // BC7 layers are populated GPU-side (CopyImageSubDataFull block copy).
        virtual void SetLayerData(u32 layer, const void* data, u32 width, u32 height) = 0;

        // Generate mipmaps for the texture array. Refused for depth and BC7
        // arrays (compressed VT caches never mip).
        virtual void GenerateMipmaps() = 0;

        static Ref<Texture2DArray> Create(const Texture2DArraySpecification& spec);
    };
} // namespace OloEngine
