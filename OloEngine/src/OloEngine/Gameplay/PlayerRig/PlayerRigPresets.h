#pragma once

// =============================================================================
// The two rig TEMPLATES issue #645 asks for, as data (first-person /
// third-person follow).
//
// PlayerRigComponent + CameraRigComponent are one parameterised rig, not two —
// first person is simply a zero-length boom (see PlayerRigComponents.h). These
// presets are the two field sets that make it feel like one or the other, in
// ONE place, so the shipped .oloprefab assets, the editor's "add rig" defaults,
// the docs and the tests can't drift apart.
//
// Header-only and constexpr-friendly: a preset is a value, so a caller composes
// with it (`auto rig = PlayerRigPresets::FirstPerson(); rig.m_WalkSpeed = 6;`)
// rather than being locked into it.
// =============================================================================

#include "OloEngine/Gameplay/PlayerRig/PlayerRigComponents.h"

namespace OloEngine::PlayerRigPresets
{
    // ── First person ─────────────────────────────────────────────────────────
    // Body yaws with the look so the capsule (and anything parented to it —
    // arms, a weapon) always faces the camera. Movement is look-relative.

    [[nodiscard]] inline PlayerRigComponent FirstPersonPlayer()
    {
        PlayerRigComponent rig;
        rig.m_LookSensitivity = 0.15f;
        rig.m_MinPitchDeg = -85.0f;
        rig.m_MaxPitchDeg = 85.0f;
        rig.m_WalkSpeed = 4.5f;
        rig.m_SprintMultiplier = 1.8f;
        rig.m_AirControl = 0.35f;
        rig.m_MoveRelativeToLook = true;
        rig.m_YawBodyWithLook = true;
        rig.m_FaceMoveDirection = false;
        return rig;
    }

    [[nodiscard]] inline CameraRigComponent FirstPersonCamera(UUID target = 0)
    {
        CameraRigComponent rig;
        rig.m_Target = target;
        // Eye height for a 1.8 m capsule whose origin sits at its centre.
        rig.m_PivotOffset = { 0.0f, 0.7f, 0.0f };
        // Zero boom == first person: the camera IS the eye.
        rig.m_BoomLength = 0.0f;
        // Nothing to pull in at zero length; leave the probe off so the rig
        // does no per-tick scene query it can't act on.
        rig.m_CollisionEnabled = false;
        // Rigid: a first-person view must track the head 1:1 or it reads as
        // input lag.
        rig.m_PositionSmoothTime = 0.0f;
        // Subtle stride bob — off by default in the component, on here because
        // it is what makes a first-person walk feel like walking.
        rig.m_HeadBobAmplitude = 0.035f;
        rig.m_HeadBobFrequency = 1.1f;
        return rig;
    }

    // ── Third person follow ──────────────────────────────────────────────────
    // Body turns to face where it is MOVING, independently of where the camera
    // is pointed; the camera orbits on a collision-aware boom.

    [[nodiscard]] inline PlayerRigComponent ThirdPersonPlayer()
    {
        PlayerRigComponent rig;
        rig.m_LookSensitivity = 0.15f;
        // Room to look down over the character's head and up past it.
        rig.m_MinPitchDeg = -60.0f;
        rig.m_MaxPitchDeg = 55.0f;
        rig.m_WalkSpeed = 4.5f;
        rig.m_SprintMultiplier = 1.8f;
        rig.m_AirControl = 0.35f;
        rig.m_MoveRelativeToLook = true;
        rig.m_YawBodyWithLook = false;
        rig.m_FaceMoveDirection = true;
        rig.m_TurnRateDeg = 720.0f;
        return rig;
    }

    [[nodiscard]] inline CameraRigComponent ThirdPersonCamera(UUID target = 0)
    {
        CameraRigComponent rig;
        rig.m_Target = target;
        // Slight shoulder offset + head height, so the character doesn't sit
        // dead centre of frame.
        rig.m_PivotOffset = { 0.45f, 0.9f, 0.0f };
        rig.m_BoomLength = 4.0f;
        rig.m_CollisionEnabled = true;
        rig.m_ProbeRadius = 0.25f;
        rig.m_MinBoomLength = 0.6f;
        rig.m_BoomReturnSpeed = 6.0f;
        // Enough smoothing to take the edge off physics jitter, little enough
        // that the camera doesn't feel detached from the character.
        rig.m_PositionSmoothTime = 0.06f;
        rig.m_HeadBobAmplitude = 0.0f;
        return rig;
    }
} // namespace OloEngine::PlayerRigPresets
