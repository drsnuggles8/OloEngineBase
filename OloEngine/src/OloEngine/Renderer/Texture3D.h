#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

namespace OloEngine
{
    enum class Texture3DFormat
    {
        RGBA16F,
        RGBA32F
    };

    struct Texture3DSpecification
    {
        u32 Width = 128;
        u32 Height = 128;
        u32 Depth = 128;
        Texture3DFormat Format = Texture3DFormat::RGBA16F;
    };

    // @brief 3D volume texture abstraction.
    //
    // Used for volumetric data such as wind fields, 3D noise textures, and
    // density volumes. Supports trilinear filtering for smooth interpolation.
    // Can be bound as a sampler3D for reading or as an image3D for compute
    // shader write access via RenderCommand::BindImageTexture().
    class Texture3D : public RefCounted
    {
      public:
        virtual ~Texture3D() = default;

        [[nodiscard]] virtual u32 GetWidth() const = 0;
        [[nodiscard]] virtual u32 GetHeight() const = 0;
        [[nodiscard]] virtual u32 GetDepth() const = 0;
        [[nodiscard]] virtual u32 GetRendererID() const = 0;
        [[nodiscard]] virtual const Texture3DSpecification& GetSpecification() const = 0;

        // Bind as a sampler3D for shader sampling (trilinear filtered)
        virtual void Bind(u32 slot) const = 0;

        static Ref<Texture3D> Create(const Texture3DSpecification& spec);
    };
} // namespace OloEngine
