#pragma once

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Renderer/Instancing/InstanceData.h"
#include "OloEngine/Renderer/Mesh.h"

#include <unordered_map>

namespace OloEngine
{
    struct GPUSceneMaterialKey;

    // Conventional skeletal draws for a crowd part that cannot share an
    // atlas draw. A member's pose and model transform belong to the world;
    // a second submesh or view in the same frame reuses the sampled data.
    class CrowdIndividual
    {
      public:
        struct Sample
        {
            Ref<Skeleton> Pose;
            glm::mat4 Transform{ 1.0f };
            glm::mat4 PreviousTransform{ 1.0f };
        };

        [[nodiscard]] const Sample& Evaluate(u64 owner, const InstanceData& member, const Ref<Skeleton>& rig,
                                             const AnimationStateComponent& animation, u64 serial);
        void SubmitPart(u64 owner, i32 pickingID, const Ref<Mesh>& mesh, const Ref<Skeleton>& rig,
                        const AnimationStateComponent& animation, const InstanceData& member, const Material& material,
                        bool shadows, const GPUSceneMaterialKey& key, u32 visibilityMask, u64 morphStateHash = 0);
        void Remove(u64 owner);
        void FinishFrame(u64 serial);

      private:
        struct Member
        {
            Ref<Skeleton> Source;
            Sample Rendered;
            u64 Serial = 0;
        };
        std::unordered_map<u64, std::unordered_map<u64, Member>> m_Owners;
    };
} // namespace OloEngine
