#include "OloEnginePCH.h"
#include "LayerManager.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"

namespace OloEngine
{
    static constexpr LayerID kLayerZoneIDOffset = 20000;
    static constexpr LayerID kMaxLayerID = 9999; // Keeps zone IDs in [20000, 29999]

    void LayerManager::SetConfig(const LayerConfig& config)
    {
        OLO_PROFILE_FUNCTION();
        m_Config = config;
    }

    LayerID LayerManager::CreateLayer(const ZoneDefinition& templateZone)
    {
        OLO_PROFILE_FUNCTION();

        if (m_NextLayerID > kMaxLayerID)
        {
            OLO_CORE_ERROR("LayerManager: layer ID exhausted (max {})", kMaxLayerID);
            return 0;
        }
        LayerID const layerID = m_NextLayerID++;

        ZoneDefinition layerDef = templateZone;
        layerDef.ID = kLayerZoneIDOffset + layerID;
        layerDef.Name = templateZone.Name + "_Layer_" + std::to_string(layerID);

        ZoneServer server;
        server.Initialize(layerDef);
        server.Start();

        LayerInfo info;
        info.ID = layerID;
        info.ZoneTemplateID = templateZone.ID;
        info.PlayerCount = 0;

        m_Layers.emplace(layerID, std::move(server));
        m_LayerInfos[layerID] = info;
        m_ZoneLayers[templateZone.ID].push_back(layerID);

        OLO_CORE_INFO("[LayerManager] Created layer {} for zone '{}'", layerID, templateZone.Name);
        return layerID;
    }

    LayerID LayerManager::AssignPlayerToLayer(ZoneID zoneID, u32 clientID, u32 partyID)
    {
        OLO_PROFILE_FUNCTION();

        // If player has a party, check if party already has a layer
        if (partyID != 0)
        {
            auto partyIt = m_PartyLayerMap.find(partyID);
            if (partyIt != m_PartyLayerMap.end())
            {
                LayerID layerID = partyIt->second;
                // Validate the cached layer belongs to the requested zone
                auto infoIt = m_LayerInfos.find(layerID);
                if (infoIt != m_LayerInfos.end() && infoIt->second.ZoneTemplateID == zoneID)
                {
                    auto* server = GetLayerServer(layerID);
                    if (server && server->AddPlayer(clientID))
                    {
                        m_PlayerLayerMap[clientID] = layerID;
                        infoIt->second.PlayerCount++;
                        return layerID;
                    }
                }
                // Stale or wrong-zone entry — remove and fall through to normal allocation
                m_PartyLayerMap.erase(partyIt);
            }
        }

        // Find a layer for this zone that isn't at soft cap
        auto zoneLayers = m_ZoneLayers.find(zoneID);
        if (zoneLayers != m_ZoneLayers.end())
        {
            for (LayerID layerID : zoneLayers->second)
            {
                auto infoIt = m_LayerInfos.find(layerID);
                if (infoIt != m_LayerInfos.end() && infoIt->second.PlayerCount < m_Config.SoftCap)
                {
                    auto* server = GetLayerServer(layerID);
                    if (server && server->AddPlayer(clientID))
                    {
                        m_PlayerLayerMap[clientID] = layerID;
                        infoIt->second.PlayerCount++;
                        if (partyID != 0)
                        {
                            m_PartyLayerMap[partyID] = layerID;
                        }
                        return layerID;
                    }
                }
            }
        }

        // All layers at soft cap or no layers exist — need a zone def to create a new layer
        // Cannot auto-create without a ZoneDefinition, return 0 to signal failure
        OLO_CORE_WARN("[LayerManager] No available layer for zone {}, client {}", zoneID, clientID);
        return 0;
    }

    void LayerManager::RemovePlayerFromLayer(u32 clientID)
    {
        OLO_PROFILE_FUNCTION();

        auto it = m_PlayerLayerMap.find(clientID);
        if (it == m_PlayerLayerMap.end())
        {
            return;
        }

        LayerID layerID = it->second;
        auto* server = GetLayerServer(layerID);
        if (server)
        {
            server->RemovePlayer(clientID);
        }

        auto infoIt = m_LayerInfos.find(layerID);
        if (infoIt != m_LayerInfos.end() && infoIt->second.PlayerCount > 0)
        {
            infoIt->second.PlayerCount--;
        }

        m_PlayerLayerMap.erase(it);
    }

    LayerID LayerManager::GetPlayerLayer(u32 clientID) const
    {
        OLO_PROFILE_FUNCTION();

        auto it = m_PlayerLayerMap.find(clientID);
        if (it == m_PlayerLayerMap.end())
        {
            return 0;
        }
        return it->second;
    }

    ZoneServer* LayerManager::GetLayerServer(LayerID layerID)
    {
        OLO_PROFILE_FUNCTION();

        auto it = m_Layers.find(layerID);
        if (it == m_Layers.end())
        {
            return nullptr;
        }
        return &it->second;
    }

    std::vector<LayerID> LayerManager::GetLayersForZone(ZoneID zoneID) const
    {
        OLO_PROFILE_FUNCTION();

        auto it = m_ZoneLayers.find(zoneID);
        if (it == m_ZoneLayers.end())
        {
            return {};
        }
        return it->second;
    }

    u32 LayerManager::GetLayerCount() const
    {
        OLO_PROFILE_FUNCTION();
        return static_cast<u32>(m_Layers.size());
    }

    u32 LayerManager::TryMergeLayers()
    {
        OLO_PROFILE_FUNCTION();

        u32 mergeCount = 0;

        for (auto& [zoneID, layerIDs] : m_ZoneLayers)
        {
            if (layerIDs.size() < 2)
            {
                continue;
            }

            // Find layers below merge cap
            std::vector<LayerID> mergeCandidates;
            for (LayerID layerID : layerIDs)
            {
                auto infoIt = m_LayerInfos.find(layerID);
                if (infoIt != m_LayerInfos.end() && infoIt->second.PlayerCount <= m_Config.MergeCap)
                {
                    mergeCandidates.push_back(layerID);
                }
            }

            // Merge pairs of under-populated layers
            while (mergeCandidates.size() >= 2)
            {
                LayerID target = mergeCandidates[0];
                LayerID source = mergeCandidates[1];

                // Check if combined count would exceed SoftCap
                auto targetInfoIt = m_LayerInfos.find(target);
                auto sourceInfoIt = m_LayerInfos.find(source);
                if (targetInfoIt == m_LayerInfos.end() || sourceInfoIt == m_LayerInfos.end())
                {
                    // Missing layer info — skip this candidate pair
                    mergeCandidates.erase(mergeCandidates.begin() + 1);
                    continue;
                }
                if (targetInfoIt->second.PlayerCount + sourceInfoIt->second.PlayerCount > m_Config.SoftCap)
                {
                    mergeCandidates.erase(mergeCandidates.begin() + 1);
                    continue;
                }

                // Move all players from source to target
                auto* targetServer = GetLayerServer(target);
                auto* sourceServer = GetLayerServer(source);
                if (!targetServer)
                {
                    mergeCandidates.erase(mergeCandidates.begin() + 1);
                    continue;
                }

                std::vector<u32> playersToMove;
                for (auto& [clientID, layerID] : m_PlayerLayerMap)
                {
                    if (layerID == source)
                    {
                        playersToMove.push_back(clientID);
                    }
                }

                // Two-phase: attempt all adds first
                std::vector<u32> movedClients;
                bool mergeOk = true;
                for (u32 clientID : playersToMove)
                {
                    if (targetServer->AddPlayer(clientID))
                    {
                        movedClients.push_back(clientID);
                    }
                    else
                    {
                        // Rollback successful adds
                        for (u32 movedID : movedClients)
                        {
                            targetServer->RemovePlayer(movedID);
                        }
                        mergeOk = false;
                        break;
                    }
                }

                if (!mergeOk)
                {
                    mergeCandidates.erase(mergeCandidates.begin() + 1);
                    continue;
                }

                // Remove from source and update player map
                for (u32 clientID : movedClients)
                {
                    if (sourceServer)
                    {
                        sourceServer->RemovePlayer(clientID);
                    }
                    m_PlayerLayerMap[clientID] = target;
                }

                // Update party layer mappings: any party pointing to source now points to target
                for (auto& [partyID, layerID] : m_PartyLayerMap)
                {
                    if (layerID == source)
                    {
                        layerID = target;
                    }
                }

                // Update counts (iterators from capacity check are still valid)
                targetInfoIt->second.PlayerCount += sourceInfoIt->second.PlayerCount;

                // Destroy source layer
                auto sourceIt = m_Layers.find(source);
                if (sourceIt != m_Layers.end())
                {
                    sourceIt->second.Stop();
                    m_Layers.erase(sourceIt);
                }
                m_LayerInfos.erase(source);

                // Remove from zone layers list
                auto& zl = m_ZoneLayers[zoneID];
                zl.erase(std::remove(zl.begin(), zl.end(), source), zl.end());

                mergeCandidates.erase(mergeCandidates.begin() + 1);
                mergeCount++;

                OLO_CORE_INFO("[LayerManager] Merged layer {} into layer {} for zone {}", source, target, zoneID);
            }
        }

        return mergeCount;
    }

    void LayerManager::TickAll(f32 dt)
    {
        OLO_PROFILE_FUNCTION();

        for (auto& [id, server] : m_Layers)
        {
            if (server.IsRunning())
            {
                server.Tick(dt);
            }
        }
    }
} // namespace OloEngine
