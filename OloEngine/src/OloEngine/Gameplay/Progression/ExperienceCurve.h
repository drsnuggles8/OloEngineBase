#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Base.h"

#include <vector>

namespace OloEngine
{
    /**
     * @brief XP-to-next-level curve asset (`.oloxpcurve`).
     *
     * Referenced by ProgressionComponent::ExperienceCurveHandle (directly, or
     * indirectly through CharacterClassDefinition::ExperienceCurve). A handle
     * of 0 means "engine default curve" (DefaultXPForLevelUp / kDefaultMaxLevel).
     * Pure value data serialized by ExperienceCurveSerializer; every float is
     * finite-validated and clamped to the documented range on load.
     *
     * Two modes:
     *  - Formula: XP required to advance FROM level L is
     *    round(m_BaseXP * pow(L, m_Exponent)).
     *  - Table:   m_Table[L - 1] is the XP required to advance FROM level L;
     *    levels past the end of the table reuse the last entry.
     */
    class ExperienceCurve : public Asset
    {
      public:
        enum class CurveMode : u8
        {
            Formula = 0,
            Table = 1
        };

        ExperienceCurve() = default;

        static AssetType GetStaticType()
        {
            return AssetType::ExperienceCurve;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        /// XP required to advance from `currentLevel` to `currentLevel + 1`.
        /// Always returns >= 1 so a malformed curve can never divide-by-zero or
        /// grant infinite levels from a single XP drip.
        [[nodiscard]] i32 GetXPForLevelUp(i32 currentLevel) const;

        /// The highest reachable level (>= 1).
        [[nodiscard]] i32 GetMaxLevel() const
        {
            return m_MaxLevel < 1 ? 1 : m_MaxLevel;
        }

        /// Engine-default curve used when no ExperienceCurve asset is assigned
        /// (handle 0): a linear 100 XP per current level.
        [[nodiscard]] static i32 DefaultXPForLevelUp(i32 currentLevel);

        /// Max level of the engine-default curve.
        static constexpr i32 kDefaultMaxLevel = 99;

        /// Clamp every member into its documented valid range (called by the
        /// serializer after a load and usable by tests / editor tooling).
        void Sanitize();

        CurveMode m_Mode = CurveMode::Formula;
        i32 m_MaxLevel = 50;      ///< [1, 1000]
        f32 m_BaseXP = 100.0f;    ///< Formula base [1, 1e7]
        f32 m_Exponent = 1.5f;    ///< Formula exponent [0, 10]
        std::vector<i32> m_Table; ///< Table mode entries, each >= 1
    };
} // namespace OloEngine
