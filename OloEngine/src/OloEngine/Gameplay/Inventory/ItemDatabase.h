#pragma once

#include "OloEngine/Core/TransparentStringHash.h"
#include "OloEngine/Gameplay/Inventory/Item.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class ItemDatabase
    {
      public:
        static void LoadFromDirectory(const std::string& path);
        static void Register(const ItemDefinition& definition);
        static const ItemDefinition* Get(const std::string& itemId);
        static std::vector<const ItemDefinition*> GetByCategory(ItemCategory category);
        static std::vector<const ItemDefinition*> GetByTag(std::string_view tag);
        static std::vector<const ItemDefinition*> GetAll();
        static void Clear();

      private:
        [[nodiscard("items map must be used")]] static std::unordered_map<std::string, ItemDefinition, StringHash, StringEqual>& GetItems();
    };

} // namespace OloEngine
