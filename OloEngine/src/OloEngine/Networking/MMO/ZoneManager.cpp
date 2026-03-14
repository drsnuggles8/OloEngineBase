#include "OloEnginePCH.h"
#include "ZoneManager.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"

namespace OloEngine
{
    static constexpr ZoneID kMaxBaseZoneID = 9999; // Base zone IDs must stay below instance/layer ranges

    void ZoneManager::RegisterZone(const ZoneDefinition& definition)
    {
        OLO_CORE_ASSERT(definition.ID != InvalidZoneID,
                        "ZoneManager: cannot register a zone with InvalidZoneID (0)");
        OLO_CORE_ASSERT(definition.ID <= kMaxBaseZoneID,
                        "ZoneManager: base zone ID must be < 10000 to avoid collision with instance/layer ID spaces");

        if (m_Zones.contains(definition.ID))
        {
            OLO_CORE_WARN("[ZoneManager] Zone ID {} already registered", definition.ID);
            return;
        }

        ZoneServer server;
        server.Initialize(definition);
        m_Zones.emplace(definition.ID, std::move(server));
        OLO_CORE_INFO("[ZoneManager] Registered zone '{}' (ID={})", definition.Name, definition.ID);
    }

    void ZoneManager::StartAll()
    {
        for (auto& [id, zone] : m_Zones)
        {
            zone.Start();
        }
    }

    void ZoneManager::StopAll()
    {
        for (auto& [id, zone] : m_Zones)
        {
            zone.Stop();
        }
        m_PlayerZoneMap.clear();
    }

    void ZoneManager::TickAll(f32 dt)
    {
        OLO_PROFILE_FUNCTION();

        for (auto& [id, zone] : m_Zones)
        {
            if (zone.IsRunning())
            {
                zone.Tick(dt);
            }
        }

        // Expire stale handoff transactions
        std::vector<u32> expiredHandoffs;
        for (auto& [txID, tx] : m_ActiveHandoffs)
        {
            tx.ElapsedTime += dt;
            if (tx.ElapsedTime >= m_HandoffTimeout)
            {
                expiredHandoffs.push_back(txID);
            }
        }
        for (u32 txID : expiredHandoffs)
        {
            OLO_CORE_WARN("[ZoneManager] Handoff {} timed out after {:.1f}s", txID, m_HandoffTimeout);
            RejectHandoff(txID);
        }
    }

    ZoneServer* ZoneManager::GetZoneAt(const glm::vec3& position)
    {
        for (auto& [id, zone] : m_Zones)
        {
            if (zone.GetDefinition().Bounds.Contains(position))
            {
                return &zone;
            }
        }
        return nullptr;
    }

    ZoneServer* ZoneManager::GetZone(ZoneID zoneID)
    {
        auto it = m_Zones.find(zoneID);
        if (it == m_Zones.end())
        {
            return nullptr;
        }
        return &it->second;
    }

    ZoneID ZoneManager::RoutePlayerToZone(u32 clientID, const glm::vec3& position)
    {
        auto* zone = GetZoneAt(position);
        if (!zone)
        {
            OLO_CORE_WARN("[ZoneManager] No zone found for position ({}, {}, {})", position.x, position.y, position.z);
            return InvalidZoneID;
        }

        if (!zone->AddPlayer(clientID))
        {
            return InvalidZoneID; // Zone full
        }

        // Successfully added — now remove from old zone
        auto it = m_PlayerZoneMap.find(clientID);
        if (it != m_PlayerZoneMap.end())
        {
            if (auto* oldZone = GetZone(it->second))
            {
                oldZone->RemovePlayer(clientID);
            }
        }

        ZoneID const zoneID = zone->GetZoneID();
        m_PlayerZoneMap[clientID] = zoneID;
        return zoneID;
    }

    bool ZoneManager::TransferPlayerToZone(u32 clientID, ZoneID targetZone)
    {
        auto* target = GetZone(targetZone);
        if (!target)
        {
            return false;
        }

        if (target->IsFull())
        {
            return false;
        }

        // Add to target zone first
        if (!target->AddPlayer(clientID))
        {
            return false;
        }

        // Successfully added — now remove from old zone
        auto it = m_PlayerZoneMap.find(clientID);
        if (it != m_PlayerZoneMap.end())
        {
            if (auto* oldZone = GetZone(it->second))
            {
                oldZone->RemovePlayer(clientID);
            }
        }

        m_PlayerZoneMap[clientID] = targetZone;
        return true;
    }

    void ZoneManager::RemovePlayer(u32 clientID)
    {
        auto it = m_PlayerZoneMap.find(clientID);
        if (it == m_PlayerZoneMap.end())
        {
            return;
        }

        if (auto* zone = GetZone(it->second))
        {
            zone->RemovePlayer(clientID);
        }

        m_PlayerZoneMap.erase(it);
    }

    ZoneID ZoneManager::GetPlayerZone(u32 clientID) const
    {
        auto it = m_PlayerZoneMap.find(clientID);
        if (it == m_PlayerZoneMap.end())
        {
            return InvalidZoneID;
        }
        return it->second;
    }

    std::vector<const ZoneDefinition*> ZoneManager::GetAllZoneDefinitions() const
    {
        std::vector<const ZoneDefinition*> result;
        result.reserve(m_Zones.size());
        for (auto const& [id, zone] : m_Zones)
        {
            result.push_back(&zone.GetDefinition());
        }
        return result;
    }

    u32 ZoneManager::GetZoneCount() const
    {
        return static_cast<u32>(m_Zones.size());
    }

    u32 ZoneManager::BeginHandoff(u32 clientID, ZoneID targetZoneID, const PlayerStatePacket& state)
    {
        auto* target = GetZone(targetZoneID);
        if (!target || !target->IsRunning())
        {
            return 0;
        }

        ZoneID sourceZoneID = GetPlayerZone(clientID);
        if (sourceZoneID == InvalidZoneID)
        {
            return 0;
        }

        auto* source = GetZone(sourceZoneID);
        if (source)
        {
            source->SetPlayerTransitioning(clientID, true);
        }

        u32 transactionID = m_NextTransactionID++;

        HandoffTransaction tx;
        tx.TransactionID = transactionID;
        tx.ClientID = clientID;
        tx.SourceZoneID = sourceZoneID;
        tx.TargetZoneID = targetZoneID;
        tx.State = EHandoffState::Requested;
        tx.PlayerState = state;

        m_ActiveHandoffs[transactionID] = std::move(tx);
        OLO_CORE_INFO("[ZoneManager] Handoff {} started: player {} from zone {} to zone {}",
                      transactionID, clientID, sourceZoneID, targetZoneID);
        return transactionID;
    }

    bool ZoneManager::AcceptHandoff(u32 transactionID)
    {
        auto it = m_ActiveHandoffs.find(transactionID);
        if (it == m_ActiveHandoffs.end())
        {
            return false;
        }

        auto& tx = it->second;
        if (tx.State != EHandoffState::Requested)
        {
            return false;
        }

        auto* target = GetZone(tx.TargetZoneID);
        if (!target || target->IsFull())
        {
            RejectHandoff(transactionID);
            return false;
        }

        tx.State = EHandoffState::Ready;
        return true;
    }

    bool ZoneManager::CompleteHandoff(u32 transactionID)
    {
        auto it = m_ActiveHandoffs.find(transactionID);
        if (it == m_ActiveHandoffs.end())
        {
            return false;
        }

        auto& tx = it->second;
        if (tx.State != EHandoffState::Ready)
        {
            return false;
        }

        tx.State = EHandoffState::Completing;

        // Remove from source zone
        auto* source = GetZone(tx.SourceZoneID);
        if (source)
        {
            source->SetPlayerTransitioning(tx.ClientID, false);
            source->RemovePlayer(tx.ClientID);
        }

        // Add to target zone
        auto* target = GetZone(tx.TargetZoneID);
        if (target && target->AddPlayer(tx.ClientID))
        {
            m_PlayerZoneMap[tx.ClientID] = tx.TargetZoneID;
            tx.State = EHandoffState::Completed;
            OLO_CORE_INFO("[ZoneManager] Handoff {} completed: player {} now in zone {}",
                          transactionID, tx.ClientID, tx.TargetZoneID);
            m_ActiveHandoffs.erase(it);
            return true;
        }

        // Target rejected — restore source
        if (source)
        {
            source->AddPlayer(tx.ClientID);
        }
        tx.State = EHandoffState::Rejected;
        m_ActiveHandoffs.erase(it);
        return false;
    }

    void ZoneManager::RejectHandoff(u32 transactionID)
    {
        auto it = m_ActiveHandoffs.find(transactionID);
        if (it == m_ActiveHandoffs.end())
        {
            return;
        }

        auto& tx = it->second;
        auto* source = GetZone(tx.SourceZoneID);
        if (source)
        {
            source->SetPlayerTransitioning(tx.ClientID, false);
        }

        OLO_CORE_WARN("[ZoneManager] Handoff {} rejected for player {}", transactionID, tx.ClientID);
        m_ActiveHandoffs.erase(it);
    }

    const HandoffTransaction* ZoneManager::GetHandoff(u32 transactionID) const
    {
        auto it = m_ActiveHandoffs.find(transactionID);
        if (it == m_ActiveHandoffs.end())
        {
            return nullptr;
        }
        return &it->second;
    }

    void ZoneManager::SetHandoffTimeout(f32 seconds)
    {
        m_HandoffTimeout = seconds;
    }

    f32 ZoneManager::GetHandoffTimeout() const
    {
        return m_HandoffTimeout;
    }
} // namespace OloEngine
