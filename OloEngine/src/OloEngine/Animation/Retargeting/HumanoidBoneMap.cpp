#include "OloEnginePCH.h"
#include "OloEngine/Animation/Retargeting/HumanoidBoneMap.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <locale>
#include <string>
#include <vector>

namespace OloEngine::Animation
{
    namespace
    {
        // Convert a role to its [0, Count) array index (real roles only).
        constexpr sizet RoleIndex(HumanoidBone role)
        {
            return static_cast<sizet>(static_cast<i32>(role));
        }

        enum class Side
        {
            Unknown,
            Left,
            Right
        };

        // Pick the left/right variant of a limb role, or None when the side is
        // unknown — a limb bone can't be placed without knowing its side.
        constexpr HumanoidBone WithSide(Side side, HumanoidBone left, HumanoidBone right)
        {
            if (side == Side::Left)
                return left;
            if (side == Side::Right)
                return right;
            return HumanoidBone::None;
        }

        // Split a bone name into lowercase tokens. Strips the namespace/rig prefix
        // (up to the last ':' or '|', as NormalizeBoneName does), then breaks on every
        // non-alphanumeric separator AND at camelCase (lower->upper) and letter<->digit
        // boundaries — so "mixamorig:LeftForeArm" -> [left, fore, arm], "upperarm_l" ->
        // [upperarm, l], "Bip01 L Forearm" -> [bip, 01, l, forearm], "spine_03" ->
        // [spine, 03], "forearm.L" -> [forearm, l].
        //
        // Uses the locale-aware <locale> classifiers against the classic "C" locale
        // (ASCII), matching SkeletonRetargetMap::NormalizeBoneName — MISRA C++ 24.5.1
        // forbids the <cctype> functions.
        std::vector<std::string> TokenizeBoneName(std::string_view name)
        {
            if (const sizet sep = name.find_last_of(":|"); sep != std::string_view::npos)
                name.remove_prefix(sep + 1);

            const std::locale& loc = std::locale::classic();
            std::vector<std::string> tokens;
            std::string current;

            const auto flush = [&tokens, &current]()
            {
                if (!current.empty())
                {
                    tokens.push_back(std::move(current));
                    current.clear();
                }
            };

            char prev = '\0';
            for (const char c : name)
            {
                if (!std::isalnum(c, loc))
                {
                    flush();
                    prev = '\0';
                    continue;
                }

                if (!current.empty())
                {
                    const bool camelBoundary = std::isupper(c, loc) && std::islower(prev, loc);
                    const bool digitBoundary = std::isdigit(c, loc) != std::isdigit(prev, loc);
                    if (camelBoundary || digitBoundary)
                        flush();
                }

                current.push_back(static_cast<char>(std::tolower(c, loc)));
                prev = c;
            }
            flush();
            return tokens;
        }

        // Trailing decimal value of a key ("spine03" -> 3, "spine" -> 0). Exception-free.
        int TrailingNumber(const std::string& key)
        {
            sizet start = key.size();
            const std::locale& loc = std::locale::classic();
            while (start > 0 && std::isdigit(key[start - 1], loc))
                --start;
            if (start == key.size())
                return 0;
            int value = 0;
            std::from_chars(key.data() + start, key.data() + key.size(), value);
            return value;
        }

        bool Contains(const std::string& haystack, std::string_view needle)
        {
            return haystack.find(needle) != std::string::npos;
        }

        // Map a side-stripped, joined key + detected side to a humanoid role.
        // outSpineNumber is set for spine bones (used by the whole-skeleton
        // Spine/Chest resolution pass).
        HumanoidBone ClassifyKey(const std::string& key, Side side, int& outSpineNumber)
        {
            outSpineNumber = 0;
            if (key.empty())
                return HumanoidBone::None;

            // Helper / non-skinning bones never carry a humanoid role. Twist/IK helpers
            // are rejected before the limb chains so they don't steal a limb role.
            if (Contains(key, "twist") || Contains(key, "roll") || Contains(key, "ik") ||
                Contains(key, "pole") || Contains(key, "target"))
                return HumanoidBone::None;
            // Fingers. Unambiguous finger words disqualify outright; "index"/"middle"/
            // "ring" only count as fingers alongside "hand" (the Mixamo "LeftHandIndex1"
            // pattern, which embeds "hand" and would otherwise grab the Hand role) so a
            // descriptive bone like "MiddleSpine" isn't mistaken for a finger. Standalone
            // UE finger bones ("index_01_l") match no limb keyword and resolve to None on
            // their own.
            if (Contains(key, "finger") || Contains(key, "thumb") || Contains(key, "pinky"))
                return HumanoidBone::None;
            if (Contains(key, "hand") &&
                (Contains(key, "index") || Contains(key, "middle") || Contains(key, "ring")))
                return HumanoidBone::None;

            // Spine column (center, side-independent). Check most-specific first so a
            // "chest"/"upperchest" bone isn't swallowed by the "spine" rule, and so
            // the bare "spine" rule doesn't fire on them.
            if (Contains(key, "pelvis") || Contains(key, "hips") || Contains(key, "hip"))
                return HumanoidBone::Hips;
            if (Contains(key, "upperchest"))
                return HumanoidBone::UpperChest;
            if (Contains(key, "chest"))
                return HumanoidBone::Chest;
            if (Contains(key, "spine"))
            {
                outSpineNumber = TrailingNumber(key);
                return HumanoidBone::Spine;
            }
            if (Contains(key, "neck"))
                return HumanoidBone::Neck;
            if (Contains(key, "head"))
                return HumanoidBone::Head;

            // Arm chain. Order matters: the specific spellings must beat the bare
            // "arm" rule (Mixamo "Arm" == upper arm), since "forearm"/"upperarm"
            // both contain "arm".
            if (Contains(key, "clavicle") || Contains(key, "shoulder"))
                return WithSide(side, HumanoidBone::LeftShoulder, HumanoidBone::RightShoulder);
            if (Contains(key, "forearm") || Contains(key, "lowerarm") || Contains(key, "loarm"))
                return WithSide(side, HumanoidBone::LeftLowerArm, HumanoidBone::RightLowerArm);
            if (Contains(key, "upperarm") || Contains(key, "uparm"))
                return WithSide(side, HumanoidBone::LeftUpperArm, HumanoidBone::RightUpperArm);
            if (Contains(key, "hand"))
                return WithSide(side, HumanoidBone::LeftHand, HumanoidBone::RightHand);
            if (Contains(key, "arm"))
                return WithSide(side, HumanoidBone::LeftUpperArm, HumanoidBone::RightUpperArm);

            // Leg chain. Same idea: "upleg"/"upperleg" and "lowerleg" contain "leg",
            // so the bare "leg" rule (Mixamo "Leg" == lower leg) comes last.
            if (Contains(key, "thigh") || Contains(key, "upleg") || Contains(key, "upperleg"))
                return WithSide(side, HumanoidBone::LeftUpperLeg, HumanoidBone::RightUpperLeg);
            if (Contains(key, "calf") || Contains(key, "shin") || Contains(key, "lowerleg") ||
                Contains(key, "loleg"))
                return WithSide(side, HumanoidBone::LeftLowerLeg, HumanoidBone::RightLowerLeg);
            if (Contains(key, "toe") || Contains(key, "ball"))
                return WithSide(side, HumanoidBone::LeftToes, HumanoidBone::RightToes);
            if (Contains(key, "foot") || Contains(key, "ankle"))
                return WithSide(side, HumanoidBone::LeftFoot, HumanoidBone::RightFoot);
            if (Contains(key, "leg"))
                return WithSide(side, HumanoidBone::LeftLowerLeg, HumanoidBone::RightLowerLeg);

            return HumanoidBone::None;
        }

        // Full single-name classification: tokenize, pull out the side and rig-noise
        // tokens, join the remainder into a keyword, then ClassifyKey.
        HumanoidBone ClassifyImpl(std::string_view boneName, int& outSpineNumber)
        {
            outSpineNumber = 0;
            const std::vector<std::string> tokens = TokenizeBoneName(boneName);

            Side side = Side::Unknown;
            std::string key;
            bool sawBip = false;
            bool droppedBipIndex = false;

            for (const std::string& tok : tokens)
            {
                // Side markers: standalone tokens, as produced by camelCase ("Left"),
                // suffixes ("_l", ".r" -> "l"/"r") and biped ("L"/"R") across all four
                // supported conventions.
                if (tok == "left" || tok == "l" || tok == "lft")
                {
                    side = Side::Left;
                    continue;
                }
                if (tok == "right" || tok == "r" || tok == "rgt")
                {
                    side = Side::Right;
                    continue;
                }

                // Rig-prefix noise. "bip" plus the biped index digits ("Bip01") and the
                // common control/deform prefixes carry no role information.
                if (tok == "bip")
                {
                    sawBip = true;
                    continue;
                }
                if (tok == "def" || tok == "org" || tok == "mch" || tok == "ctrl" || tok == "ctl" ||
                    tok == "tweak" || tok == "deform" || tok == "armature" || tok == "mixamorig")
                    continue;

                // The first pure-digit token after "bip" is the biped instance index.
                if (sawBip && !droppedBipIndex &&
                    std::ranges::all_of(tok, [](char ch)
                                        { return ch >= '0' && ch <= '9'; }))
                {
                    droppedBipIndex = true;
                    continue;
                }

                key += tok;
            }

            return ClassifyKey(key, side, outSpineNumber);
        }
    } // namespace

    HumanoidBoneMap::HumanoidBoneMap()
    {
        m_RoleToBone.fill(kUnassigned);
    }

    HumanoidBone HumanoidBoneMap::ClassifyBoneName(std::string_view boneName)
    {
        int spineNumber = 0;
        return ClassifyImpl(boneName, spineNumber);
    }

    HumanoidBoneMap HumanoidBoneMap::AutoDetect(const SkeletonData& skeleton)
    {
        HumanoidBoneMap map;

        struct SpineCandidate
        {
            int BoneIndex;
            int Number;
        };
        std::vector<SpineCandidate> spines;
        bool explicitChest = false;

        const sizet boneCount = skeleton.m_BoneNames.size();
        for (sizet i = 0; i < boneCount; ++i)
        {
            int spineNumber = 0;
            const HumanoidBone role = ClassifyImpl(skeleton.m_BoneNames[i], spineNumber);
            if (role == HumanoidBone::None)
                continue;

            if (role == HumanoidBone::Spine)
            {
                spines.push_back({ static_cast<int>(i), spineNumber });
                continue; // resolved together in the post-pass below
            }

            if (role == HumanoidBone::Chest)
                explicitChest = true;

            // First bone to claim a role keeps it (stable for duplicate-named rigs).
            if (map.m_RoleToBone[RoleIndex(role)] == kUnassigned)
                map.m_RoleToBone[RoleIndex(role)] = static_cast<int>(i);
        }

        // Resolve the spine column: lowest-numbered spine -> Spine; absent an explicit
        // chest, the highest of >=2 spine bones -> Chest. Intermediate spine bones are
        // left unassigned (a name/identity fallback can still pick them up).
        if (!spines.empty())
        {
            std::ranges::sort(spines,
                              [](const SpineCandidate& a, const SpineCandidate& b)
                              {
                                  if (a.Number != b.Number)
                                      return a.Number < b.Number;
                                  return a.BoneIndex < b.BoneIndex;
                              });

            if (map.m_RoleToBone[RoleIndex(HumanoidBone::Spine)] == kUnassigned)
                map.m_RoleToBone[RoleIndex(HumanoidBone::Spine)] = spines.front().BoneIndex;

            if (!explicitChest && spines.size() >= 2 &&
                map.m_RoleToBone[RoleIndex(HumanoidBone::Chest)] == kUnassigned)
                map.m_RoleToBone[RoleIndex(HumanoidBone::Chest)] = spines.back().BoneIndex;
        }

        return map;
    }

    void HumanoidBoneMap::SetBone(HumanoidBone role, int boneIndex)
    {
        const auto r = static_cast<i32>(role);
        if (r < 0 || static_cast<sizet>(r) >= HumanoidBoneCount)
            return;
        m_RoleToBone[static_cast<sizet>(r)] = (boneIndex < 0) ? kUnassigned : boneIndex;
    }

    int HumanoidBoneMap::GetBone(HumanoidBone role) const
    {
        const auto r = static_cast<i32>(role);
        if (r < 0 || static_cast<sizet>(r) >= HumanoidBoneCount)
            return kUnassigned;
        return m_RoleToBone[static_cast<sizet>(r)];
    }

    HumanoidBone HumanoidBoneMap::GetRole(int boneIndex) const
    {
        if (boneIndex < 0)
            return HumanoidBone::None;
        for (sizet r = 0; r < HumanoidBoneCount; ++r)
        {
            if (m_RoleToBone[r] == boneIndex)
                return static_cast<HumanoidBone>(r);
        }
        return HumanoidBone::None;
    }

    bool HumanoidBoneMap::HasRole(HumanoidBone role) const
    {
        return GetBone(role) != kUnassigned;
    }

    sizet HumanoidBoneMap::GetAssignedRoleCount() const
    {
        return static_cast<sizet>(std::ranges::count_if(m_RoleToBone,
                                                        [](int b)
                                                        { return b != kUnassigned; }));
    }
} // namespace OloEngine::Animation
