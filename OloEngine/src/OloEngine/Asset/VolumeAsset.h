#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Texture3D.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    // Dense volumetric density grid (issue #724) — the runtime-side asset a
    // .olovol resolves to. Cooked from a sparse OpenVDB source at editor/cook
    // time (OloEngine-VolumeCook, editor-only); this class and its serializer
    // (Asset/Serializers/VolumeSerializer.cpp) have no OpenVDB dependency —
    // see docs/agent-rules/asset-import-usd-alembic.md for why that split
    // exists, and the OLO_WITH_OPENVDB option comment in the root
    // CMakeLists.txt.
    //
    // GridTransform + VoxelSize are the metadata the issue names as the
    // reason not to just bake slices of a volume in a DCC: they let a
    // consumer (FogVolumeComponent, see Components.h) place and orient the
    // volume correctly in world space instead of guessing a unit cube.
    class VolumeAsset : public Asset
    {
      public:
        VolumeAsset() = default;

        static AssetType GetStaticType()
        {
            return AssetType::Volume;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        [[nodiscard]] const Ref<Texture3D>& GetTexture() const
        {
            return m_Texture;
        }
        [[nodiscard]] bool IsLoaded() const
        {
            return m_Texture != nullptr;
        }

        [[nodiscard]] const glm::uvec3& GetDimensions() const
        {
            return m_Dimensions;
        }
        // World units per voxel along each grid-local axis (before
        // GridTransform's rotation is applied).
        [[nodiscard]] const glm::vec3& GetVoxelSize() const
        {
            return m_VoxelSize;
        }
        // Grid-index-space (0..Dimensions) -> object-local space, preserving
        // the source .vdb's affine map (translation + rotation + non-uniform
        // scale). A consumer samples the texture in [0,1]^3 UVW and must
        // apply this transform (or the equivalent bounds it encodes) to
        // place the volume correctly.
        [[nodiscard]] const glm::mat4& GetGridTransform() const
        {
            return m_GridTransform;
        }
        // Density value implicitly filling every voxel OUTSIDE the source
        // grid's active/dense region (OpenVDB's per-grid background value).
        [[nodiscard]] f32 GetBackgroundValue() const
        {
            return m_BackgroundValue;
        }

      private:
        friend class VolumeSerializer;

        Ref<Texture3D> m_Texture;
        glm::uvec3 m_Dimensions{ 0u, 0u, 0u };
        glm::vec3 m_VoxelSize{ 1.0f, 1.0f, 1.0f };
        glm::mat4 m_GridTransform{ 1.0f };
        f32 m_BackgroundValue = 0.0f;
    };
} // namespace OloEngine
