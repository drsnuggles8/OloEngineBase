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
    };

    class Texture2DArray : public RefCounted
    {
      public:
        virtual ~Texture2DArray() = default;

        [[nodiscard]] virtual u32 GetWidth() const = 0;
        [[nodiscard]] virtual u32 GetHeight() const = 0;
        [[nodiscard]] virtual u32 GetLayers() const = 0;
        [[nodiscard]] virtual u32 GetRendererID() const = 0;

        virtual void Bind(u32 slot) const = 0;

        static Ref<Texture2DArray> Create(const Texture2DArraySpecification& spec);
    };
} // namespace OloEngine
