#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/TransparentStringHash.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbility.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    /**
     * @brief Per-attribute row of a character class: the authored base value,
     * the automatic per-level growth, and the attribute-point spend rule.
     *
     * GrowthPerLevel feeds the "Progression.LevelGrowth" AttributeSet modifier
     * source: at level L the accumulated growth is GrowthPerLevel * (L - 1).
     * ValuePerPoint > 0 marks the attribute spendable via attribute points
     * (the "Progression.AllocatedPoints" source, ValuePerPoint per point).
     */
    struct ClassAttributeSpec
    {
        std::string Attribute;
        f32 BaseValue = 0.0f;
        f32 GrowthPerLevel = 0.0f;
        f32 ValuePerPoint = 0.0f;

        auto operator==(const ClassAttributeSpec&) const -> bool = default;
    };

    /**
     * @brief One character class / archetype definition.
     *
     * Initializing an entity from a class (ProgressionSystem::InitializeFromClass)
     * defines the attribute set, grants the starting abilities and tags, and
     * seeds the experience curve — replacing the hard-coded
     * AbilityComponent::InitializeDefaultRPGAttributes path.
     */
    struct CharacterClassDefinition
    {
        std::string ClassID; ///< Stable string id, e.g. "warrior"
        std::string DisplayName;
        std::string Description;

        std::vector<ClassAttributeSpec> Attributes;
        std::vector<GameplayAbilityDef> StartingAbilities;
        std::vector<std::string> StartingTags; ///< e.g. "State.Alive", "Class.Warrior"

        std::vector<AssetHandle> SkillTrees; ///< Associated SkillTreeDatabase assets
        AssetHandle ExperienceCurve = 0;     ///< Default curve (0 = engine default)

        i32 AttributePointsPerLevel = 5;
        i32 SkillPointsPerLevel = 1;
        i32 LevelCap = 0; ///< 0 = the experience curve's MaxLevel

        auto operator==(const CharacterClassDefinition&) const -> bool = default;
    };

    /**
     * @brief Character class database asset (`.olocharclass`).
     *
     * A set of CharacterClassDefinitions keyed by ClassID. Referenced by
     * ProgressionComponent::ClassDatabaseHandle + ClassID. Loaded via the
     * AssetManager (serializer: CharacterClassDatabaseSerializer), which
     * rejects the asset when Validate() fails (duplicate/empty class ids).
     */
    class CharacterClassDatabase : public Asset
    {
      public:
        CharacterClassDatabase() = default;

        static AssetType GetStaticType()
        {
            return AssetType::CharacterClassDatabase;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        /// O(1) class lookup by id; nullptr when absent. Requires RebuildIndex()
        /// after any mutation of m_Classes (the serializer does this on load).
        [[nodiscard]] const CharacterClassDefinition* FindClass(std::string_view classId) const;

        /// Rebuild the ClassID -> index map from m_Classes.
        void RebuildIndex();

        /// Structural validation: non-empty unique ClassIDs, non-negative point
        /// grants, and non-empty attribute names. On failure returns false and
        /// (when outError is non-null) a human-readable reason.
        [[nodiscard]] bool Validate(std::string* outError = nullptr) const;

        std::vector<CharacterClassDefinition> m_Classes;

      private:
        std::unordered_map<std::string, sizet, StringHash, StringEqual> m_ClassIndex;
    };
} // namespace OloEngine
