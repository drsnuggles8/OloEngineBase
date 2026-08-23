#pragma once

// =============================================================================
// Which local axis an entity treats as "forward" (issue #897).
//
// The engine has TWO forward conventions and both are legitimate:
//
//   * -Z forward — cameras, lights, the fly-cam basis in Scene::RenderRuntime,
//     glm::quatLookAt, and PlayerRigComponent (whose body yaw is written by
//     PlayerRigSystem::YawRotation, which maps -Z).
//   * +Z forward — every force-model / constrained vehicle, matching Jolt:
//     BoatSystem reads hull forward as rotation * (0,0,+1), and
//     VehicleComponent / AircraftComponent document the same.
//
// ── Starboard is local -X for a +Z-forward body ──────────────────────────────
// Right-handed, +Y up, right == forward x up. For the camera convention that
// gives the familiar (0,0,-1) x (0,1,0) == (+1,0,0). Turn the body round to
// +Z forward and the same formula gives (0,0,1) x (0,1,0) == (-1,0,0): +X is
// to PORT. Jolt agrees — VehicleConstraint's wheel basis is forward x up, and
// Wheel::mSteerAngle ("positive is to the left") is a rotation about the
// suspension direction that takes +Z toward +X.
//
// Getting that backwards is what #897 was: a boat that turned to port when the
// player pressed starboard, an aircraft that rolled and yawed the wrong way,
// and a chase camera that parked itself ahead of the hull looking back at it.
// None of it failed a test, because every one of those is a sign, not a
// magnitude — see docs/agent-rules/force-model-vehicles.md.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace OloEngine
{
    class Entity;

    // Which local axis a thing points along. Serialized as an int (see the
    // OLO_SERIALIZE(Reject, ...) on CameraRigComponent::m_TargetForward), so the
    // numbering is on disk — append, never renumber.
    enum class ForwardConvention : u8
    {
        // Work it out from the target's own components. This is what a scene
        // should almost always leave it on.
        Auto = 0,
        // Camera / player convention: forward is rotation * (0, 0, -1).
        MinusZ = 1,
        // Jolt / vehicle convention: forward is rotation * (0, 0, +1).
        PlusZ = 2,
    };

    namespace EntityFacing
    {
        // The local forward axis for a convention. `Auto` has no axis of its
        // own and resolves to the camera convention here — callers that have an
        // entity should go through ConventionFor / Resolve instead.
        [[nodiscard("the forward axis must be used")]] constexpr glm::vec3 LocalForward(ForwardConvention convention)
        {
            return (convention == ForwardConvention::PlusZ) ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                            : glm::vec3(0.0f, 0.0f, -1.0f);
        }

        // Starboard (the occupant's right) for a convention, in local space.
        // right == forward x up with +Y up — see the header comment.
        [[nodiscard("the starboard axis must be used")]] constexpr glm::vec3 LocalStarboard(ForwardConvention convention)
        {
            return (convention == ForwardConvention::PlusZ) ? glm::vec3(-1.0f, 0.0f, 0.0f)
                                                            : glm::vec3(1.0f, 0.0f, 0.0f);
        }

        // The convention an entity's own components use. Never returns `Auto`.
        //
        // A component that drives the entity through Jolt owns the answer: a
        // hull, a chassis or an airframe is +Z forward by definition of the
        // systems that push it. Everything else — a player capsule, a prop, a
        // bare transform — keeps the camera convention, which is what the rest
        // of the engine writes into a TransformComponent.
        [[nodiscard("the resolved convention must be used")]] ForwardConvention ConventionFor(const Entity& entity);

        // ConventionFor(entity), unless `authored` names one explicitly. This is
        // the seam a scene uses to correct the guess — e.g. a proxy transform
        // that carries a vehicle's heading but has no vehicle component of its
        // own to be recognised by.
        [[nodiscard("the resolved convention must be used")]] ForwardConvention Resolve(const Entity& entity, ForwardConvention authored);

        // World-space forward of a rotation under a given convention.
        [[nodiscard("the forward vector must be used")]] glm::vec3 Forward(const glm::quat& rotation, ForwardConvention convention);

        // Yaw in degrees about world +Y, in the SAME angle space as
        // PlayerRigSystem::YawRotation — i.e. YawRotation(YawDegrees(q, c))
        // reproduces the planar part of `q`'s forward under convention `c`.
        // Returns 0 for a degenerate (straight up/down) or non-finite rotation,
        // where yaw is undefined.
        [[nodiscard("the yaw must be used")]] f32 YawDegrees(const glm::quat& rotation, ForwardConvention convention);
    } // namespace EntityFacing
} // namespace OloEngine
