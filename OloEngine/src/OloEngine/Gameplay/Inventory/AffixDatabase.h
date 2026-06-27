#pragma once

#include "OloEngine/Core/TransparentStringHash.h"
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
        [[nodiscard("affix definitions map must be used")]] static std::unordered_map<std::string, AffixDefinition, StringHash, StringEqual>& GetDefinitions();
        [[nodiscard("affix pools map must be used")]] static std::unordered_map<std::string, AffixPool, StringHash, StringEqual>& GetPools();
    };

} // namespace OloEngine
