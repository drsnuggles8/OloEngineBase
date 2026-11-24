#pragma once

#include "OloEngine/Core/Base.h"

#include <functional>
#include <glm/glm.hpp>
#include <optional>

namespace OloEngine
{
    class JoltScene;

    // Runtime state owned by the controller. Angles are radians, distances metres.
    struct StrategyCameraState
    {
        glm::vec2 Focus{};
        glm::vec2 TargetFocus{};
        f32 Height = 0.0f;
        f32 Yaw = 0.0f;
        f32 TargetYaw = 0.0f;
        f32 Pitch = 0.0f;
        f32 TargetPitch = 0.0f;
        f32 LogDistance = 0.0f;
        f32 TargetLogDistance = 0.0f;
    };

    struct StrategyCameraSettings
    {
        glm::vec2 MinFocus{};
        glm::vec2 MaxFocus{};
        f32 MinDistance = 20.0f;
        f32 MaxDistance = 17000.0f;
        f32 GroundBottom = -100.0f;
        f32 GroundTop = 2000.0f;
    };

    struct StrategyCameraPose
    {
        glm::vec3 Position{};
        glm::vec3 Rotation{};
    };

    // Pan is a local right/forward rate. The other inputs are displacements.
    struct StrategyCameraIntent
    {
        glm::vec2 Pan{};
        glm::vec2 Drag{};
        glm::vec2 Orbit{};
        f32 LogZoom = 0.0f;
    };

    // A point query (radius zero) returns the ground Y. A sphere query returns
    // the sphere centre at first contact when swept down from above.
    using StrategyGroundQuery = std::function<std::optional<f32>(glm::vec2 xz, f32 radius)>;

    [[nodiscard]] std::optional<f32> QueryStrategyGround(JoltScene& physics, glm::vec2 xz,
                                                         f32 radius, f32 bottom, f32 top);

    // nearRadius encloses the camera origin and the entire perspective near plane.
    [[nodiscard]] StrategyCameraPose StepStrategyCamera(
        StrategyCameraState& state, const StrategyCameraSettings& settings, f32 dt,
        const StrategyCameraIntent& intent, f32 nearRadius, const StrategyGroundQuery& ground);
} // namespace OloEngine
