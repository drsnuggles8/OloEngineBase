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
    };

} // namespace OloEngine
