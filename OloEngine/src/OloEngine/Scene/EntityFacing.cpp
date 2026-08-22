#include "OloEnginePCH.h"
#include "OloEngine/Scene/EntityFacing.h"

#include "OloEngine/Math/Math.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <cmath>

namespace OloEngine::EntityFacing
{
    namespace
    {
        // Below this the planar part of a forward vector is meaningless and
        // atan2 would return an arbitrary angle rather than a yaw.
        constexpr f32 kDegenerateLengthSq = 1.0e-12f;
    } // namespace

    ForwardConvention ConventionFor(const Entity& entity)
    {
        // The list is deliberately the set of components whose SYSTEM reads
        // rotation * (0,0,+1) as forward. Adding another force-model vehicle
        // (a submarine, a hovercraft) means adding it here too — the guard is
        // ForwardConventionTest's roster check, not the compiler.
        if (!entity)
            return ForwardConvention::MinusZ;

        if (entity.HasComponent<BoatComponent>() || entity.HasComponent<VehicleComponent>() ||
            entity.HasComponent<AircraftComponent>())
        {
            return ForwardConvention::PlusZ;
        }

        return ForwardConvention::MinusZ;
    }

    ForwardConvention Resolve(const Entity& entity, ForwardConvention authored)
    {
        if (authored == ForwardConvention::MinusZ || authored == ForwardConvention::PlusZ)
            return authored;
        // Anything else — Auto, or a corrupt/scripted value outside the enum —
        // falls back to the derived answer rather than to an arbitrary axis.
        return ConventionFor(entity);
    }

    glm::vec3 Forward(const glm::quat& rotation, ForwardConvention convention)
    {
        return rotation * LocalForward(convention);
    }

    f32 YawDegrees(const glm::quat& rotation, ForwardConvention convention)
    {
        // PlayerRigSystem::YawRotation(t) maps -Z to (-sin t, 0, -cos t), so
        // atan2(-x, -z) of the CONVENTION'S forward is its inverse. Deriving it
        // from the rotated vector rather than from Euler angles is what makes it
        // agree with YawRotation by construction — see the note on the (now
        // removed) local copy of this in PlayerRigSystem.cpp.
        const glm::vec3 forward = Forward(rotation, convention);
        if (!Math::IsFinite(forward))
            return 0.0f;

        const f32 planarLengthSq = (forward.x * forward.x) + (forward.z * forward.z);
        if (planarLengthSq <= kDegenerateLengthSq)
            return 0.0f; // pointing straight up/down — yaw is undefined

        return glm::degrees(std::atan2(-forward.x, -forward.z));
    }
} // namespace OloEngine::EntityFacing
