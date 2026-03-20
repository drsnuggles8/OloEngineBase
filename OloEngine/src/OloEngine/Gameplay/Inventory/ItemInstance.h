#pragma once

#include "OloEngine/Gameplay/Inventory/Item.h"
#include "OloEngine/Core/UUID.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    struct ItemAffix
    {
        std::string Name;
        std::string Attribute;
        f32 Value = 0.0f;

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

        bool operator==(const ItemInstance& other) const
        {
            return static_cast<u64>(InstanceID) == static_cast<u64>(other.InstanceID) && ItemDefinitionID == other.ItemDefinitionID && StackCount == other.StackCount && Durability == other.Durability && MaxDurability == other.MaxDurability && Affixes == other.Affixes && CustomData == other.CustomData;
        }
    };

} // namespace OloEngine
