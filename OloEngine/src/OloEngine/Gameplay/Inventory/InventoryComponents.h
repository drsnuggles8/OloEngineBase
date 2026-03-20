#pragma once

#include "OloEngine/Gameplay/Inventory/Inventory.h"
#include "OloEngine/Gameplay/Inventory/EquipmentSlots.h"
#include "OloEngine/Gameplay/Inventory/ItemInstance.h"

#include <string>

namespace OloEngine
{
    struct InventoryComponent
    {
        Inventory PlayerInventory{ 40 };
        EquipmentSlots Equipment;
        i32 Currency = 0;

        auto operator==(const InventoryComponent&) const -> bool = default;
    };

    struct ItemPickupComponent
    {
        ItemInstance Item;
        f32 PickupRadius = 2.0f;
        bool AutoPickup = false;
        f32 DespawnTimer = -1.0f;

        auto operator==(const ItemPickupComponent&) const -> bool = default;
    };

    struct ItemContainerComponent
    {
        Inventory Contents{ 20 };
        bool IsShop = false;
        std::string LootTableID;
        bool HasBeenLooted = false;

        auto operator==(const ItemContainerComponent&) const -> bool = default;
    };

} // namespace OloEngine
