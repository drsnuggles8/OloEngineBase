#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
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
                    const std::vector<std::string>& boneNames = {});

        [[nodiscard]] Ref<AnimationGraph> Clone() const;
        [[nodiscard]] const std::string& GetCurrentStateName(i32 layerIndex = 0) const;
        [[nodiscard]] bool IsInTransition(i32 layerIndex = 0) const;

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
