#include "OloEnginePCH.h"
#include "ZoneServer.h"
#include "OloEngine/Networking/MMO/InterZoneMessageBus.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"

namespace OloEngine
{
    void ZoneServer::Initialize(const ZoneDefinition& definition)
    {
        m_Definition = definition;
        m_SpatialGrid.SetCellSize(64.0f);
    }

    void ZoneServer::Start()
    {
        if (m_Running)
        {
            return;
        }
        m_Running = true;
        OLO_CORE_INFO("[ZoneServer] Zone '{}' (ID={}) started", m_Definition.Name, m_Definition.ID);
    }

    void ZoneServer::Stop()
    {
        if (!m_Running)
        {
            return;
        }
        m_Running = false;
        m_Players.clear();
        m_TransitioningPlayers.clear();
        m_GhostEntities.clear();
        m_SpatialGrid.Clear();
        OLO_CORE_INFO("[ZoneServer] Zone '{}' (ID={}) stopped", m_Definition.Name, m_Definition.ID);
    }

    void ZoneServer::Tick([[maybe_unused]] f32 dt)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Running)
        {
            return;
        }

        // Process incoming inter-zone messages targeted at this zone
        if (m_MessageBus)
        {
            auto messages = m_MessageBus->DrainForZone(m_Definition.ID);
            for (auto const& msg : messages)
            {
                switch (msg.Type)
                {
                    case EInterZoneMessageType::ChatRelay:
                        OLO_CORE_TRACE("[ZoneServer] Zone '{}' received chat relay from zone {}", m_Definition.Name,
                                       msg.SourceZoneID);
                        break;
                    case EInterZoneMessageType::WorldEvent:
                        OLO_CORE_TRACE("[ZoneServer] Zone '{}' received world event from zone {}", m_Definition.Name,
                                       msg.SourceZoneID);
                        break;
                    case EInterZoneMessageType::AdminCommand:
                        OLO_CORE_INFO("[ZoneServer] Zone '{}' received admin command from zone {}", m_Definition.Name,
                                      msg.SourceZoneID);
                        break;
                    default:
                        break;
                }
            }
        }
    }

    void ZoneServer::SetMessageBus(InterZoneMessageBus* bus)
    {
        m_MessageBus = bus;
    }

    bool ZoneServer::AddPlayer(u32 clientID)
    {
        if (IsFull())
        {
            OLO_CORE_WARN("[ZoneServer] Zone '{}' is full ({}/{}), rejecting player {}",
                          m_Definition.Name, m_Players.size(), m_Definition.MaxPlayers, clientID);
            return false;
        }
        m_Players.insert(clientID);
        return true;
    }

    void ZoneServer::RemovePlayer(u32 clientID)
    {
        m_Players.erase(clientID);
        m_TransitioningPlayers.erase(clientID);
    }

    bool ZoneServer::HasPlayer(u32 clientID) const
    {
        return m_Players.contains(clientID);
    }

    u32 ZoneServer::GetPlayerCount() const
    {
        return static_cast<u32>(m_Players.size());
    }

    bool ZoneServer::IsFull() const
    {
        return m_Players.size() >= m_Definition.MaxPlayers;
    }

    bool ZoneServer::IsRunning() const
    {
        return m_Running;
    }

    ZoneID ZoneServer::GetZoneID() const
    {
        return m_Definition.ID;
    }

    const std::string& ZoneServer::GetName() const
    {
        return m_Definition.Name;
    }

    const ZoneDefinition& ZoneServer::GetDefinition() const
    {
        return m_Definition;
    }

    const SpatialGrid& ZoneServer::GetSpatialGrid() const
    {
        return m_SpatialGrid;
    }

    NetworkInterestManager& ZoneServer::GetInterestManager()
    {
        return m_InterestManager;
    }

    void ZoneServer::UpdateEntityPosition(u64 uuid, const glm::vec3& position)
    {
        m_SpatialGrid.InsertOrUpdate(uuid, position);
    }

    void ZoneServer::RemoveEntity(u64 uuid)
    {
        m_SpatialGrid.Remove(uuid);
    }

    u32 ZoneServer::GetEntityCount() const
    {
        return m_SpatialGrid.GetEntityCount();
    }

    void ZoneServer::SetPlayerTransitioning(u32 clientID, bool transitioning)
    {
        if (transitioning)
        {
            m_TransitioningPlayers.insert(clientID);
        }
        else
        {
            m_TransitioningPlayers.erase(clientID);
        }
    }

    bool ZoneServer::IsPlayerTransitioning(u32 clientID) const
    {
        return m_TransitioningPlayers.contains(clientID);
    }

    void ZoneServer::AddGhostEntity(u64 uuid, const glm::vec3& position)
    {
        m_GhostEntities.insert(uuid);
        m_SpatialGrid.InsertOrUpdate(uuid, position);
    }

    void ZoneServer::RemoveGhostEntity(u64 uuid)
    {
        m_GhostEntities.erase(uuid);
        m_SpatialGrid.Remove(uuid);
    }

    bool ZoneServer::IsGhostEntity(u64 uuid) const
    {
        return m_GhostEntities.contains(uuid);
    }

    u32 ZoneServer::GetGhostEntityCount() const
    {
        return static_cast<u32>(m_GhostEntities.size());
    }
} // namespace OloEngine
