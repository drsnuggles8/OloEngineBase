#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Animation/SkeletonData.h"

#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::Animation
{
    /**
     * @brief Bone correspondence between a SOURCE skeleton and a TARGET skeleton.
     *
     * Stores, for every target bone, the index of the source bone whose animation
     * drives it (or kUnmapped). Built by matching bone names: an exact pass first,
     * then a normalized pass (namespace/rig prefix stripped, case-folded, separators
     * removed) so common humanoid naming differences resolve — e.g. the source's
     * "mixamorig:LeftArm" maps to the target's "Left_Arm".
     *
     * Full humanoid-bone-enum mapping (relating anatomically equivalent bones whose
     * names share nothing — e.g. 3ds Max biped "Bip01 L UpperArm" <-> UE "upperarm_l")
     * is a documented follow-up; see docs/animation-retargeting.md.
     */
    class SkeletonRetargetMap
    {
      public:
        static constexpr int kUnmapped = -1;

        SkeletonRetargetMap() = default;

        /**
         * @brief Build a map by matching target bone names against source bone names.
         *
         * Each target bone resolves via an exact name match first; failing that, via
         * its normalized form (see NormalizeBoneName). When several source bones share
         * the same exact or normalized name, the first one encountered wins. Target
         * bones with no match are left kUnmapped.
         */
        [[nodiscard]] static SkeletonRetargetMap BuildByName(const SkeletonData& source, const SkeletonData& target);

        /// Manually set (or clear, with kUnmapped) the source bone driving a target bone.
        void SetBoneMapping(int targetBoneIndex, int sourceBoneIndex);

        /// Source bone index driving @p targetBoneIndex, or kUnmapped.
        [[nodiscard]] int GetSourceBone(int targetBoneIndex) const;
        [[nodiscard]] bool HasMapping(int targetBoneIndex) const;

        [[nodiscard]] sizet GetTargetBoneCount() const { return m_TargetToSource.size(); }
        [[nodiscard]] sizet GetMappedBoneCount() const;

        [[nodiscard]] const std::vector<int>& GetTargetToSourceTable() const { return m_TargetToSource; }

        /**
         * @brief Normalize a bone name for fuzzy matching.
         *
         * Strips the namespace/rig prefix (everything up to and including the last
         * ':' or '|'), drops every non-alphanumeric character, and lower-cases the
         * rest — so "mixamorig:LeftArm", "Armature|Left_Arm" and "left arm" all
         * collapse to "leftarm". Exposed for tests.
         */
        [[nodiscard]] static std::string NormalizeBoneName(std::string_view name);

      private:
        // index: target bone, value: source bone index (or kUnmapped)
        std::vector<int> m_TargetToSource;
    };
} // namespace OloEngine::Animation
