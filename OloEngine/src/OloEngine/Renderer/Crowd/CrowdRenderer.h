#pragma once

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Renderer/Crowd/CrowdCull.h"
#include "OloEngine/Renderer/Crowd/CrowdIndividual.h"
#include "OloEngine/Renderer/Crowd/CrowdLod.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"

#include <memory>
#include <map>
#include <unordered_map>

namespace OloEngine
{
    class MaterialOverrideCache;
    struct CrowdMaterials
    {
        const Material* Override;
        const MaterialOverrideCache* Patches;
        const Material& Default;
        GPUSceneMaterialOverrideLane Lane;
    };

    // Scene-owned caches. Animation time and transform history advance once per
    // physical frame. Culling buffers and the hysteresis decision belong to a view.
    class CrowdRenderer
    {
      public:
        bool Submit(u64 owner, i32 pickingID, const Ref<MeshSource>& source, const Ref<Skeleton>& rig,
                    const AnimationStateComponent& animation, const CrowdAnimationComponent& settings,
                    std::span<const InstanceData> placements, sizet inlineCount,
                    const CrowdMaterials& materials, bool castShadows);
        void Remove(u64 owner);
        void FinishView();
        [[nodiscard]] CrowdVisibilityCounts ReadVisibilityCounts() const;
        ~CrowdRenderer();
        CrowdRenderer();

      private:
        struct Batch;
        CrowdIndividual m_Individual;
        std::map<std::pair<u64, i32>, std::unique_ptr<Batch>> m_Batches;
        bool SubmitGroup(u64 owner, i32 clipIndex, i32 pickingID, const Ref<MeshSource>& source, const Ref<Skeleton>& rig,
                         const AnimationStateComponent& animation, const CrowdAnimationComponent& settings,
                         std::span<const InstanceData> placements, sizet inlineCount,
                         const CrowdMaterials& materials, bool castShadows);
    };
} // namespace OloEngine
