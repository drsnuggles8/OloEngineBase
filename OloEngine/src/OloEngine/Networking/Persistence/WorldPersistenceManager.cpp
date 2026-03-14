#include "OloEnginePCH.h"
#include "WorldPersistenceManager.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine
{
    void WorldPersistenceManager::Initialize(IWorldDatabase* database, f32 saveIntervalSeconds)
    {
        m_Database = database;
        m_SaveInterval = saveIntervalSeconds;
        m_TimeSinceLastSave = 0.0f;
    }

    void WorldPersistenceManager::Shutdown()
    {
        if (m_Database)
        {
            SaveAll();
        }
        m_Database = nullptr;
        m_DirtyEntities.clear();
    }

    void WorldPersistenceManager::MarkDirty(u64 uuid)
    {
        m_DirtyEntities.insert(uuid);
    }

    bool WorldPersistenceManager::IsDirty(u64 uuid) const
    {
        return m_DirtyEntities.contains(uuid);
    }

    u32 WorldPersistenceManager::GetDirtyCount() const
    {
        return static_cast<u32>(m_DirtyEntities.size());
    }

    void WorldPersistenceManager::Tick(f32 dt)
    {
        m_TimeSinceLastSave += dt;
        if (m_TimeSinceLastSave >= m_SaveInterval && !m_DirtyEntities.empty())
        {
            OLO_CORE_INFO("[WorldPersistenceManager] Auto-save triggered ({} dirty entities)", m_DirtyEntities.size());
            SaveAll();
        }
    }

    void WorldPersistenceManager::SaveAll()
    {
        if (!m_Database)
        {
            return;
        }

        OLO_CORE_INFO("[WorldPersistenceManager] Saving all {} dirty entities", m_DirtyEntities.size());

        if (m_DataProvider)
        {
            std::vector<u64> saved;
            for (u64 uuid : m_DirtyEntities)
            {
                ZoneID zoneID = 0;
                std::vector<u8> data;
                if (m_DataProvider(uuid, zoneID, data))
                {
                    if (m_Database->SaveEntityState(uuid, zoneID, data))
                    {
                        saved.push_back(uuid);
                    }
                }
            }
            for (u64 uuid : saved)
            {
                m_DirtyEntities.erase(uuid);
            }
        }
        else
        {
            m_DirtyEntities.clear();
        }

        m_TimeSinceLastSave = 0.0f;
    }

    void WorldPersistenceManager::SetEntityDataProvider(EntityDataProvider provider)
    {
        m_DataProvider = std::move(provider);
    }

    bool WorldPersistenceManager::SaveEntity(u64 uuid, ZoneID zoneID, const std::vector<u8>& data)
    {
        if (!m_Database)
        {
            return false;
        }
        bool result = m_Database->SaveEntityState(uuid, zoneID, data);
        if (result)
        {
            m_DirtyEntities.erase(uuid);
        }
        return result;
    }

    bool WorldPersistenceManager::LoadEntitiesForZone(ZoneID zoneID, std::vector<std::pair<u64, std::vector<u8>>>& outEntities)
    {
        if (!m_Database)
        {
            return false;
        }
        return m_Database->LoadEntitiesForZone(zoneID, outEntities);
    }

    bool WorldPersistenceManager::SavePlayer(u32 accountID, const PlayerStatePacket& state)
    {
        if (!m_Database)
        {
            return false;
        }
        return m_Database->SavePlayerState(accountID, state);
    }

    bool WorldPersistenceManager::LoadPlayer(u32 accountID, PlayerStatePacket& outState)
    {
        if (!m_Database)
        {
            return false;
        }
        return m_Database->LoadPlayerState(accountID, outState);
    }

    f32 WorldPersistenceManager::GetSaveInterval() const
    {
        return m_SaveInterval;
    }
} // namespace OloEngine
