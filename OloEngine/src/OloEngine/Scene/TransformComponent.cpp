#include "OloEnginePCH.h"
#include "Components.h"
#include "OloEngine/Math/Math.h"

namespace OloEngine
{
    void TransformComponent::SetTransform(const glm::mat4& transform)
    {
        glm::vec3 translation;
        glm::vec3 euler;
        glm::vec3 scale;
        if (!Math::DecomposeTransform(transform, translation, euler, scale))
        {
            return; // Decomposition failed; leave component unchanged
        }
        Translation = translation;
        Scale = scale;
        SetRotation(glm::quat(euler));
    }
} // namespace OloEngine
