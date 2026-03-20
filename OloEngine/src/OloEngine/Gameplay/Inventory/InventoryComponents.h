#pragma once

#include "OloEngine/Gameplay/Inventory/Inventory.h"
#include "OloEngine/Gameplay/Inventory/EquipmentSlots.h"
#include "OloEngine/Gameplay/Inventory/ItemInstance.h"
#include "OloEngine/Gameplay/Inventory/LootTable.h"

namespace OloEngine
{
    struct InventoryComponent
    {
        Inventory PlayerInventory{ 40 };
        EquipmentSlots Equipment;
        i32 Currency = 0;
    };

    struct ItemPickupComponent
    {
        ItemInstance Item;
        f32 PickupRadius = 2.0f;
        bool AutoPickup = false;
        f32 DespawnTimer = -1.0f;
    };

    struct ItemContainerComponent
    {
        Inventory Contents{ 20 };
        bool IsShop = false;
        std::string LootTableID;
        bool HasBeenLooted = false;
    };

} // namespace OloEngine
