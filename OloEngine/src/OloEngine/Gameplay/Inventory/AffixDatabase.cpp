#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Inventory/AffixDatabase.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine
{
    std::unordered_map<std::string, AffixDefinition, StringHash, StringEqual>& AffixDatabase::GetDefinitions()
    {
        static std::unordered_map<std::string, AffixDefinition, StringHash, StringEqual> s_Definitions;
        return s_Definitions;
    }

    std::unordered_map<std::string, AffixPool, StringHash, StringEqual>& AffixDatabase::GetPools()
    {
        static std::unordered_map<std::string, AffixPool, StringHash, StringEqual> s_Pools;
        return s_Pools;
    }

    void AffixDatabase::Register(const AffixDefinition& definition)
    {
        auto& defs = GetDefinitions();
        if (defs.contains(definition.AffixID))
        {
            OLO_CORE_WARN("[AffixDatabase] Duplicate affix ID '{}' — registration ignored", definition.AffixID);
            return;
        }
        defs[definition.AffixID] = definition;
    }

    void AffixDatabase::RegisterPool(const AffixPool& pool)
    {
        auto& pools = GetPools();
        if (pools.contains(pool.PoolID))
        {
            OLO_CORE_WARN("[AffixDatabase] Duplicate pool ID '{}' — registration ignored", pool.PoolID);
            return;
        }
        pools[pool.PoolID] = pool;
    }

    const AffixDefinition* AffixDatabase::Get(const std::string& affixId)
    {
        auto& defs = GetDefinitions();
        if (auto it = defs.find(affixId); it != defs.end())
        {
            return &it->second;
        }
        return nullptr;
    }

    const AffixPool* AffixDatabase::GetPool(const std::string& poolId)
    {
        auto& pools = GetPools();
        if (auto it = pools.find(poolId); it != pools.end())
        {
            return &it->second;
        }
        return nullptr;
    }

    std::vector<const AffixDefinition*> AffixDatabase::GetAll()
    {
        std::vector<const AffixDefinition*> result;
        for (auto const& [id, def] : GetDefinitions())
        {
            result.push_back(&def);
        }
        return result;
    }

    void AffixDatabase::Clear()
    {
        GetDefinitions().clear();
        GetPools().clear();
    }

} // namespace OloEngine
