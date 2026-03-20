#pragma once

#include "OloEngine/Core/Base.h"

#include <string>
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
    inline const char* ItemCategoryToString(ItemCategory category)
    {
        switch (category)
        {
            case ItemCategory::Weapon:
                return "Weapon";
            case ItemCategory::Armor:
                return "Armor";
            case ItemCategory::Consumable:
                return "Consumable";
            case ItemCategory::QuestItem:
                return "QuestItem";
            case ItemCategory::Material:
                return "Material";
            case ItemCategory::Currency:
                return "Currency";
            case ItemCategory::Misc:
                return "Misc";
        }
        return "Misc";
    }

    inline ItemCategory ItemCategoryFromString(const std::string& str)
    {
        if (str == "Weapon")
            return ItemCategory::Weapon;
        if (str == "Armor")
            return ItemCategory::Armor;
        if (str == "Consumable")
            return ItemCategory::Consumable;
        if (str == "QuestItem")
            return ItemCategory::QuestItem;
        if (str == "Material")
            return ItemCategory::Material;
        if (str == "Currency")
            return ItemCategory::Currency;
        if (str != "Misc")
        {
            OLO_CORE_WARN("[Item] Unrecognized ItemCategory '{}' \u2014 defaulting to Misc", str);
        }
        return ItemCategory::Misc;
    }

    inline const char* ItemRarityToString(ItemRarity rarity)
    {
        switch (rarity)
        {
            case ItemRarity::Common:
                return "Common";
            case ItemRarity::Uncommon:
                return "Uncommon";
            case ItemRarity::Rare:
                return "Rare";
            case ItemRarity::Epic:
                return "Epic";
            case ItemRarity::Legendary:
                return "Legendary";
        }
        return "Common";
    }

    inline ItemRarity ItemRarityFromString(const std::string& str)
    {
        if (str == "Uncommon")
            return ItemRarity::Uncommon;
        if (str == "Rare")
            return ItemRarity::Rare;
        if (str == "Epic")
            return ItemRarity::Epic;
        if (str == "Legendary")
            return ItemRarity::Legendary;
        if (str != "Common")
        {
            OLO_CORE_WARN("[Item] Unrecognized ItemRarity '{}' \u2014 defaulting to Common", str);
        }
        return ItemRarity::Common;
    }

} // namespace OloEngine
