#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/AnimationParameter.h"
#include "OloEngine/Animation/AnimationLayer.h"
#include "OloEngine/Animation/BlendNode.h"
#include "OloEngine/Animation/RootMotion.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <span>
#include <string>
#include <vector>
#include <unordered_set>

namespace OloEngine
{
    class AnimationGraph : public RefCounted
    {
      public:
        AnimationParameterSet Parameters;
        std::vector<AnimationLayer> Layers;

        void Start();
        // bindPoseLocal supplies each bone's rest local transform so bones a clip
        // does not animate fall back to bind pose rather than identity (#543); it
        // may be empty (identity fallback). boneNames drives by-name channel mapping.
        // bindPoseGlobals/preTransforms feed root-motion model-space conversion
        // (issue #631); empty spans degrade that conversion to identity.
        void Update(f32 dt, sizet boneCount, std::vector<glm::mat4>& outFinalBoneMatrices,
                    const std::vector<std::string>& boneNames,
                    const std::vector<i32>& parentIndices,
                    const std::vector<BoneTransform>& bindPoseLocal,
                    std::span<const glm::mat4> bindPoseGlobals = {},
                    std::span<const glm::mat4> preTransforms = {});

        // Root-motion delta the last Update() extracted from the BASE layer
        // (layer 0, Override mode), scaled by the layer weight. Additive and
        // upper layers do not contribute root motion by design — matching the
        // usual layered-locomotion convention (issue #631).
        [[nodiscard("extracted delta must be used")]] const Animation::RootMotionDelta& GetLastRootMotion() const
        {
            return m_LastRootMotion;
        }

        [[nodiscard("cloned graph must be assigned")]] Ref<AnimationGraph> Clone() const;
        void ResolveClips(const std::vector<Ref<AnimationClip>>& availableClips);
        [[nodiscard("check needed to avoid sampling missing clips")]] bool HasUnresolvedClips() const;
        [[nodiscard("state name needed for UI or logic")]] const std::string& GetCurrentStateName(i32 layerIndex) const;
        [[nodiscard("state name needed for UI or logic")]] const std::string& GetCurrentStateName() const
        {
            return GetCurrentStateName(0);
        }
        [[nodiscard("transition check needed for blend decisions")]] bool IsInTransition(i32 layerIndex) const;
        [[nodiscard("transition check needed for blend decisions")]] bool IsInTransition() const
        {
            return IsInTransition(0);
        }

        // Morph-target (blend-shape) sampling support: the active single clip on
        // a layer plus the clip-space time (seconds) at which to sample its
        // morph keyframes.
        struct ActiveMorphClip
        {
            Ref<AnimationClip> Clip;
            f64 Time = 0.0;
        };

        // Collects one ActiveMorphClip per layer whose current state is a
        // SingleClip carrying morph keyframes, in layer order (so a later
        // facial layer overrides an earlier one when sampled in sequence).
        // Blend-tree states and — during a cross-fade — the transition target
        // are intentionally not sampled in this CPU slice; the active (source)
        // state's clip is used. See AnimationGraphSystem::Update.
        void CollectActiveMorphClips(std::vector<ActiveMorphClip>& outClips) const;

      private:
        void ApplyLayerTransforms(const AnimationLayer& layer,
                                  const std::vector<BoneTransform>& layerTransforms,
                                  const std::vector<std::string>& boneNames,
                                  std::vector<BoneTransform>& accumulatedTransforms) const;
        bool IsBoneAffected(const AnimationLayer& layer, const std::string& boneName) const;

        static glm::mat4 BoneTransformToMatrix(const BoneTransform& transform);

        static const std::string s_EmptyString;

        // Per-instance pose-evaluation scratch, reused across Update() calls
        // (issue #445): every callee here (FillBindPose / StateMachine::Update /
        // BlendTree::Evaluate) only ever resize()s these, never clears+regrows
        // from empty, so keeping them as instance storage instead of Update()
        // locals turns the per-frame allocation into a one-time warm-up cost
        // (first tick / bone-count change) instead of steady-state heap churn.
        // Each entity owns its own AnimationGraph instance (Scene.cpp Clone()s
        // one per entity) and the AnimationGraph/AnimationGraphSystem gameplay
        // step is not marked .Parallelizable(), so this is safe without locking.
        std::vector<BoneTransform> m_ScratchAccumulated;
        std::vector<BoneTransform> m_ScratchLayer;

        // Root-motion delta extracted by the last Update() (see GetLastRootMotion)
        Animation::RootMotionDelta m_LastRootMotion;
    };
} // namespace OloEngine
