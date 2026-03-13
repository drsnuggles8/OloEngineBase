#include "OloEnginePCH.h"
#include "InMemoryWorldDatabase.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine
{
    bool InMemoryWorldDatabase::SavePlayerState(u32 accountID, const PlayerStatePacket& state)
    {
        std::lock_guard lock(m_Mutex);
        m_PlayerStates[accountID] = state;
        return true;
    }

    bool InMemoryWorldDatabase::LoadPlayerState(u32 accountID, PlayerStatePacket& outState)
    {
        std::lock_guard lock(m_Mutex);
        auto it = m_PlayerStates.find(accountID);
        if (it == m_PlayerStates.end())
        {
            return false;
        }
        outState = it->second;
        return true;
    }

    bool InMemoryWorldDatabase::DeletePlayerState(u32 accountID)
    {
        std::lock_guard lock(m_Mutex);
        return m_PlayerStates.erase(accountID) > 0;
    }

    bool InMemoryWorldDatabase::SaveEntityState(u64 uuid, ZoneID zoneID, const std::vector<u8>& data)
    {
        std::lock_guard lock(m_Mutex);
        m_EntityStates[uuid] = { zoneID, data };
        return true;
    }

    bool InMemoryWorldDatabase::LoadEntitiesForZone(ZoneID zoneID, std::vector<std::pair<u64, std::vector<u8>>>& outEntities)
    {
        std::lock_guard lock(m_Mutex);
        outEntities.clear();
        for (auto const& [uuid, record] : m_EntityStates)
        {
            if (record.Zone == zoneID)
            {
                outEntities.emplace_back(uuid, record.Data);
            }
        }
        return true;
    }

    bool InMemoryWorldDatabase::DeleteEntityState(u64 uuid)
    {
        std::lock_guard lock(m_Mutex);
        return m_EntityStates.erase(uuid) > 0;
    }

    bool InMemoryWorldDatabase::SetWorldState(const std::string& key, const std::string& value)
    {
        std::lock_guard lock(m_Mutex);
        m_WorldState[key] = value;
        return true;
    }

    bool InMemoryWorldDatabase::GetWorldState(const std::string& key, std::string& outValue)
    {
        std::lock_guard lock(m_Mutex);
        auto it = m_WorldState.find(key);
        if (it == m_WorldState.end())
        {
            return false;
        }
        outValue = it->second;
        return true;
    }

    bool InMemoryWorldDatabase::Initialize([[maybe_unused]] const std::string& connectionString)
    {
        m_Initialized = true;
        OLO_CORE_INFO("[InMemoryWorldDatabase] Initialized");
        return true;
    }

    void InMemoryWorldDatabase::Shutdown()
    {
        std::lock_guard lock(m_Mutex);
        m_PlayerStates.clear();
        m_EntityStates.clear();
        m_WorldState.clear();
        m_Initialized = false;
    }

    bool InMemoryWorldDatabase::IsInitialized() const
    {
        return m_Initialized;
    }

    u32 InMemoryWorldDatabase::GetPlayerCount() const
    {
        std::lock_guard lock(m_Mutex);
        return static_cast<u32>(m_PlayerStates.size());
    }

    u32 InMemoryWorldDatabase::GetEntityCount() const
    {
        std::lock_guard lock(m_Mutex);
        return static_cast<u32>(m_EntityStates.size());
    }

    void InMemoryWorldDatabase::Clear()
    {
        std::lock_guard lock(m_Mutex);
        m_PlayerStates.clear();
        m_EntityStates.clear();
        m_WorldState.clear();
    }
} // namespace OloEngine
