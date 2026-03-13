#include "OloEnginePCH.h"
#include "InstanceManager.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine
{
    InstanceID InstanceManager::CreateInstance(const ZoneDefinition& templateZone, EInstanceType type, u32 maxPlayers)
    {
        InstanceID const instanceID = m_NextInstanceID++;

        ZoneDefinition instanceDef = templateZone;
        instanceDef.ID = 10000 + instanceID; // Offset to avoid conflicting with zone IDs
        instanceDef.MaxPlayers = maxPlayers;
        instanceDef.Name = templateZone.Name + "_Instance_" + std::to_string(instanceID);

        ZoneServer server;
        server.Initialize(instanceDef);
        server.Start();

        InstanceInfo info;
        info.ID = instanceID;
        info.TemplateZoneID = templateZone.ID;
        info.Type = type;
        info.MaxPlayers = maxPlayers;
        info.LastPlayerTime = std::chrono::steady_clock::now();

        m_Instances.emplace(instanceID, std::move(server));
        m_InstanceInfos[instanceID] = info;

        OLO_CORE_INFO("[InstanceManager] Created instance {} (type={}, template zone={}, max={})",
                      instanceID, static_cast<int>(type), templateZone.Name, maxPlayers);
        return instanceID;
    }

    void InstanceManager::DestroyInstance(InstanceID instanceID)
    {
        auto it = m_Instances.find(instanceID);
        if (it == m_Instances.end())
        {
            return;
        }

        it->second.Stop();
        m_Instances.erase(it);
        m_InstanceInfos.erase(instanceID);
        OLO_CORE_INFO("[InstanceManager] Destroyed instance {}", instanceID);
    }

    ZoneServer* InstanceManager::GetInstance(InstanceID instanceID)
    {
        auto it = m_Instances.find(instanceID);
        if (it == m_Instances.end())
        {
            return nullptr;
        }
        return &it->second;
    }

    bool InstanceManager::AddPlayerToInstance(InstanceID instanceID, u32 clientID)
    {
        auto* server = GetInstance(instanceID);
        if (!server)
        {
            return false;
        }

        if (!server->AddPlayer(clientID))
        {
            return false;
        }

        // Update last player time
        auto infoIt = m_InstanceInfos.find(instanceID);
        if (infoIt != m_InstanceInfos.end())
        {
            infoIt->second.LastPlayerTime = std::chrono::steady_clock::now();
        }
        return true;
    }

    void InstanceManager::RemovePlayerFromInstance(InstanceID instanceID, u32 clientID)
    {
        auto* server = GetInstance(instanceID);
        if (!server)
        {
            return;
        }
        server->RemovePlayer(clientID);

        // Update last player time
        if (server->GetPlayerCount() > 0)
        {
            auto infoIt = m_InstanceInfos.find(instanceID);
            if (infoIt != m_InstanceInfos.end())
            {
                infoIt->second.LastPlayerTime = std::chrono::steady_clock::now();
            }
        }
    }

    bool InstanceManager::HasInstance(InstanceID instanceID) const
    {
        return m_Instances.contains(instanceID);
    }

    const InstanceInfo* InstanceManager::GetInstanceInfo(InstanceID instanceID) const
    {
        auto it = m_InstanceInfos.find(instanceID);
        if (it == m_InstanceInfos.end())
        {
            return nullptr;
        }
        return &it->second;
    }

    u32 InstanceManager::GetInstanceCount() const
    {
        return static_cast<u32>(m_Instances.size());
    }

    void InstanceManager::TickAll(f32 dt, f32 gracePeriodSeconds)
    {
        auto now = std::chrono::steady_clock::now();

        // Collect instances to destroy (avoid modifying map while iterating)
        std::vector<InstanceID> toDestroy;

        for (auto& [id, server] : m_Instances)
        {
            if (server.IsRunning())
            {
                server.Tick(dt);
            }

            // Auto-destroy empty non-OpenWorld instances after grace period
            auto infoIt = m_InstanceInfos.find(id);
            if (infoIt != m_InstanceInfos.end() && infoIt->second.Type != EInstanceType::OpenWorld)
            {
                if (server.GetPlayerCount() == 0)
                {
                    auto elapsed = std::chrono::duration<f32>(now - infoIt->second.LastPlayerTime).count();
                    if (elapsed >= gracePeriodSeconds)
                    {
                        toDestroy.push_back(id);
                    }
                }
            }
        }

        for (InstanceID id : toDestroy)
        {
            OLO_CORE_INFO("[InstanceManager] Auto-destroying empty instance {} (grace period expired)", id);
            DestroyInstance(id);
        }
    }
} // namespace OloEngine
