#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Animation/AnimationParameter.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>

namespace OloEngine
{
    struct BoneTransform
    {
        glm::vec3 Translation = glm::vec3(0.0f);
        glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 Scale = glm::vec3(1.0f);
    };

    class BlendNode : public RefCounted
    {
      public:
        virtual ~BlendNode() = default;
        virtual void Evaluate(f32 normalizedTime, const AnimationParameterSet& params,
                              sizet boneCount,
                              std::vector<BoneTransform>& outBoneTransforms) const = 0;
        virtual f32 GetDuration(const AnimationParameterSet& params) const = 0;
    };
} // namespace OloEngine
