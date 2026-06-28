#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Animation/SkeletonData.h"

#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::Animation
{
    class HumanoidBoneMap;

    /**
     * @brief Bone correspondence between a SOURCE skeleton and a TARGET skeleton.
     *
     * Stores, for every target bone, the index of the source bone whose animation
     * drives it (or kUnmapped). Two ways to build it:
     *
     * - BuildByName matches bone names: an exact pass first, then a normalized pass
     *   (namespace/rig prefix stripped, case-folded, separators removed) so common
     *   humanoid naming differences resolve — e.g. the source's "mixamorig:LeftArm"
     *   maps to the target's "Left_Arm".
     * - BuildByHumanoidRole pairs bones by their canonical HumanoidBone role instead
     *   of by name, so anatomically-equivalent bones whose names share nothing — e.g.
     *   3ds Max biped "Bip01 L UpperArm" <-> UE "upperarm_l" <-> Mixamo "LeftArm" —
     *   still map. FillUnmappedFrom then composes the two (role map first, name map
     *   for whatever the roles didn't cover).
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

        /**
         * @brief Build a map by pairing bones whose canonical HumanoidBone role matches.
         *
         * For every humanoid role assigned in BOTH @p sourceRoles and @p targetRoles,
         * the target's bone for that role is driven by the source's bone for that role.
         * Target bones with no shared role are left kUnmapped. Works even when the two
         * rigs' bone names share nothing — that is the whole point of role mapping.
         */
        [[nodiscard]] static SkeletonRetargetMap BuildByHumanoidRole(const SkeletonData& source,
                                                                     const SkeletonData& target,
                                                                     const HumanoidBoneMap& sourceRoles,
                                                                     const HumanoidBoneMap& targetRoles);

        /**
         * @brief BuildByHumanoidRole using HumanoidBoneMap::AutoDetect for both rigs.
         *
         * Convenience for the common "just retarget these two humanoid rigs" case: the
         * roles are detected from the bone names heuristically. Supply explicit
         * HumanoidBoneMaps to the four-argument overload when the heuristic needs help.
         */
        [[nodiscard]] static SkeletonRetargetMap BuildByHumanoidRole(const SkeletonData& source,
                                                                     const SkeletonData& target);

        /**
         * @brief Fill this map's still-unmapped target bones from @p fallback.
         *
         * Composes two maps without disturbing existing mappings: every target bone
         * that is kUnmapped here takes @p fallback's mapping (if any). The canonical
         * use is role map first, name map second —
         * `roleMap.FillUnmappedFrom(SkeletonRetargetMap::BuildByName(src, tgt))` — so
         * bones the roles covered keep their role mapping and the rest fall back to
         * name matching. Entries past the smaller of the two tables are left untouched.
         */
        void FillUnmappedFrom(const SkeletonRetargetMap& fallback);

        /// Manually set (or clear, with kUnmapped) the source bone driving a target bone.
        void SetBoneMapping(int targetBoneIndex, int sourceBoneIndex);

        /// Source bone index driving @p targetBoneIndex, or kUnmapped.
        [[nodiscard]] int GetSourceBone(int targetBoneIndex) const;
        [[nodiscard]] bool HasMapping(int targetBoneIndex) const;

        [[nodiscard]] sizet GetTargetBoneCount() const
        {
            return m_TargetToSource.size();
        }
        [[nodiscard]] sizet GetMappedBoneCount() const;

        [[nodiscard]] const std::vector<int>& GetTargetToSourceTable() const
        {
            return m_TargetToSource;
        }

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
