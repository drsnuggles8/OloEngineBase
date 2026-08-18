#pragma once

#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

namespace OloEngine
{
    enum class Texture3DFormat
    {
        RGBA8,
        RGBA16F,
        RGBA32F,
        // Single-channel float. The volumetric shadow volume (issue #723)
        // stores one scalar per texel — optical depth — and R32F is the
        // narrowest format that says so without adding an RHI::Format value:
        // RHI::Format::R32Float already exists, which is what the storage-image
        // descriptor needs.
        R32F
    };

    struct Texture3DSpecification
    {
        u32 Width = 128;
        u32 Height = 128;
        u32 Depth = 128;
        Texture3DFormat Format = Texture3DFormat::RGBA16F;
        // Wrap mode on all three axes: false = clamp-to-edge (wind fields,
        // froxel volumes — samples outside the grid get the boundary value),
        // true = repeat (tiling noise volumes such as the cloud-noise textures).
        bool Repeat = false;
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

        // Generation-checked identity, minted by RHI::ResourceRegistry
        // (issue #691 Phase 2 step 3). Sibling of GetRendererID() during the
        // migration: that one hands out the raw backend name and is deleted once
        // every caller has moved. Turning a handle back into a native object is
        // Platform/<Backend>/'s business.
        [[nodiscard]] virtual RHI::ResourceHandle GetRHIHandle() const = 0;
        [[nodiscard]] virtual const Texture3DSpecification& GetSpecification() const = 0;

        // Bind as a sampler3D for shader sampling (trilinear filtered)
        virtual void Bind(u32 slot) const = 0;

        static Ref<Texture3D> Create(const Texture3DSpecification& spec);
    };
} // namespace OloEngine
