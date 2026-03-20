#pragma once

#include "OloEngine/Core/Base.h"

#include <string>
#include <vector>

namespace OloEngine
{
    enum class AffixType : u8
    {
        Prefix,
        Suffix
    };

    inline const char* AffixTypeToString(AffixType type)
    {
        switch (type)
        {
            case AffixType::Prefix:
                return "Prefix";
            case AffixType::Suffix:
                return "Suffix";
        }
        return "Prefix";
    }

    inline AffixType AffixTypeFromString(const std::string& str)
    {
        if (str == "Suffix")
            return AffixType::Suffix;
        return AffixType::Prefix;
    }

    // A single tier within an affix definition (e.g., tier 1 = "of Embers" +5-10 fire damage)
    struct AffixTier
    {
        i32 Tier = 1;
        std::string DisplayName; // e.g., "of Embers", "Flaming"
        f32 MinValue = 0.0f;
        f32 MaxValue = 0.0f;
        f32 MinItemLevel = 0.0f; // Minimum item level required for this tier to appear
    };

    // An authored affix template — one per distinct affix idea (e.g., "fire_damage", "increased_life")
    struct AffixDefinition
    {
        std::string AffixID;      // Unique identifier, e.g., "fire_damage"
        AffixType Type = AffixType::Prefix;
        std::string Attribute;    // The stat it modifies, e.g., "FireDamage"
        std::string Description;  // Tooltip text

        // Tiers ordered from lowest to highest (tier 1 = weakest)
        std::vector<AffixTier> Tiers;

        // Which item categories this affix can appear on
        std::vector<std::string> AllowedCategories; // Empty = all categories
    };

    // A named pool of affix definitions that loot table entries reference
    struct AffixPool
    {
        std::string PoolID; // e.g., "weapon_prefixes", "armor_suffixes"
        std::vector<std::string> AffixIDs; // References to AffixDefinition::AffixID
    };

} // namespace OloEngine
