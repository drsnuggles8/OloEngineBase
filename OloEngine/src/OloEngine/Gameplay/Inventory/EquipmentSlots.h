#pragma once

#include "OloEngine/Gameplay/Inventory/ItemInstance.h"
#include "OloEngine/Gameplay/Inventory/Inventory.h"

#include <array>
#include <optional>
#include <utility>
#include <vector>

namespace OloEngine
{
    class EquipmentSlots
    {
      public:
        enum class Slot : u8
        {
            Head,
            Chest,
            Legs,
            Feet,
            Hands,
            Shoulders,
            Back,
            MainHand,
            OffHand,
            Ring1,
            Ring2,
            Necklace,
            Trinket1,
            Trinket2,
            Count
        };

        bool Equip(Slot slot, const ItemInstance& item, Inventory& sourceInventory);
        bool DirectEquip(Slot slot, const ItemInstance& item);
        bool Unequip(Slot slot, Inventory& targetInventory);
        [[nodiscard]] const ItemInstance* GetEquipped(Slot slot) const;
        [[nodiscard]] bool IsSlotEmpty(Slot slot) const;

        [[nodiscard]] std::vector<std::pair<std::string, f32>> GetAllAttributeModifiers() const;

        // Direct access for serialization
        [[nodiscard]] const auto& GetAllSlots() const
        {
            return m_Equipped;
        }

        static constexpr i32 SlotCount = static_cast<i32>(Slot::Count);

        static const char* SlotToString(Slot slot);
        static Slot SlotFromString(const std::string& str);

        auto operator==(const EquipmentSlots&) const -> bool = default;

      private:
        std::array<std::optional<ItemInstance>, static_cast<size_t>(Slot::Count)> m_Equipped;
    };

} // namespace OloEngine
