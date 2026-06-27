#pragma once

#include "OloEngine/Gameplay/Inventory/ItemInstance.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
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
        bool RemoveItemByDefinition(std::string_view definitionId, i32 count = 1);
        bool MoveItem(i32 fromSlot, i32 toSlot);
        bool SwapItems(i32 slotA, i32 slotB);
        bool TransferItem(i32 fromSlot, Inventory& targetInventory);

        // Queries
        [[nodiscard("item pointer must be used")]] const ItemInstance* GetItemAtSlot(i32 slot) const;
        [[nodiscard("item pointer must be used")]] ItemInstance* GetMutableItemAtSlot(i32 slot);
        [[nodiscard("found slot index must be used")]] i32 FindItem(std::string_view definitionId) const;
        [[nodiscard("item count must be used")]] i32 CountItem(std::string_view definitionId) const;
        [[nodiscard("presence check must be used")]] bool HasItem(std::string_view definitionId, i32 count = 1) const;
        [[nodiscard("capacity must be used")]] i32 GetCapacity() const
        {
            return m_Capacity;
        }
        [[nodiscard("used-slot count must be used")]] i32 GetUsedSlots() const;
        [[nodiscard("total weight must be used")]] f32 GetTotalWeight() const;

        // Sorting
        void SortByCategory();
        void SortByRarity();
        void SortByName();

        // Direct access for serialization
        [[nodiscard("slot list must be used")]] const std::vector<std::optional<ItemInstance>>& GetSlots() const
        {
            return m_Slots;
        }
        void SetCapacity(i32 capacity);

        f32 MaxWeight = 0.0f;

        auto operator==(const Inventory&) const -> bool = default;

      private:
        std::vector<std::optional<ItemInstance>> m_Slots;
        i32 m_Capacity;
    };

} // namespace OloEngine
