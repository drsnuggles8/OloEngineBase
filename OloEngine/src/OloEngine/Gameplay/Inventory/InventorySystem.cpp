#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Inventory/InventorySystem.h"
#include "OloEngine/Gameplay/Inventory/InventoryComponents.h"
#include "OloEngine/Gameplay/Inventory/InventoryEvents.h"
#include "OloEngine/Gameplay/GameplayEventBus.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <glm/glm.hpp>
#include <unordered_set>
#include <utility>
#include <vector>

namespace OloEngine
{
    namespace
    {
        InventoryComponent* ResolveInventory(Scene* scene, Entity entity)
        {
            if (!scene || !entity || !entity.HasComponent<InventoryComponent>())
            {
                return nullptr;
            }
            return &entity.GetComponent<InventoryComponent>();
        }

        // Best-effort slot lookup for an item instance after an add/before a
        // remove. Stacked items merge into an existing slot and drop the new
        // instance's ID, so this can legitimately return -1.
        i32 FindSlotOfInstance(const Inventory& inventory, const UUID& instanceId)
        {
            const auto& slots = inventory.GetSlots();
            for (sizet i = 0; i < slots.size(); ++i)
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

                if (distSq <= radiusSq)
                {
                    if (invComp.PlayerInventory.AddItem(pickupComp.Item))
                    {
                        const i32 slot = FindSlotOfInstance(invComp.PlayerInventory, pickupComp.Item.InstanceID);
                        added.push_back(ItemAddedEvent{ invEnt.GetUUID(), pickupComp.Item.InstanceID, pickupComp.Item.ItemDefinitionID, slot });
                        entitiesToDestroy.insert(pickupEntity);
                    }
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

} // namespace OloEngine
