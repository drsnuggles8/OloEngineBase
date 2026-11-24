#include "OloEnginePCH.h"

#include "OloEngine/Gameplay/Strategy/StrategyCameraRig.h"

#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/SceneQueries.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/constants.hpp>

namespace OloEngine
{
    std::optional<f32> QueryStrategyGround(JoltScene& physics, glm::vec2 xz, f32 radius, f32 bottom, f32 top)
    {
        // Start with the entire near-plane sphere above the search ceiling.
        // A sphere already intersecting the ground reports zero travel.
        const f32 startY = top + std::max(radius, 0.0f);
        const f32 travel = startY - bottom;
        const glm::vec3 origin{ xz.x, startY, xz.y };
        constexpr glm::vec3 down{ 0.0f, -1.0f, 0.0f };
        SceneQueryHit hit;
        const bool found = radius > 0.0f
                               ? physics.CastShape(SphereCastInfo(origin, down, radius, travel), hit)
                               : physics.CastRay(RayCastInfo(origin, down, travel), hit);
        if (found)
            return startY - hit.m_Distance;
        return std::nullopt;
    }

    StrategyCameraPose StepStrategyCamera(
        StrategyCameraState& state, const StrategyCameraSettings& settings, f32 dt,
        const StrategyCameraIntent& intent, f32 nearRadius, const StrategyGroundQuery& ground)
    {
        constexpr f32 smoothingSeconds = 0.12f;
        constexpr f32 clearance = 1.0f;
        const f32 alpha = -std::expm1(-dt / smoothingSeconds);
        state.TargetYaw = std::remainder(state.TargetYaw + intent.Orbit.x, glm::two_pi<f32>());
        state.TargetPitch = std::clamp(state.TargetPitch + intent.Orbit.y, glm::radians(25.0f), glm::radians(85.0f));
        state.TargetLogDistance = std::clamp(state.TargetLogDistance + intent.LogZoom,
                                             std::log(settings.MinDistance), std::log(settings.MaxDistance));
        state.Yaw = std::remainder(state.Yaw + alpha * std::remainder(state.TargetYaw - state.Yaw, glm::two_pi<f32>()),
                                   glm::two_pi<f32>());
        state.Pitch = std::lerp(state.Pitch, state.TargetPitch, alpha);
        state.LogDistance = std::lerp(state.LogDistance, state.TargetLogDistance, alpha);

        const f32 distance = std::exp(state.LogDistance);
        const glm::vec2 right{ std::cos(state.Yaw), -std::sin(state.Yaw) };
        const glm::vec2 forward{ -std::sin(state.Yaw), -std::cos(state.Yaw) };
        const f32 panLength = std::max(1.0f, glm::length(intent.Pan));
        const glm::vec2 move = intent.Pan * (0.8f * dt / panLength) + intent.Drag;
        state.TargetFocus = glm::clamp(state.TargetFocus + (right * move.x + forward * move.y) * distance,
                                       settings.MinFocus, settings.MaxFocus);
        state.Focus = glm::mix(state.Focus, state.TargetFocus, alpha);
        if (const auto height = ground(state.Focus, 0.0f))
            state.Height = std::lerp(state.Height, *height, alpha);

        const glm::vec2 cameraXZ = state.Focus - forward * (distance * std::cos(state.Pitch));
        glm::vec3 position{ cameraXZ.x, state.Height + distance * std::sin(state.Pitch), cameraXZ.y };
        if (const auto contact = ground(cameraXZ, nearRadius))
            position.y = std::max(position.y, *contact + clearance);

        const glm::vec3 toFocus = glm::vec3(state.Focus.x, state.Height, state.Focus.y) - position;
        return { position, { std::atan2(toFocus.y, glm::length(glm::vec2(toFocus.x, toFocus.z))), state.Yaw, 0.0f } };
    }
} // namespace OloEngine
