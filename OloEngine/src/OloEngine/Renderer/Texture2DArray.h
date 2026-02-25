#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

namespace OloEngine
{
    enum class Texture2DArrayFormat
    {
        DEPTH_COMPONENT32F,
        RGBA8,
        RGBA16F,
        RGBA32F
    };

    struct Texture2DArraySpecification
    {
        u32 Width = 1024;
        u32 Height = 1024;
        u32 Layers = 1;
        Texture2DArrayFormat Format = Texture2DArrayFormat::DEPTH_COMPONENT32F;
        bool DepthComparisonMode = false; // Enable hardware shadow comparison (sampler2DArrayShadow)
        bool GenerateMipmaps = false;     // Allocate mipmap levels (for color texture arrays)
    };

    class Texture2DArray : public RefCounted
    {
      public:
        virtual ~Texture2DArray() = default;

        [[nodiscard]] virtual u32 GetWidth() const = 0;
        [[nodiscard]] virtual u32 GetHeight() const = 0;
        [[nodiscard]] virtual u32 GetLayers() const = 0;
        [[nodiscard]] virtual u32 GetRendererID() const = 0;
        [[nodiscard]] virtual const Texture2DArraySpecification& GetSpecification() const = 0;

        virtual void Bind(u32 slot) const = 0;

        // Upload pixel data to a specific layer (for building texture arrays from individual images)
        // data must be RGBA8 (4 bytes per pixel), width × height pixels
        virtual void SetLayerData(u32 layer, const void* data, u32 width, u32 height) = 0;

        // Generate mipmaps for the texture array
        virtual void GenerateMipmaps() = 0;

        static Ref<Texture2DArray> Create(const Texture2DArraySpecification& spec);
    };
} // namespace OloEngine
