#pragma once

#include <glm/vec3.hpp>

namespace OloEngine::Audio
{
    struct Transform
    {
        glm::vec3 Position{ 0.0f, 0.0f, 0.0f };
        glm::vec3 Orientation{ 0.0f, 0.0f, -1.0f };
        glm::vec3 Up{ 0.0f, 1.0f, 0.0f };

        auto operator==(const Transform&) const -> bool = default;
    };

    static constexpr float SPEED_OF_SOUND = 343.3f;

} // namespace OloEngine::Audio
