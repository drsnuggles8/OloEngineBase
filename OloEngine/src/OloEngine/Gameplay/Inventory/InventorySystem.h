#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Gameplay/Inventory/EquipmentSlots.h"
#include "OloEngine/Gameplay/Inventory/ItemInstance.h"

#include <string>

namespace OloEngine
{
    class Scene;
    class Entity;

    // Entity-aware orchestration over the pure Inventory / EquipmentSlots
    // value types. Like QuestSystem, this is the layer that knows the owning
    // entity, so it is where inventory mutations become the entity-stamped
    // InventoryEvents.h payloads published on Scene::GetGameplayEvents().
    //
    // Gameplay code, scripting glue, and UI should call these instead of
    // mutating InventoryComponent directly when they want the change observed.
    class InventorySystem
    {
      public:
        // Per-frame: pickup despawn timers + auto-pickup proximity. Auto-pickup
        // routes through AddItem, so a vacuumed item publishes ItemAdded.
        static void OnUpdate(Scene* scene, f32 dt);

        // Add to the entity's player inventory (publishes ItemAdded on success).
        static bool AddItem(Scene* scene, Entity entity, const ItemInstance& item);

        // Remove a specific instance by ID (publishes ItemRemoved on success).
        static bool RemoveItem(Scene* scene, Entity entity, const UUID& instanceId, i32 count = 1);

        // Remove `count` of a definition from the inventory, lowest slots first
        // (publishes ItemRemoved on success).
        static bool RemoveItemByDefinition(Scene* scene, Entity entity, const std::string& definitionId, i32 count = 1);

        // Equip `item` into `slot`, drawing it out of the entity's own inventory
        // (publishes ItemEquipped on success).
        static bool EquipItem(Scene* scene, Entity entity, EquipmentSlots::Slot slot, const ItemInstance& item);

        // Unequip `slot` back into the entity's inventory (publishes
        // ItemUnequipped on success).
        static bool UnequipItem(Scene* scene, Entity entity, EquipmentSlots::Slot slot);

        // -------------------------- Vendor / shop trading --------------------
        //
        // A "shop" is any entity carrying an ItemContainerComponent with
        // IsShop == true (its Contents inventory is the stock). A "buyer" /
        // "seller" is an entity with an InventoryComponent (its Currency funds
        // the trade, its PlayerInventory holds the goods). Shops have unlimited
        // gold — only the player side tracks currency.
        //
        // Both calls are all-or-nothing: every precondition (shop flag, stock /
        // ownership, affordability, destination room) is validated before any
        // currency or item moves, so a rejected trade leaves both sides exactly
        // as they were. Pricing comes from ItemDatabase (ItemDefinition's
        // BuyPrice / SellPrice); see GetSellPrice for the buyback fallback.

        // Buy `quantity` of `definitionId` from `shop` into `buyer`. Fails if
        // the shop flag is unset, stock is insufficient, the buyer can't afford
        // it, or the buyer's inventory has no room. On success debits the
        // buyer's Currency and publishes ItemBoughtEvent.
        static bool BuyItem(Scene* scene, Entity buyer, Entity shop, const std::string& definitionId, i32 quantity = 1);

        // Sell `quantity` of `definitionId` from `seller` to `shop`. Fails if
        // the shop flag is unset, the seller doesn't own that many, or the shop
        // has no room. On success credits the seller's Currency and publishes
        // ItemSoldEvent.
        static bool SellItem(Scene* scene, Entity seller, Entity shop, const std::string& definitionId, i32 quantity = 1);

        // Total price (currency) to buy `quantity` of `definitionId`, or 0 if
        // the definition is unknown / not for sale. UI/preview helper — the
        // single source of truth for the buy-side number BuyItem charges.
        [[nodiscard]] static i32 GetBuyPrice(const std::string& definitionId, i32 quantity = 1);

        // Total price (currency) a shop pays to buy `quantity` of `definitionId`
        // back, or 0 if unknown. Uses ItemDefinition::SellPrice when set,
        // otherwise falls back to half the BuyPrice (integer division truncates,
        // so a BuyPrice of 1 sells for 0). Single source of truth for the
        // sell-side number SellItem credits.
        [[nodiscard]] static i32 GetSellPrice(const std::string& definitionId, i32 quantity = 1);
    };

} // namespace OloEngine
