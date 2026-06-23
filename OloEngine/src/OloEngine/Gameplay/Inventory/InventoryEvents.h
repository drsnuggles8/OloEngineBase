#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"

#include <string>

namespace OloEngine
{
    // Simple event data structs for inventory operations.
    // These are POD-style notification payloads, not Event-derived classes.

    struct ItemAddedEvent
    {
        UUID EntityID;
        UUID ItemInstanceID;
        std::string ItemDefinitionID;
        i32 SlotIndex = -1;
    };

    struct ItemRemovedEvent
    {
        UUID EntityID;
        UUID ItemInstanceID;
        std::string ItemDefinitionID;
        i32 SlotIndex = -1;
    };

    struct ItemEquippedEvent
    {
        UUID EntityID;
        UUID ItemInstanceID;
        std::string SlotName;
    };

    struct ItemUnequippedEvent
    {
        UUID EntityID;
        UUID ItemInstanceID;
        std::string SlotName;
    };

    // Published when a buyer purchases items from a shop container. TotalPrice
    // is the currency the buyer paid (unit BuyPrice × Quantity).
    struct ItemBoughtEvent
    {
        UUID BuyerEntityID;
        UUID ShopEntityID;
        std::string ItemDefinitionID;
        i32 Quantity = 0;
        i32 TotalPrice = 0;
    };

    // Published when a seller offloads items to a shop container. TotalPrice is
    // the currency the seller received (unit SellPrice × Quantity).
    struct ItemSoldEvent
    {
        UUID SellerEntityID;
        UUID ShopEntityID;
        std::string ItemDefinitionID;
        i32 Quantity = 0;
        i32 TotalPrice = 0;
    };

} // namespace OloEngine
