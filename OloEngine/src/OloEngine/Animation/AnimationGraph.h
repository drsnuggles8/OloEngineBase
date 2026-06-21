#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/AnimationParameter.h"
#include "OloEngine/Animation/AnimationLayer.h"
#include "OloEngine/Animation/BlendNode.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
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
        void Update(f32 dt, sizet boneCount, std::vector<glm::mat4>& outFinalBoneMatrices,
                    const std::vector<std::string>& boneNames,
                    const std::vector<i32>& parentIndices);

        [[nodiscard("cloned graph must be assigned")]] Ref<AnimationGraph> Clone() const;
        void ResolveClips(const std::vector<Ref<AnimationClip>>& availableClips);
        [[nodiscard("check needed to avoid sampling missing clips")]] bool HasUnresolvedClips() const;
        [[nodiscard("state name needed for UI or logic")]] const std::string& GetCurrentStateName(i32 layerIndex = 0) const;
        [[nodiscard("transition check needed for blend decisions")]] bool IsInTransition(i32 layerIndex = 0) const;

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
    };
} // namespace OloEngine
