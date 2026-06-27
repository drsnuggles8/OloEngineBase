#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Inventory/EquipmentSlots.h"
#include "OloEngine/Gameplay/Inventory/ItemDatabase.h"

namespace OloEngine
{
    bool EquipmentSlots::Equip(Slot slot, const ItemInstance& item, Inventory& sourceInventory)
    {
        auto slotIdx = static_cast<size_t>(std::to_underlying(slot));
        if (slotIdx >= static_cast<size_t>(std::to_underlying(Slot::Count)))
        {
            return false;
        }

        // Remove from source inventory first; fail if not found
        if (!sourceInventory.RemoveItem(item.InstanceID, item.StackCount))
        {
            return false;
        }

        // If slot is occupied, unequip first
        if (m_Equipped[slotIdx].has_value() && !Unequip(slot, sourceInventory))
        {
            // Rollback: re-add the removed item
            sourceInventory.AddItem(item);
            return false;
        }

        m_Equipped[slotIdx] = item;
        return true;
    }

    bool EquipmentSlots::DirectEquip(Slot slot, const ItemInstance& item)
    {
        auto slotIdx = static_cast<size_t>(std::to_underlying(slot));
        if (slotIdx >= static_cast<size_t>(std::to_underlying(Slot::Count)))
        {
            return false;
        }

        m_Equipped[slotIdx] = item;
        return true;
    }

    bool EquipmentSlots::Unequip(Slot slot, Inventory& targetInventory)
    {
        auto slotIdx = static_cast<size_t>(std::to_underlying(slot));
        if (slotIdx >= static_cast<size_t>(std::to_underlying(Slot::Count)))
        {
            return false;
        }

        if (!m_Equipped[slotIdx].has_value())
        {
            return false;
        }

        if (!targetInventory.AddItem(m_Equipped[slotIdx].value()))
        {
            return false; // Inventory full
        }

        m_Equipped[slotIdx].reset();
        return true;
    }

    const ItemInstance* EquipmentSlots::GetEquipped(Slot slot) const
    {
        auto slotIdx = static_cast<size_t>(std::to_underlying(slot));
        if (slotIdx >= static_cast<size_t>(std::to_underlying(Slot::Count)))
        {
            return nullptr;
        }
        return m_Equipped[slotIdx].has_value() ? &m_Equipped[slotIdx].value() : nullptr;
    }

    bool EquipmentSlots::IsSlotEmpty(Slot slot) const
    {
        auto slotIdx = static_cast<size_t>(std::to_underlying(slot));
        if (slotIdx >= static_cast<size_t>(std::to_underlying(Slot::Count)))
        {
            return true;
        }
        return !m_Equipped[slotIdx].has_value();
    }

    std::vector<std::pair<std::string, f32>> EquipmentSlots::GetAllAttributeModifiers() const
    {
        std::vector<std::pair<std::string, f32>> result;
        for (auto const& slot : m_Equipped)
        {
            if (!slot.has_value())
            {
                continue;
            }

            if (const ItemDefinition* def = ItemDatabase::Get(slot->ItemDefinitionID); def)
            {
                for (auto const& mod : def->AttributeModifiers)
                {
                    result.push_back(mod);
                }
            }

            // Also add affix modifiers
            for (auto const& affix : slot->Affixes)
            {
                result.emplace_back(affix.Attribute, affix.Value);
            }
        }
        return result;
    }

    const char* EquipmentSlots::SlotToString(Slot slot)
    {
        using enum Slot;
        switch (slot)
        {
            case Head:
                return "Head";
            case Chest:
                return "Chest";
            case Legs:
                return "Legs";
            case Feet:
                return "Feet";
            case Hands:
                return "Hands";
            case Shoulders:
                return "Shoulders";
            case Back:
                return "Back";
            case MainHand:
                return "MainHand";
            case OffHand:
                return "OffHand";
            case Ring1:
                return "Ring1";
            case Ring2:
                return "Ring2";
            case Necklace:
                return "Necklace";
            case Trinket1:
                return "Trinket1";
            case Trinket2:
                return "Trinket2";
            case Count:
                return "Count";
            default:
                break;
        }
        return "Unknown";
    }

    EquipmentSlots::Slot EquipmentSlots::SlotFromString(std::string_view str)
    {
        using enum Slot;
        if (str == "Head")
            return Head;
        if (str == "Chest")
            return Chest;
        if (str == "Legs")
            return Legs;
        if (str == "Feet")
            return Feet;
        if (str == "Hands")
            return Hands;
        if (str == "Shoulders")
            return Shoulders;
        if (str == "Back")
            return Back;
        if (str == "MainHand")
            return MainHand;
        if (str == "OffHand")
            return OffHand;
        if (str == "Ring1")
            return Ring1;
        if (str == "Ring2")
            return Ring2;
        if (str == "Necklace")
            return Necklace;
        if (str == "Trinket1")
            return Trinket1;
        if (str == "Trinket2")
            return Trinket2;
        return Count; // Invalid
    }

} // namespace OloEngine
