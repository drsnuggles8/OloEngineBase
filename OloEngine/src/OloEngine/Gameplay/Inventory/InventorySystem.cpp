#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Inventory/InventorySystem.h"
#include "OloEngine/Gameplay/Inventory/InventoryComponents.h"
#include "OloEngine/Gameplay/Inventory/InventoryEvents.h"
#include "OloEngine/Gameplay/Inventory/Item.h"
#include "OloEngine/Gameplay/Inventory/ItemDatabase.h"
#include "OloEngine/Gameplay/GameplayEventBus.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace OloEngine
{
    namespace
    {
        InventoryComponent* ResolveInventory(const Scene* scene, Entity entity)
        {
            if (!scene || !entity || !entity.HasComponent<InventoryComponent>())
            {
                return nullptr;
            }
            return &entity.GetComponent<InventoryComponent>();
        }

        ItemContainerComponent* ResolveContainer(const Scene* scene, Entity entity)
        {
            if (!scene || !entity || !entity.HasComponent<ItemContainerComponent>())
            {
                return nullptr;
            }
            return &entity.GetComponent<ItemContainerComponent>();
        }

        // When an item carries no explicit SellPrice, shops buy it back at this
        // fraction of its BuyPrice. Integer division truncates toward zero, so a
        // BuyPrice of 1 yields a sell price of 0 — a deliberate, documented
        // rounding choice (currency is integral; no float ever enters trading).
        constexpr i32 kBuybackNumerator = 1;
        constexpr i32 kBuybackDenominator = 2;

        [[nodiscard("unit buy price must be used")]] i32 UnitBuyPrice(const ItemDefinition& def)
        {
            return def.BuyPrice;
        }

        [[nodiscard("unit sell price must be used")]] i32 UnitSellPrice(const ItemDefinition& def)
        {
            if (def.SellPrice > 0)
            {
                return def.SellPrice;
            }
            if (def.BuyPrice > 0)
            {
                return (def.BuyPrice * kBuybackNumerator) / kBuybackDenominator;
            }
            return 0;
        }

        // Saturating total = unitPrice * quantity, clamped to i32 range. Used by
        // the GetBuyPrice / GetSellPrice UI helpers; the trade paths compute in
        // i64 and reject overflow rather than clamp.
        [[nodiscard("computed total must be used")]] i32 SaturatingTotal(i32 unitPrice, i32 quantity)
        {
            if (unitPrice <= 0 || quantity <= 0)
            {
                return 0;
            }
            const i64 total = static_cast<i64>(unitPrice) * static_cast<i64>(quantity);
            return total > static_cast<i64>(std::numeric_limits<i32>::max()) ? std::numeric_limits<i32>::max() : static_cast<i32>(total);
        }

        // Move `quantity` of `definitionId` from `source` into `dest`,
        // all-or-nothing. A whole-stack move carries the original ItemInstance
        // (ID, affixes, durability, custom data) across intact; a partial pull
        // from a larger stack mints a fresh InstanceID for the moved portion so
        // the leftover in `source` keeps the original ID (two live instances must
        // never share an ID). The adds run against a copy of `dest`, so a full
        // destination rolls the whole operation back: returns false with both
        // inventories untouched. Returns true only after `dest` is committed and
        // `source` debited.
        bool TransferByDefinition(Inventory& source, Inventory& dest, const std::string& definitionId, i32 quantity)
        {
            if (quantity <= 0)
            {
                return false;
            }

            if (!ItemDatabase::Get(definitionId))
            {
                return false;
            }

            if (!source.HasItem(definitionId, quantity))
            {
                return false;
            }

            // Plan the pull lowest-slot-first — the same order
            // RemoveItemByDefinition uses, so the commit below removes exactly
            // what we planned to add.
            struct Pull
            {
                ItemInstance Snapshot;
                i32 Take;
            };
            std::vector<Pull> pulls;
            i32 remaining = quantity;
            const auto& slots = source.GetSlots();
            const sizet slotCount = slots.size();
            for (sizet i = 0; i < slotCount; ++i)
            {
                if (remaining <= 0)
                {
                    break;
                }
                if (slots[i].has_value() && slots[i]->ItemDefinitionID == definitionId)
                {
                    const i32 take = std::min(slots[i]->StackCount, remaining);
                    pulls.emplace_back(*slots[i], take);
                    remaining -= take;
                }
            }
            // HasItem guaranteed sufficient stock, so remaining == 0 here.

            // Validate the adds against a copy so a full destination can't leave
            // a partially-applied transfer behind.
            Inventory destCopy = dest;
            for (const Pull& pull : pulls)
            {
                ItemInstance moved = pull.Snapshot;
                moved.StackCount = pull.Take;
                if (pull.Take < pull.Snapshot.StackCount)
                {
                    moved.InstanceID = UUID();
                }
                if (!destCopy.AddItem(moved))
                {
                    return false; // destination can't hold it — nothing committed
                }
            }

            // Commit.
            if (!source.RemoveItemByDefinition(definitionId, quantity))
            {
                return false; // defensive — HasItem already verified availability
            }
            dest = std::move(destCopy);
            return true;
        }

        // Best-effort slot lookup for an item instance after an add/before a
        // remove. Stacked items merge into an existing slot and drop the new
        // instance's ID, so this can legitimately return -1.
        [[nodiscard("slot index must be used")]] i32 FindSlotOfInstance(const Inventory& inventory, const UUID& instanceId)
        {
            const auto& slots = inventory.GetSlots();
            const sizet slotCount = slots.size();
            for (sizet i = 0; i < slotCount; ++i)
            {
                if (slots[i].has_value() && slots[i]->InstanceID == instanceId)
                {
                    return static_cast<i32>(i);
                }
            }
            return -1;
        }
    } // namespace

    void InventorySystem::OnUpdate(Scene* scene, f32 dt)
    {
        OLO_PROFILE_FUNCTION();

        std::unordered_set<entt::entity> entitiesToDestroy;

        // Process pickup despawn timers
        auto pickupView = scene->GetAllEntitiesWith<ItemPickupComponent, TransformComponent>();
        for (auto e : pickupView)
        {
            Entity entity = { e, scene };
            auto& pickup = entity.GetComponent<ItemPickupComponent>();

            if (pickup.DespawnTimer > 0.0f)
            {
                pickup.DespawnTimer -= dt;
                if (pickup.DespawnTimer <= 0.0f)
                {
                    entitiesToDestroy.insert(e);
                    continue;
                }
            }
        }

        // Process auto-pickup: check proximity between pickups and entities with inventories.
        // AddItem happens inline (preserving the original claim semantics: a
        // pickup is consumed only if it actually fits), but the resulting
        // ItemAdded events are collected and published *after* the view walk so
        // a subscriber can't run scene-mutating code mid-iteration.
        std::vector<ItemAddedEvent> added;
        auto inventoryView = scene->GetAllEntitiesWith<InventoryComponent, TransformComponent>();
        auto pickupView2 = scene->GetAllEntitiesWith<ItemPickupComponent, TransformComponent>();
        for (auto invEntity : inventoryView)
        {
            Entity invEnt = { invEntity, scene };
            const auto& invTransform = invEnt.GetComponent<TransformComponent>();
            auto& invComp = invEnt.GetComponent<InventoryComponent>();

            for (auto pickupEntity : pickupView2)
            {
                // Skip pickups already claimed this frame
                if (entitiesToDestroy.contains(pickupEntity))
                {
                    continue;
                }

                Entity pickupEnt = { pickupEntity, scene };
                if (!pickupEnt)
                {
                    continue;
                }

                const auto& pickupComp = pickupEnt.GetComponent<ItemPickupComponent>();
                if (!pickupComp.AutoPickup)
                {
                    continue;
                }

                const auto& pickupTransform = pickupEnt.GetComponent<TransformComponent>();
                glm::vec3 diff = invTransform.Translation - pickupTransform.Translation;
                f32 distSq = glm::dot(diff, diff);
                f32 radiusSq = pickupComp.PickupRadius * pickupComp.PickupRadius;

                if (distSq <= radiusSq && invComp.PlayerInventory.AddItem(pickupComp.Item))
                {
                    const i32 slot = FindSlotOfInstance(invComp.PlayerInventory, pickupComp.Item.InstanceID);
                    added.emplace_back(invEnt.GetUUID(), pickupComp.Item.InstanceID, pickupComp.Item.ItemDefinitionID, slot);
                    entitiesToDestroy.insert(pickupEntity);
                }
            }
        }

        // Publish auto-pickup events now that both views are no longer iterating.
        const GameplayEventBus& bus = scene->GetGameplayEvents();
        for (auto const& event : added)
        {
            bus.Publish(event);
        }

        // Deferred destruction
        for (auto e : entitiesToDestroy)
        {
            Entity entity = { e, scene };
            if (entity)
            {
                scene->DestroyEntity(entity);
            }
        }
    }

    bool InventorySystem::AddItem(Scene* scene, Entity entity, const ItemInstance& item)
    {
        InventoryComponent* ic = ResolveInventory(scene, entity);
        if (!ic)
        {
            return false;
        }

        if (!ic->PlayerInventory.AddItem(item))
        {
            return false;
        }

        const i32 slot = FindSlotOfInstance(ic->PlayerInventory, item.InstanceID);
        scene->GetGameplayEvents().Publish(ItemAddedEvent{ entity.GetUUID(), item.InstanceID, item.ItemDefinitionID, slot });
        return true;
    }

    bool InventorySystem::RemoveItem(Scene* scene, Entity entity, const UUID& instanceId, i32 count)
    {
        InventoryComponent* ic = ResolveInventory(scene, entity);
        if (!ic)
        {
            return false;
        }

        // Capture identity before removal (the slot may be emptied).
        const i32 slot = FindSlotOfInstance(ic->PlayerInventory, instanceId);
        std::string definitionId;
        if (const ItemInstance* found = (slot >= 0) ? ic->PlayerInventory.GetItemAtSlot(slot) : nullptr; found)
        {
            definitionId = found->ItemDefinitionID;
        }

        if (!ic->PlayerInventory.RemoveItem(instanceId, count))
        {
            return false;
        }

        scene->GetGameplayEvents().Publish(ItemRemovedEvent{ entity.GetUUID(), instanceId, definitionId, slot });
        return true;
    }

    bool InventorySystem::RemoveItemByDefinition(Scene* scene, Entity entity, const std::string& definitionId, i32 count)
    {
        InventoryComponent* ic = ResolveInventory(scene, entity);
        if (!ic)
        {
            return false;
        }

        // Capture the first matching instance (best effort — the event reports a
        // representative instance/slot for a definition-keyed removal).
        const i32 slot = ic->PlayerInventory.FindItem(definitionId);
        UUID instanceId;
        if (const ItemInstance* found = (slot >= 0) ? ic->PlayerInventory.GetItemAtSlot(slot) : nullptr; found)
        {
            instanceId = found->InstanceID;
        }

        if (!ic->PlayerInventory.RemoveItemByDefinition(definitionId, count))
        {
            return false;
        }

        scene->GetGameplayEvents().Publish(ItemRemovedEvent{ entity.GetUUID(), instanceId, definitionId, slot });
        return true;
    }

    bool InventorySystem::EquipItem(Scene* scene, Entity entity, EquipmentSlots::Slot slot, const ItemInstance& item)
    {
        InventoryComponent* ic = ResolveInventory(scene, entity);
        if (!ic)
        {
            return false;
        }

        if (!ic->Equipment.Equip(slot, item, ic->PlayerInventory))
        {
            return false;
        }

        scene->GetGameplayEvents().Publish(ItemEquippedEvent{ entity.GetUUID(), item.InstanceID, EquipmentSlots::SlotToString(slot) });
        return true;
    }

    bool InventorySystem::UnequipItem(Scene* scene, Entity entity, EquipmentSlots::Slot slot)
    {
        InventoryComponent* ic = ResolveInventory(scene, entity);
        if (!ic)
        {
            return false;
        }

        // Capture the equipped instance before it moves back to the inventory.
        UUID instanceId;
        if (const ItemInstance* equipped = ic->Equipment.GetEquipped(slot); equipped)
        {
            instanceId = equipped->InstanceID;
        }

        if (!ic->Equipment.Unequip(slot, ic->PlayerInventory))
        {
            return false;
        }

        scene->GetGameplayEvents().Publish(ItemUnequippedEvent{ entity.GetUUID(), instanceId, EquipmentSlots::SlotToString(slot) });
        return true;
    }

    bool InventorySystem::BuyItem(Scene* scene, Entity buyer, Entity shop, const std::string& definitionId, i32 quantity)
    {
        OLO_PROFILE_FUNCTION();

        if (quantity <= 0)
        {
            return false;
        }

        InventoryComponent* buyerInv = ResolveInventory(scene, buyer);
        ItemContainerComponent* shopContainer = ResolveContainer(scene, shop);
        if (!buyerInv || !shopContainer || !shopContainer->IsShop)
        {
            return false;
        }

        const ItemDefinition* def = ItemDatabase::Get(definitionId);
        if (!def)
        {
            return false;
        }

        const i32 unitPrice = UnitBuyPrice(*def);
        if (unitPrice < 0)
        {
            return false; // mis-authored negative price would gift currency on buy
        }

        // Affordability in i64 so a huge quantity can't wrap the multiply.
        const i64 totalCost = static_cast<i64>(unitPrice) * static_cast<i64>(quantity);
        if (totalCost > static_cast<i64>(buyerInv->Currency))
        {
            return false; // can't afford
        }

        // Stock (shop has it) and room (buyer can hold it) are validated
        // atomically inside the transfer — currency is only touched on success.
        if (!TransferByDefinition(shopContainer->Contents, buyerInv->PlayerInventory, definitionId, quantity))
        {
            return false;
        }

        buyerInv->Currency -= static_cast<i32>(totalCost); // totalCost <= Currency, fits i32

        scene->GetGameplayEvents().Publish(ItemBoughtEvent{ buyer.GetUUID(), shop.GetUUID(), definitionId, quantity, static_cast<i32>(totalCost) });
        return true;
    }

    bool InventorySystem::SellItem(Scene* scene, Entity seller, Entity shop, const std::string& definitionId, i32 quantity)
    {
        OLO_PROFILE_FUNCTION();

        if (quantity <= 0)
        {
            return false;
        }

        InventoryComponent* sellerInv = ResolveInventory(scene, seller);
        ItemContainerComponent* shopContainer = ResolveContainer(scene, shop);
        if (!sellerInv || !shopContainer || !shopContainer->IsShop)
        {
            return false;
        }

        const ItemDefinition* def = ItemDatabase::Get(definitionId);
        if (!def)
        {
            return false;
        }

        const i32 unitPrice = UnitSellPrice(*def);
        if (unitPrice < 0)
        {
            return false; // mis-authored negative price would charge the seller
        }

        const i64 totalGain = static_cast<i64>(unitPrice) * static_cast<i64>(quantity);
        // Reject rather than silently wrap an over-rich seller's currency.
        if (static_cast<i64>(sellerInv->Currency) + totalGain > static_cast<i64>(std::numeric_limits<i32>::max()))
        {
            OLO_CORE_WARN("[InventorySystem] SellItem would overflow currency — sale rejected (def '{}', qty {})", definitionId, quantity);
            return false;
        }

        // Ownership (seller has it) and room (shop can hold it) are validated
        // atomically inside the transfer — currency is only touched on success.
        if (!TransferByDefinition(sellerInv->PlayerInventory, shopContainer->Contents, definitionId, quantity))
        {
            return false;
        }

        sellerInv->Currency += static_cast<i32>(totalGain); // overflow guarded above

        scene->GetGameplayEvents().Publish(ItemSoldEvent{ seller.GetUUID(), shop.GetUUID(), definitionId, quantity, static_cast<i32>(totalGain) });
        return true;
    }

    i32 InventorySystem::GetBuyPrice(const std::string& definitionId, i32 quantity)
    {
        const ItemDefinition* def = ItemDatabase::Get(definitionId);
        if (!def)
        {
            return 0;
        }
        return SaturatingTotal(UnitBuyPrice(*def), quantity);
    }

    i32 InventorySystem::GetSellPrice(const std::string& definitionId, i32 quantity)
    {
        const ItemDefinition* def = ItemDatabase::Get(definitionId);
        if (!def)
        {
            return 0;
        }
        return SaturatingTotal(UnitSellPrice(*def), quantity);
    }

} // namespace OloEngine
