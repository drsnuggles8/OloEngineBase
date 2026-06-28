#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Animation/SkeletonData.h"
#include "OloEngine/Animation/Retargeting/HumanoidBone.h"

#include <array>
#include <string_view>

namespace OloEngine::Animation
{
    /**
     * @brief Per-skeleton assignment of bones to canonical HumanoidBone roles.
     *
     * One side of a humanoid retarget: it answers "which bone of THIS skeleton
     * plays the role of (say) LeftUpperArm?". AutoDetect() fills it heuristically
     * from the bone names — recognizing the common rig naming conventions (Mixamo,
     * Unreal, 3ds Max Biped, Blender/Rigify) — and SetBone() lets a caller override
     * a mis-detected bone or supply a role the heuristic missed. Pairing a source
     * map and a target map role-by-role yields a SkeletonRetargetMap that works
     * even when the two rigs' bone names share nothing
     * (SkeletonRetargetMap::BuildByHumanoidRole).
     *
     * @see docs/animation-retargeting.md
     */
    class HumanoidBoneMap
    {
      public:
        static constexpr int kUnassigned = -1;

        /// Constructs an empty map (every role kUnassigned).
        HumanoidBoneMap();

        /**
         * @brief Heuristically assign each bone of @p skeleton to a humanoid role.
         *
         * Per-bone name classification (see ClassifyBoneName) decides the role; the
         * first bone to claim a role keeps it. The spine column gets a whole-skeleton
         * resolution pass: the lowest-numbered "spine*" bone becomes Spine and, when
         * there is no explicit "chest" bone and at least two spine bones exist, the
         * highest-numbered one becomes Chest (matching how engines collapse a long
         * spine onto Spine/Chest).
         */
        [[nodiscard]] static HumanoidBoneMap AutoDetect(const SkeletonData& skeleton);

        /**
         * @brief Classify a single bone name to its humanoid role.
         *
         * Tokenizes the name (stripping rig/namespace prefixes, splitting camelCase
         * and letter/digit boundaries), detects a left/right side, and matches a
         * body-part keyword. Recognizes Mixamo ("LeftForeArm"), Unreal ("lowerarm_l"),
         * 3ds Max Biped ("Bip01 L Forearm") and Blender/Rigify ("forearm.L") spellings.
         *
         * Returns the BASE role: a numbered spine bone always classifies as Spine
         * (the Spine-vs-Chest split is a whole-skeleton decision made by AutoDetect),
         * a finger/twist/IK helper bone classifies as None, and a limb keyword without
         * a detectable side returns None (a limb can't be placed without a side).
         * Exposed for tests.
         */
        [[nodiscard]] static HumanoidBone ClassifyBoneName(std::string_view boneName);

        /// Assign @p boneIndex to @p role. A negative @p boneIndex clears the role's
        /// mapping; a HumanoidBone::None or out-of-range @p role is ignored (it owns no
        /// array slot to assign or clear).
        void SetBone(HumanoidBone role, int boneIndex);

        /// Bone index assigned to @p role, or kUnassigned.
        [[nodiscard]] int GetBone(HumanoidBone role) const;

        /// Role assigned to @p boneIndex, or HumanoidBone::None.
        [[nodiscard]] HumanoidBone GetRole(int boneIndex) const;

        [[nodiscard]] bool HasRole(HumanoidBone role) const;
        [[nodiscard]] sizet GetAssignedRoleCount() const;

      private:
        // index: HumanoidBone role value in [0, Count); value: bone index (or kUnassigned)
        std::array<int, HumanoidBoneCount> m_RoleToBone;
    };
} // namespace OloEngine::Animation
