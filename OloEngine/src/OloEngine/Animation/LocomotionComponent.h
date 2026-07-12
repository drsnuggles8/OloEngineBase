#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/ComponentReflection.h"

#include <glm/glm.hpp>
#include <string>
#include <unordered_map>

namespace OloEngine
{
    // Velocity-driven locomotion controller (issue #631 part 4): maps the
    // character's velocity, local movement direction, and turn rate onto the
    // animation graph's parameters each tick, selects a gait with hysteresis,
    // and stride-warps the active locomotion state's playback rate so the
    // clip's authored stride speed matches the actual ground speed (killing
    // foot-slide for velocity-driven characters; root-motion characters get
    // their movement FROM the clips, so their feet cannot slide by
    // construction).
    //
    // The velocity source is measured by default — the Jolt character
    // controller's linear velocity when the entity has one, else the entity's
    // transform delta — or, for root-motion characters steered by gameplay
    // code, the DesiredVelocity field (scripts/AI write it, the controller
    // animates toward it and the clips move the body).
    //
    // Parameters are Defined on the graph if missing, so a graph only has to
    // reference the names it cares about (e.g. a 1D tree on "Speed", a 2D
    // directional tree on "MoveX"/"MoveY", transitions on "Gait").
    struct LocomotionComponent
    {
        OLO_PROPERTY()
        bool Enabled = true;

        // --- Graph parameter names ---
        std::string SpeedParameter = "Speed";      // f32: smoothed planar speed (m/s)
        std::string DirectionXParameter = "MoveX"; // f32: local strafe, normalized [-1, 1]
        std::string DirectionYParameter = "MoveY"; // f32: local forward, normalized [-1, 1]
        std::string GaitParameter = "Gait";        // i32: 0 = idle, 1 = walk, 2 = run
        std::string TurnParameter = "Turn";        // f32: smoothed yaw rate (rad/s)

        // --- Velocity source ---
        // When true, gameplay code steers via DesiredVelocity below (the
        // root-motion workflow); when false, velocity is measured (controller
        // linear velocity, else transform deltas).
        OLO_PROPERTY()
        bool UseDesiredVelocity = false;
        OLO_PROPERTY()
        OLO_SERIALIZE(Skip)
        glm::vec3 DesiredVelocity{ 0.0f }; // world space, runtime input

        // --- Gait selection with hysteresis (exit < enter, so a speed hovering
        // at a boundary can't flicker between gaits) ---
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 100.0f)
        f32 WalkEnterSpeed = 0.15f;
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 100.0f)
        f32 WalkExitSpeed = 0.08f;
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 100.0f)
        f32 RunEnterSpeed = 3.0f;
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 100.0f)
        f32 RunExitSpeed = 2.5f;

        // Exponential smoothing rate for speed/turn (1/s); higher = snappier.
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 100.0f)
        f32 SpeedSmoothing = 12.0f;

        // Speed used to normalize MoveX/MoveY into [-1, 1] (typically the run speed).
        OLO_SERIALIZE(Clamp, Min = 0.01f, Max = 100.0f)
        f32 DirectionReferenceSpeed = 4.0f;

        // --- Stride warping ---
        OLO_PROPERTY()
        bool StrideWarp = true;
        // Ground speed (m/s) each gait's clips are authored at (playback 1.0);
        // 0 disables warping for that gait.
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 100.0f)
        f32 WalkClipSpeed = 1.4f;
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 100.0f)
        f32 RunClipSpeed = 4.0f;
        // Playback-rate clamp: the warp never scales a state faster/slower than
        // this factor (and its inverse) of the authored speed.
        OLO_SERIALIZE(Clamp, Min = 1.0f, Max = 4.0f)
        f32 MaxStrideScale = 1.5f;

        // Deliberately NOT trivially-copyable-comparable (strings) — equality
        // for editor undo (tier 2) covers the authored settings.
        auto operator==(const LocomotionComponent&) const -> bool = default;
    };

    // Runtime-only controller state for LocomotionComponent. Created on demand
    // by the locomotion system; deliberately NOT serialized and NOT in the
    // AllComponents tuple — a scene copy (play mode) restarts from idle.
    struct LocomotionStateComponent
    {
        f32 SmoothedSpeed = 0.0f;
        f32 SmoothedTurnRate = 0.0f;
        i32 CurrentGait = 0; // 0 idle, 1 walk, 2 run

        bool HasPrev = false;
        glm::vec3 PrevPosition{ 0.0f };
        f32 PrevYaw = 0.0f;

        // Authored playback speeds of graph states the stride warp has touched,
        // keyed by state name — the warp scales relative to these and restores
        // them when it turns off.
        std::unordered_map<std::string, f32> BaseStateSpeeds;
    };
} // namespace OloEngine
