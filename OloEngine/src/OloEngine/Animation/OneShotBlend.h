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

        // The unordered_map / std::function / vector members have moves that are
        // not all guaranteed noexcept; declare the moves noexcept so this type —
        // and AnimationLayer, which embeds it — moves without throwing (S5018).
        OneShotBlend() = default;
        OneShotBlend(const OneShotBlend&) = default;
        OneShotBlend& operator=(const OneShotBlend&) = default;
        OneShotBlend(OneShotBlend&&) noexcept = default;
        OneShotBlend& operator=(OneShotBlend&&) noexcept = default;

        void Trigger();
        void Cancel();
        [[nodiscard("check if one-shot is still playing")]] bool IsActive() const
        {
            return m_Phase != Phase::Idle;
        }
        [[nodiscard("phase needed to determine blend state")]] Phase GetPhase() const
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
        // Advance phase transitions for the time already accumulated this frame.
        // Returns false when the one-shot has finished (state reset + OnFinished
        // fired) and Update should stop early.
        bool AdvancePhase();

        Phase m_Phase = Phase::Idle;
        f32 m_PlaybackTime = 0.0f;
        f32 m_PhaseTime = 0.0f; // time within the current phase (for blend-in/out)

        // Cached bone-name-to-index mapping for O(1) lookup during Update
        std::unordered_map<std::string, sizet> m_BoneNameToIndex;
        sizet m_CachedBoneCount = 0;
        sizet m_CachedBoneNamesHash = 0;

        // Reusable scratch buffer to avoid per-frame heap allocation
        std::vector<BoneTransform> m_OneShotPose;

        // Snapshot of basePose captured on first additive Update after Trigger.
        // Used as the stable rest pose for computing additive deltas.
        std::vector<BoneTransform> m_AdditiveRestPose;
    };
} // namespace OloEngine::Animation
