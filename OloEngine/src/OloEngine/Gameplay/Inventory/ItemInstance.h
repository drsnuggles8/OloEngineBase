#pragma once

#include "OloEngine/Gameplay/Inventory/AffixDefinition.h"
#include "OloEngine/Gameplay/Inventory/Item.h"
#include "OloEngine/Core/UUID.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    struct ItemAffix
    {
        std::string DefinitionID; // References AffixDefinition::AffixID (empty for legacy/manual affixes)
        AffixType Type = AffixType::Prefix;
        i32 Tier = 0;          // Which tier was rolled (0 = untiered/legacy)
        std::string Name;      // Display name (e.g., "Flaming", "of the Bear")
        std::string Attribute; // Stat key this modifies (e.g., "FireDamage")
        f32 Value = 0.0f;      // Rolled value within the tier's range

        auto operator==(const ItemAffix&) const -> bool = default;
    };

    struct ItemInstance
    {
        UUID InstanceID;
        std::string ItemDefinitionID;

        i32 StackCount = 1;
        f32 Durability = -1.0f;
        f32 MaxDurability = -1.0f;

        std::vector<ItemAffix> Affixes;
        std::unordered_map<std::string, std::string> CustomData;

        // Returns true when two instances can be merged into one stack
        // (same definition, no per-instance state differences)
        [[nodiscard]] bool IsStackCompatible(const ItemInstance& other) const
        {
            return ItemDefinitionID == other.ItemDefinitionID && Durability == other.Durability && MaxDurability == other.MaxDurability && Affixes == other.Affixes && CustomData == other.CustomData;
        }

        bool operator==(const ItemInstance& other) const
        {
            return static_cast<u64>(InstanceID) == static_cast<u64>(other.InstanceID) && ItemDefinitionID == other.ItemDefinitionID && StackCount == other.StackCount && Durability == other.Durability && MaxDurability == other.MaxDurability && Affixes == other.Affixes && CustomData == other.CustomData;
        }
    };

} // namespace OloEngine
