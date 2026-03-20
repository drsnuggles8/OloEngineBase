#pragma once

#include "OloEngine/Gameplay/Inventory/AffixDefinition.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class AffixDatabase
    {
      public:
        static void Register(const AffixDefinition& definition);
        static void RegisterPool(const AffixPool& pool);
        static const AffixDefinition* Get(const std::string& affixId);
        static const AffixPool* GetPool(const std::string& poolId);
        static std::vector<const AffixDefinition*> GetAll();
        static void Clear();

      private:
        static std::unordered_map<std::string, AffixDefinition>& GetDefinitions();
        static std::unordered_map<std::string, AffixPool>& GetPools();
    };

} // namespace OloEngine
