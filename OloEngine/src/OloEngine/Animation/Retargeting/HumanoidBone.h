#pragma once

#include "OloEngine/Core/Base.h"

#include <string_view>

namespace OloEngine::Animation
{
    /**
     * @brief Canonical humanoid bone roles, modeled on Unity's HumanBodyBones and
     *        Unreal's humanoid rig.
     *
     * Used to relate anatomically-equivalent bones across skeletons whose bone
     * NAMES share nothing — e.g. 3ds Max biped "Bip01 L UpperArm" <-> UE
     * "upperarm_l" <-> Mixamo "LeftArm" all denote the same role
     * (HumanoidBone::LeftUpperArm). Name normalization (SkeletonRetargetMap::
     * BuildByName) cannot bridge these; a role enum can.
     *
     * Pragmatic, not exhaustive: spine, neck/head, and the L/R arm and leg chains
     * down to a single toe joint — no individual fingers, eyes, or jaw. The real
     * values are contiguous in [0, Count) so a role can index a fixed-size array;
     * None is the unassigned sentinel and is deliberately outside that range.
     *
     * @see docs/design/animation-retargeting.md
     */
    enum class HumanoidBone : i32
    {
        None = -1,

        // Spine column (center, side-independent).
        Hips = 0,
        Spine,
        Chest,
        UpperChest,
        Neck,
        Head,

        // Left arm chain.
        LeftShoulder,
        LeftUpperArm,
        LeftLowerArm,
        LeftHand,

        // Right arm chain.
        RightShoulder,
        RightUpperArm,
        RightLowerArm,
        RightHand,

        // Left leg chain.
        LeftUpperLeg,
        LeftLowerLeg,
        LeftFoot,
        LeftToes,

        // Right leg chain.
        RightUpperLeg,
        RightLowerLeg,
        RightFoot,
        RightToes,

        Count
    };

    /// Number of real humanoid roles (excludes None and the Count sentinel).
    inline constexpr sizet HumanoidBoneCount = static_cast<sizet>(HumanoidBone::Count);

    /// Stable lower-camel name for a role ("leftUpperArm"); "none" for None / out of
    /// range. Exposed for tests, logging, and editor display.
    [[nodiscard]] constexpr std::string_view ToString(HumanoidBone bone) noexcept
    {
        switch (bone)
        {
            case HumanoidBone::Hips:
                return "hips";
            case HumanoidBone::Spine:
                return "spine";
            case HumanoidBone::Chest:
                return "chest";
            case HumanoidBone::UpperChest:
                return "upperChest";
            case HumanoidBone::Neck:
                return "neck";
            case HumanoidBone::Head:
                return "head";
            case HumanoidBone::LeftShoulder:
                return "leftShoulder";
            case HumanoidBone::LeftUpperArm:
                return "leftUpperArm";
            case HumanoidBone::LeftLowerArm:
                return "leftLowerArm";
            case HumanoidBone::LeftHand:
                return "leftHand";
            case HumanoidBone::RightShoulder:
                return "rightShoulder";
            case HumanoidBone::RightUpperArm:
                return "rightUpperArm";
            case HumanoidBone::RightLowerArm:
                return "rightLowerArm";
            case HumanoidBone::RightHand:
                return "rightHand";
            case HumanoidBone::LeftUpperLeg:
                return "leftUpperLeg";
            case HumanoidBone::LeftLowerLeg:
                return "leftLowerLeg";
            case HumanoidBone::LeftFoot:
                return "leftFoot";
            case HumanoidBone::LeftToes:
                return "leftToes";
            case HumanoidBone::RightUpperLeg:
                return "rightUpperLeg";
            case HumanoidBone::RightLowerLeg:
                return "rightLowerLeg";
            case HumanoidBone::RightFoot:
                return "rightFoot";
            case HumanoidBone::RightToes:
                return "rightToes";
            case HumanoidBone::None:
            case HumanoidBone::Count:
                break;
        }
        return "none";
    }
} // namespace OloEngine::Animation
