#pragma once

#include "OloEngine/Core/Base.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OloEngine
{
    enum class ItemCategory : u8
    {
        Weapon,
        Armor,
        Consumable,
        QuestItem,
        Material,
        Currency,
        Misc
    };

    enum class ItemRarity : u8
    {
        Common,
        Uncommon,
        Rare,
        Epic,
        Legendary
    };

    struct ItemDefinition
    {
        std::string ItemID;
        std::string DisplayName;
        std::string Description;
        std::string IconPath;
        std::string MeshAsset;

        ItemCategory Category = ItemCategory::Misc;
        ItemRarity Rarity = ItemRarity::Common;

        i32 MaxStackSize = 1;
        f32 Weight = 0.0f;
        i32 BuyPrice = 0;
        i32 SellPrice = 0;

        bool IsQuestItem = false;
        bool IsConsumable = false;

        // Attribute modifiers when equipped (attribute name, value)
        std::vector<std::pair<std::string, f32>> AttributeModifiers;

        // Tags for filtering/categorization (simple string tags)
        std::vector<std::string> Tags;
    };

    // String conversion helpers
    [[nodiscard("category string must be used")]] inline const char* ItemCategoryToString(ItemCategory category)
    {
        using enum ItemCategory;
        switch (category)
        {
            case Weapon:
                return "Weapon";
            case Armor:
                return "Armor";
            case Consumable:
                return "Consumable";
            case QuestItem:
                return "QuestItem";
            case Material:
                return "Material";
            case Currency:
                return "Currency";
            case Misc:
                return "Misc";
            default:
                break;
        }
        return "Misc";
    }

    inline ItemCategory ItemCategoryFromString(std::string_view str)
    {
        using enum ItemCategory;
        if (str == "Weapon")
            return Weapon;
        if (str == "Armor")
            return Armor;
        if (str == "Consumable")
            return Consumable;
        if (str == "QuestItem")
            return QuestItem;
        if (str == "Material")
            return Material;
        if (str == "Currency")
            return Currency;
        if (str != "Misc")
        {
            OLO_CORE_WARN("[Item] Unrecognized ItemCategory '{}' \u2014 defaulting to Misc", str);
        }
        return Misc;
    }

    [[nodiscard("rarity string must be used")]] inline const char* ItemRarityToString(ItemRarity rarity)
    {
        using enum ItemRarity;
        switch (rarity)
        {
            case Common:
                return "Common";
            case Uncommon:
                return "Uncommon";
            case Rare:
                return "Rare";
            case Epic:
                return "Epic";
            case Legendary:
                return "Legendary";
            default:
                break;
        }
        return "Common";
    }

    inline ItemRarity ItemRarityFromString(std::string_view str)
    {
        using enum ItemRarity;
        if (str == "Uncommon")
            return Uncommon;
        if (str == "Rare")
            return Rare;
        if (str == "Epic")
            return Epic;
        if (str == "Legendary")
            return Legendary;
        if (str != "Common")
        {
            OLO_CORE_WARN("[Item] Unrecognized ItemRarity '{}' \u2014 defaulting to Common", str);
        }
        return Common;
    }

} // namespace OloEngine
