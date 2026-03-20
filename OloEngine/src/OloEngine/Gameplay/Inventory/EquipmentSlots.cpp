#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Inventory/EquipmentSlots.h"
#include "OloEngine/Gameplay/Inventory/ItemDatabase.h"

namespace OloEngine
{
    bool EquipmentSlots::Equip(Slot slot, const ItemInstance& item, Inventory& sourceInventory)
    {
        auto slotIdx = static_cast<size_t>(slot);
        if (slotIdx >= static_cast<size_t>(Slot::Count))
        {
            return false;
        }

        // If slot is occupied, unequip first
        if (m_Equipped[slotIdx].has_value())
        {
            if (!Unequip(slot, sourceInventory))
            {
                return false;
            }
        }

        // Remove from source inventory; fail if not found (gameplay path)
        if (!sourceInventory.RemoveItem(item.InstanceID, item.StackCount))
        {
            return false;
        }

        m_Equipped[slotIdx] = item;
        return true;
    }

    bool EquipmentSlots::DirectEquip(Slot slot, const ItemInstance& item)
    {
        auto slotIdx = static_cast<size_t>(slot);
        if (slotIdx >= static_cast<size_t>(Slot::Count))
        {
            return false;
        }

        m_Equipped[slotIdx] = item;
        return true;
    }

    bool EquipmentSlots::Unequip(Slot slot, Inventory& targetInventory)
    {
        auto slotIdx = static_cast<size_t>(slot);
        if (slotIdx >= static_cast<size_t>(Slot::Count))
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
        auto slotIdx = static_cast<size_t>(slot);
        if (slotIdx >= static_cast<size_t>(Slot::Count))
        {
            return nullptr;
        }
        return m_Equipped[slotIdx].has_value() ? &m_Equipped[slotIdx].value() : nullptr;
    }

    bool EquipmentSlots::IsSlotEmpty(Slot slot) const
    {
        auto slotIdx = static_cast<size_t>(slot);
        if (slotIdx >= static_cast<size_t>(Slot::Count))
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

            const ItemDefinition* def = ItemDatabase::Get(slot->ItemDefinitionID);
            if (def)
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
        switch (slot)
        {
            case Slot::Head:
                return "Head";
            case Slot::Chest:
                return "Chest";
            case Slot::Legs:
                return "Legs";
            case Slot::Feet:
                return "Feet";
            case Slot::Hands:
                return "Hands";
            case Slot::Shoulders:
                return "Shoulders";
            case Slot::Back:
                return "Back";
            case Slot::MainHand:
                return "MainHand";
            case Slot::OffHand:
                return "OffHand";
            case Slot::Ring1:
                return "Ring1";
            case Slot::Ring2:
                return "Ring2";
            case Slot::Necklace:
                return "Necklace";
            case Slot::Trinket1:
                return "Trinket1";
            case Slot::Trinket2:
                return "Trinket2";
            case Slot::Count:
                return "Count";
        }
        return "Unknown";
    }

    EquipmentSlots::Slot EquipmentSlots::SlotFromString(const std::string& str)
    {
        if (str == "Head")
            return Slot::Head;
        if (str == "Chest")
            return Slot::Chest;
        if (str == "Legs")
            return Slot::Legs;
        if (str == "Feet")
            return Slot::Feet;
        if (str == "Hands")
            return Slot::Hands;
        if (str == "Shoulders")
            return Slot::Shoulders;
        if (str == "Back")
            return Slot::Back;
        if (str == "MainHand")
            return Slot::MainHand;
        if (str == "OffHand")
            return Slot::OffHand;
        if (str == "Ring1")
            return Slot::Ring1;
        if (str == "Ring2")
            return Slot::Ring2;
        if (str == "Necklace")
            return Slot::Necklace;
        if (str == "Trinket1")
            return Slot::Trinket1;
        if (str == "Trinket2")
            return Slot::Trinket2;
        return Slot::Count; // Invalid
    }

} // namespace OloEngine
