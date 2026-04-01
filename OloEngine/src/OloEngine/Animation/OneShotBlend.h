#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Animation/BlendNode.h"
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class AnimationClip;
}

namespace OloEngine::Animation
{
    class OneShotBlend
    {
      public:
        enum class Phase : u8
        {
            Idle,
            BlendIn,
            Playing,
            BlendOut
        };

        void Trigger();
        void Cancel();
        [[nodiscard]] bool IsActive() const
        {
            return m_Phase != Phase::Idle;
        }
        [[nodiscard]] Phase GetPhase() const
        {
            return m_Phase;
        }

        // Samples the one-shot clip and blends over basePose in-place.
        // parentIndices is needed for additive/masked blending.
        // boneNames maps pose indices to bone names for clip sampling.
        void Update(
            f32 dt,
            std::span<BoneTransform> basePose,
            std::span<const int> parentIndices,
            std::span<const std::string> boneNames);

        // Configuration
        Ref<AnimationClip> Clip;
        f32 BlendInDuration = 0.1f;
        f32 BlendOutDuration = 0.1f;
        f32 Weight = 1.0f;
        bool Additive = false;
        u32 BlendRootBone = 0; // 0 = full body

        using FinishedCallback = std::function<void()>;
        FinishedCallback OnFinished;

      private:
        Phase m_Phase = Phase::Idle;
        f32 m_PlaybackTime = 0.0f;
        f32 m_PhaseTime = 0.0f; // time within the current phase (for blend-in/out)

        // Cached bone-name-to-index mapping for O(1) lookup during Update
        std::unordered_map<std::string, sizet> m_BoneNameToIndex;
        sizet m_CachedBoneCount = 0;
    };
} // namespace OloEngine::Animation
