#pragma once

#include "OloEngine/Gameplay/Inventory/ItemInstance.h"
#include "OloEngine/Gameplay/Inventory/ItemDatabase.h"

#include <algorithm>
#include <optional>
#include <vector>

namespace OloEngine
{
    class Inventory
    {
      public:
        explicit Inventory(i32 capacity = 20);

        // Core operations
        bool AddItem(const ItemInstance& item);
        bool AddItemToSlot(i32 slotIndex, const ItemInstance& item);
        bool RemoveItem(const UUID& instanceId, i32 count = 1);
        bool RemoveItemByDefinition(const std::string& definitionId, i32 count = 1);
        bool MoveItem(i32 fromSlot, i32 toSlot);
        bool SwapItems(i32 slotA, i32 slotB);
        bool TransferItem(i32 fromSlot, Inventory& targetInventory);

        // Queries
        [[nodiscard]] const ItemInstance* GetItemAtSlot(i32 slot) const;
        [[nodiscard]] ItemInstance* GetMutableItemAtSlot(i32 slot);
        [[nodiscard]] i32 FindItem(const std::string& definitionId) const;
        [[nodiscard]] i32 CountItem(const std::string& definitionId) const;
        [[nodiscard]] bool HasItem(const std::string& definitionId, i32 count = 1) const;
        [[nodiscard]] i32 GetCapacity() const { return m_Capacity; }
        [[nodiscard]] i32 GetUsedSlots() const;
        [[nodiscard]] f32 GetTotalWeight() const;

        // Sorting
        void SortByCategory();
        void SortByRarity();
        void SortByName();

        // Direct access for serialization
        [[nodiscard]] const std::vector<std::optional<ItemInstance>>& GetSlots() const { return m_Slots; }
        void SetCapacity(i32 capacity);

        f32 MaxWeight = 0.0f;

      private:
        std::vector<std::optional<ItemInstance>> m_Slots;
        i32 m_Capacity;
    };

} // namespace OloEngine
