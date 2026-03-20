#pragma once

#include "OloEngine/Gameplay/Inventory/Item.h"

#include <string>
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
        static std::vector<const ItemDefinition*> GetByTag(const std::string& tag);
        static std::vector<const ItemDefinition*> GetAll();
        static void Clear();

      private:
        static std::unordered_map<std::string, ItemDefinition>& GetItems();
    };

} // namespace OloEngine
