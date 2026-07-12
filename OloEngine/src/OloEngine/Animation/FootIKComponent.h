#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Scene/ComponentReflection.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace OloEngine
{
    // Ground-adaptation foot & hand IK (issue #631 part 3): per-foot ground
    // raycasts pull the feet onto uneven terrain, the pelvis lowers so the
    // downhill leg can reach, a grounded slow foot plants (world-locks) during
    // stance, the foot rotates to the slope (with an optional toe counter-roll),
    // and each hand can LimbIK onto a prop/ledge target.
    //
    // Runs as a post-pass ordered AFTER ApplyIKPostPass (aim/limb/chain IK) and
    // before spring bones, mutating skeleton local transforms only. The ground
    // cache is resolved Scene-side (raycasts against JoltScene from LAST tick's
    // foot pose — the animation systems run pre-PhysicsKick where Jolt queries
    // are legal, see the physics-shadow rules in Scene.cpp).
    //
    // Bones are addressed by skeleton index like IKTargetComponent; the chain
    // walks up ChainLength bones from each end-effector (ankle → knee → hip).
    struct FootIKComponent
    {
        OLO_PROPERTY()
        bool Enabled = true;

        // --- Feet ---
        OLO_PROPERTY()
        u32 LeftFootBone = 0;
        OLO_PROPERTY()
        u32 RightFootBone = 0;
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 2u)
        u32 ChainLength = 3; // ankle+knee+hip — 3 chain bones = the two leg segments

        // Optional toe bones for the slope counter-roll (see AlignFootToSlope).
        OLO_PROPERTY()
        bool EnableToeRoll = false;
        OLO_PROPERTY()
        u32 LeftToeBone = 0;
        OLO_PROPERTY()
        u32 RightToeBone = 0;

        // --- Ground probing ---
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 10.0f)
        f32 RaycastUp = 0.5f; // probe start height above the foot
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 10.0f)
        f32 RaycastDown = 1.0f; // probe reach below the foot
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 1.0f)
        f32 FootHeight = 0.1f; // ankle height above the sole when planted

        // --- Pelvis adaptation ---
        OLO_PROPERTY()
        bool AdjustPelvis = true;
        OLO_PROPERTY()
        u32 PelvisBone = 0;
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 2.0f)
        f32 MaxPelvisDrop = 0.4f; // furthest the hips may lower to reach ground
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 100.0f)
        f32 PelvisLerpSpeed = 10.0f; // smoothing rate (1/s) for the pelvis offset

        // --- Foot planting (world-lock during stance) ---
        OLO_PROPERTY()
        bool FootLock = true;
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 10.0f)
        f32 PlantVelocityThreshold = 0.15f; // world speed (m/s) below which a grounded foot plants
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 1.0f)
        f32 PlantLiftThreshold = 0.06f; // animated lift (m) above which a planted foot releases
        OLO_SERIALIZE(Clamp, Min = 0.01f, Max = 2.0f)
        f32 UnlockBlendTime = 0.12f; // seconds to blend a released foot back to animation

        // --- Slope alignment ---
        OLO_PROPERTY()
        bool AlignFootToSlope = true;
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 90.0f)
        f32 MaxSlopeAngle = 50.0f; // steeper ground is treated as this angle

        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 1.0f)
        f32 Weight = 1.0f; // 0 = animation, 1 = full ground adaptation

        // --- Hand IK (props / ledges) ---
        OLO_PROPERTY()
        bool LeftHandEnabled = false;
        OLO_PROPERTY()
        u32 LeftHandBone = 0;
        glm::vec3 LeftHandTarget{ 0.0f }; // world-space; overridden per frame by the entity below
        UUID LeftHandTargetEntity = 0;

        OLO_PROPERTY()
        bool RightHandEnabled = false;
        OLO_PROPERTY()
        u32 RightHandBone = 0;
        glm::vec3 RightHandTarget{ 0.0f };
        UUID RightHandTargetEntity = 0;

        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 2u)
        u32 HandChainLength = 3; // hand+elbow+shoulder — 3 chain bones = the two arm segments
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 1.0f)
        f32 HandWeight = 1.0f;

        // Trivially-copyable POD component: whole-struct bitwise compare matches
        // the editor's tier-1 memcmp undo (docs/agent-rules/cpp-coding-quality.md §7).
        auto operator==(const FootIKComponent& o) const -> bool
        {
            return Math::BitwiseEqual(*this, o);
        }
    };

    // Per-foot runtime adaptation state (ground cache, plant lock, velocity
    // history). Not named *Component — plain nested data.
    struct FootIKFootState
    {
        // Ground cache, refreshed Scene-side each tick before the animation update.
        bool HasGround = false;
        glm::vec3 GroundPoint{ 0.0f }; // world space
        glm::vec3 GroundNormal{ 0.0f, 1.0f, 0.0f };

        // Plant lock
        bool Locked = false;
        glm::vec3 LockedWorldPos{ 0.0f };
        f32 UnlockBlend = 0.0f; // 1 = fully locked, eases to 0 over UnlockBlendTime

        // Previous-tick world position of the (post-IK) foot, for plant detection.
        bool HasPrev = false;
        glm::vec3 PrevWorldPos{ 0.0f };
    };

    // Runtime-only adaptation state for FootIKComponent. Created on demand by
    // the Scene's animation update; deliberately NOT serialized and NOT in the
    // AllComponents tuple — a scene copy (play mode) starts adaptation fresh
    // from the animated pose.
    struct FootIKStateComponent
    {
        FootIKFootState Left;
        FootIKFootState Right;

        // Smoothed pelvis offset along world up (<= 0; negative = lowered).
        f32 PelvisOffset = 0.0f;

        // Hand targets resolved Scene-side (entity overrides applied), world space.
        bool LeftHandActive = false;
        glm::vec3 LeftHandResolvedTarget{ 0.0f };
        bool RightHandActive = false;
        glm::vec3 RightHandResolvedTarget{ 0.0f };
    };
} // namespace OloEngine
