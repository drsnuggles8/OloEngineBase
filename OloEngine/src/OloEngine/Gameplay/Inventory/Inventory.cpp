#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Inventory/Inventory.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine
{
    Inventory::Inventory(i32 capacity)
        : m_Capacity(capacity)
    {
        m_Slots.resize(static_cast<size_t>(capacity));
    }

    bool Inventory::AddItem(const ItemInstance& item)
    {
        OLO_PROFILE_FUNCTION();

        const ItemDefinition* def = ItemDatabase::Get(item.ItemDefinitionID);
        i32 maxStack = def ? def->MaxStackSize : 1;

        // Check weight limit
        if (MaxWeight > 0.0f && def)
        {
            f32 addedWeight = def->Weight * static_cast<f32>(item.StackCount);
            if (GetTotalWeight() + addedWeight > MaxWeight)
            {
                return false;
            }
        }

        // Try to stack with existing items first
        if (maxStack > 1)
        {
            for (auto& slot : m_Slots)
            {
                if (slot.has_value() && slot->ItemDefinitionID == item.ItemDefinitionID)
                {
                    i32 canAdd = maxStack - slot->StackCount;
                    if (canAdd >= item.StackCount)
                    {
                        slot->StackCount += item.StackCount;
                        return true;
                    }
                }
            }
        }

        // Find empty slot
        for (i32 i = 0; i < m_Capacity; ++i)
        {
            if (!m_Slots[static_cast<size_t>(i)].has_value())
            {
                m_Slots[static_cast<size_t>(i)] = item;
                return true;
            }
        }

        return false; // No room
    }

    bool Inventory::AddItemToSlot(i32 slotIndex, const ItemInstance& item)
    {
        if (slotIndex < 0 || slotIndex >= m_Capacity)
        {
            return false;
        }

        auto& slot = m_Slots[static_cast<size_t>(slotIndex)];
        if (slot.has_value())
        {
            // Try stacking
            if (slot->ItemDefinitionID == item.ItemDefinitionID)
            {
                const ItemDefinition* def = ItemDatabase::Get(item.ItemDefinitionID);
                i32 maxStack = def ? def->MaxStackSize : 1;
                if (slot->StackCount + item.StackCount <= maxStack)
                {
                    slot->StackCount += item.StackCount;
                    return true;
                }
            }
            return false;
        }

        slot = item;
        return true;
    }

    bool Inventory::RemoveItem(const UUID& instanceId, i32 count)
    {
        for (auto& slot : m_Slots)
        {
            if (slot.has_value() && slot->InstanceID == instanceId)
            {
                if (slot->StackCount <= count)
                {
                    slot.reset();
                }
                else
                {
                    slot->StackCount -= count;
                }
                return true;
            }
        }
        return false;
    }

    bool Inventory::RemoveItemByDefinition(const std::string& definitionId, i32 count)
    {
        i32 remaining = count;
        for (auto& slot : m_Slots)
        {
            if (remaining <= 0)
            {
                break;
            }
            if (slot.has_value() && slot->ItemDefinitionID == definitionId)
            {
                if (slot->StackCount <= remaining)
                {
                    remaining -= slot->StackCount;
                    slot.reset();
                }
                else
                {
                    slot->StackCount -= remaining;
                    remaining = 0;
                }
            }
        }
        return remaining <= 0;
    }

    bool Inventory::MoveItem(i32 fromSlot, i32 toSlot)
    {
        if (fromSlot < 0 || fromSlot >= m_Capacity || toSlot < 0 || toSlot >= m_Capacity)
        {
            return false;
        }
        if (fromSlot == toSlot)
        {
            return true;
        }

        auto& from = m_Slots[static_cast<size_t>(fromSlot)];
        auto& to = m_Slots[static_cast<size_t>(toSlot)];

        if (!from.has_value())
        {
            return false;
        }

        if (!to.has_value())
        {
            to = std::move(from);
            from.reset();
            return true;
        }

        // Try stacking
        if (from->ItemDefinitionID == to->ItemDefinitionID)
        {
            const ItemDefinition* def = ItemDatabase::Get(from->ItemDefinitionID);
            i32 maxStack = def ? def->MaxStackSize : 1;
            if (to->StackCount + from->StackCount <= maxStack)
            {
                to->StackCount += from->StackCount;
                from.reset();
                return true;
            }
        }

        // Swap
        std::swap(from, to);
        return true;
    }

    bool Inventory::SwapItems(i32 slotA, i32 slotB)
    {
        if (slotA < 0 || slotA >= m_Capacity || slotB < 0 || slotB >= m_Capacity)
        {
            return false;
        }

        std::swap(m_Slots[static_cast<size_t>(slotA)], m_Slots[static_cast<size_t>(slotB)]);
        return true;
    }

    bool Inventory::TransferItem(i32 fromSlot, Inventory& targetInventory)
    {
        if (fromSlot < 0 || fromSlot >= m_Capacity)
        {
            return false;
        }

        auto& slot = m_Slots[static_cast<size_t>(fromSlot)];
        if (!slot.has_value())
        {
            return false;
        }

        if (targetInventory.AddItem(slot.value()))
        {
            slot.reset();
            return true;
        }

        return false;
    }

    const ItemInstance* Inventory::GetItemAtSlot(i32 slot) const
    {
        if (slot < 0 || slot >= m_Capacity)
        {
            return nullptr;
        }
        auto& s = m_Slots[static_cast<size_t>(slot)];
        return s.has_value() ? &s.value() : nullptr;
    }

    ItemInstance* Inventory::GetMutableItemAtSlot(i32 slot)
    {
        if (slot < 0 || slot >= m_Capacity)
        {
            return nullptr;
        }
        auto& s = m_Slots[static_cast<size_t>(slot)];
        return s.has_value() ? &s.value() : nullptr;
    }

    i32 Inventory::FindItem(const std::string& definitionId) const
    {
        for (i32 i = 0; i < m_Capacity; ++i)
        {
            auto const& slot = m_Slots[static_cast<size_t>(i)];
            if (slot.has_value() && slot->ItemDefinitionID == definitionId)
            {
                return i;
            }
        }
        return -1;
    }

    i32 Inventory::CountItem(const std::string& definitionId) const
    {
        i32 total = 0;
        for (auto const& slot : m_Slots)
        {
            if (slot.has_value() && slot->ItemDefinitionID == definitionId)
            {
                total += slot->StackCount;
            }
        }
        return total;
    }

    bool Inventory::HasItem(const std::string& definitionId, i32 count) const
    {
        return CountItem(definitionId) >= count;
    }

    i32 Inventory::GetUsedSlots() const
    {
        i32 count = 0;
        for (auto const& slot : m_Slots)
        {
            if (slot.has_value())
            {
                ++count;
            }
        }
        return count;
    }

    f32 Inventory::GetTotalWeight() const
    {
        f32 totalWeight = 0.0f;
        for (auto const& slot : m_Slots)
        {
            if (slot.has_value())
            {
                const ItemDefinition* def = ItemDatabase::Get(slot->ItemDefinitionID);
                if (def)
                {
                    totalWeight += def->Weight * static_cast<f32>(slot->StackCount);
                }
            }
        }
        return totalWeight;
    }

    void Inventory::SortByCategory()
    {
        std::sort(m_Slots.begin(), m_Slots.end(),
                  [](auto const& a, auto const& b)
                  {
                      if (!a.has_value()) return false;
                      if (!b.has_value()) return true;
                      const auto* defA = ItemDatabase::Get(a->ItemDefinitionID);
                      const auto* defB = ItemDatabase::Get(b->ItemDefinitionID);
                      if (!defA) return false;
                      if (!defB) return true;
                      return static_cast<u8>(defA->Category) < static_cast<u8>(defB->Category);
                  });
    }

    void Inventory::SortByRarity()
    {
        std::sort(m_Slots.begin(), m_Slots.end(),
                  [](auto const& a, auto const& b)
                  {
                      if (!a.has_value()) return false;
                      if (!b.has_value()) return true;
                      const auto* defA = ItemDatabase::Get(a->ItemDefinitionID);
                      const auto* defB = ItemDatabase::Get(b->ItemDefinitionID);
                      if (!defA) return false;
                      if (!defB) return true;
                      return static_cast<u8>(defA->Rarity) > static_cast<u8>(defB->Rarity);
                  });
    }

    void Inventory::SortByName()
    {
        std::sort(m_Slots.begin(), m_Slots.end(),
                  [](auto const& a, auto const& b)
                  {
                      if (!a.has_value()) return false;
                      if (!b.has_value()) return true;
                      const auto* defA = ItemDatabase::Get(a->ItemDefinitionID);
                      const auto* defB = ItemDatabase::Get(b->ItemDefinitionID);
                      if (!defA) return false;
                      if (!defB) return true;
                      return defA->DisplayName < defB->DisplayName;
                  });
    }

    void Inventory::SetCapacity(i32 capacity)
    {
        m_Capacity = capacity;
        m_Slots.resize(static_cast<size_t>(capacity));
    }

} // namespace OloEngine
