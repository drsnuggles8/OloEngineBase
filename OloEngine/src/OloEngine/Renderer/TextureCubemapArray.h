#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Texture.h"

#include <vector>

namespace OloEngine
{
    class TextureCubemap;

    struct CubemapArraySpecification
    {
        u32 Resolution = 1; // square face size
        u32 Layers = 1;     // number of cubemaps in the array
        ImageFormat Format = ImageFormat::RGBA8;
        u32 MipLevels = 0; // 0 = full chain derived from Resolution; >0 = exactly this many
    };

    // GPU cubemap array (GL_TEXTURE_CUBE_MAP_ARRAY / samplerCubeArray).
    // Added for the distance-impostor reflection probes (issue #705): one
    // array holds every probe's prefiltered radiance chain, a second holds
    // the R32F radial-distance fields, so the deferred/forward lit passes
    // can select probes per pixel. Renderer-internal — never registered
    // with the AssetManager (GetAssetType stays None rather than widening
    // the serialized AssetType enum for a non-asset resource).
    class TextureCubemapArray : public Texture
    {
      public:
        ~TextureCubemapArray() override = default;

        static Ref<TextureCubemapArray> Create(const CubemapArraySpecification& specification);

        [[nodiscard]] virtual const CubemapArraySpecification& GetArraySpecification() const = 0;
        [[nodiscard("Store this!")]] virtual u32 GetMipLevelCount() const = 0;

        // Upload one mip of one cubemap layer: `data` holds the 6 faces
        // contiguously in GL face order (+X,-X,+Y,-Y,+Z,-Z), each face
        // row-major — the layout ReflectionProbeDistanceField stores.
        // No mip auto-generation (the probe mips are hand-built max-mips).
        virtual bool SetLayerMipData(u32 layer, u32 mip, const void* data, sizet sizeBytes) = 0;

        // GPU-side copy of an entire cubemap (all faces, all shared mips)
        // into one array layer. Source format and face size must match the
        // array's; mips are copied up to min(source mips, array mips).
        virtual bool CopyLayerFromCubemap(u32 layer, const TextureCubemap& source) = 0;

        // Asset interface — see class comment: renderer-internal resource,
        // never registered with the AssetManager. Deliberately AssetType::None
        // (the abstract-Texture convention) rather than TextureCube: reusing
        // TextureCube would let AssetType-keyed dispatch mistake an array for
        // a TextureCubemap.
        static constexpr AssetType GetStaticType() noexcept
        {
            return AssetType::None;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }
    };
} // namespace OloEngine
