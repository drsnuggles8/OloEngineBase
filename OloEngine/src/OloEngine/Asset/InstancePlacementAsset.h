#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Renderer/Instancing/InstanceData.h"

#include <vector>

namespace OloEngine
{
    // @brief Authored placement data for InstancedMeshComponent.
    //
    // Stores a flat array of world-space `InstanceData` records (transform,
    // color, custom, entity id). A single `.oloinstances` asset can be
    // referenced by multiple InstancedMeshComponents — useful for sharing
    // expensive vegetation / debris / crowd placements across scenes, or for
    // splitting a huge population across LOD chunks.
    //
    // The component-level `Instances` vector remains the authoritative
    // runtime list; the asset path is *additive*: when an
    // InstancedMeshComponent references this asset, the Scene renderer
    // appends the asset's instances to the component's inline list each
    // frame. Procedural / scripted placements can coexist with authored
    // ones.
    class InstancePlacementAsset : public Asset
    {
      public:
        InstancePlacementAsset() = default;
        ~InstancePlacementAsset() override = default;

        static AssetType GetStaticType()
        {
            return AssetType::InstancePlacement;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        const std::vector<InstanceData>& GetInstances() const
        {
            return m_Instances;
        }
        std::vector<InstanceData>& GetInstances()
        {
            return m_Instances;
        }

        void AddInstance(const InstanceData& inst)
        {
            m_Instances.push_back(inst);
        }

        void Clear()
        {
            m_Instances.clear();
        }

      private:
        std::vector<InstanceData> m_Instances;
    };
} // namespace OloEngine
