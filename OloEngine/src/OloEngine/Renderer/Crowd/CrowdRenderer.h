#pragma once

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Renderer/Crowd/CrowdCull.h"
#include "OloEngine/Renderer/Crowd/CrowdIndividual.h"
#include "OloEngine/Renderer/Crowd/CrowdLod.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"

#include <memory>
#include <optional>
#include <map>
#include <unordered_map>

namespace OloEngine
{
    class MaterialOverrideCache;
    struct CrowdDrawState
    {
        const Material* Override;
        const MaterialOverrideCache* Patches;
        const Material& Default;
        GPUSceneMaterialOverrideLane Lane;
        bool CastShadows;
        std::optional<u64> MorphStateHash; // present for a mutable, CPU-morphed surface
        f32 SkinExpressionDetail = 0.0f;
    };

    // Scene-owned caches. Animation time and transform history advance once per
    // physical frame. Culling buffers and the hysteresis decision belong to a view.
    class CrowdRenderer
    {
      public:
        bool Submit(u64 owner, i32 pickingID, const Ref<MeshSource>& source, const Ref<Skeleton>& rig,
                    const AnimationStateComponent& animation, const CrowdAnimationComponent& settings,
                    std::span<const InstanceData> placements, sizet inlineCount,
                    const CrowdDrawState& materials, bool castShadows);
        void Remove(u64 owner);
        void FinishView();
        [[nodiscard]] CrowdVisibilityCounts ReadVisibilityCounts() const;
        ~CrowdRenderer();
        CrowdRenderer();

      private:
        struct Batch;
        struct Owner;
        std::unordered_map<u64, std::unique_ptr<Owner>> m_Owners;
        CrowdIndividual m_Individual;
        u64 m_LastPoseHistoryResetFrame = 0;
        std::map<std::pair<u64, i32>, std::unique_ptr<Batch>> m_Batches;
        bool SubmitGroup(u64 owner, i32 clipIndex, i32 pickingID, const Ref<MeshSource>& source, const Ref<Skeleton>& rig,
                         const AnimationStateComponent& animation, const CrowdAnimationComponent& settings,
                         std::span<const InstanceData> placements,
                         const CrowdDrawState& materials, bool castShadows);
    };
} // namespace OloEngine
