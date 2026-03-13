#include "OloEnginePCH.h"
#include "LayerManager.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine
{
    void LayerManager::SetConfig(const LayerConfig& config)
    {
        m_Config = config;
    }

    LayerID LayerManager::CreateLayer(const ZoneDefinition& templateZone)
    {
        LayerID const layerID = m_NextLayerID++;

        ZoneDefinition layerDef = templateZone;
        layerDef.ID = 20000 + layerID; // Offset to avoid zone/instance ID conflicts
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
        // If player has a party, check if party already has a layer
        if (partyID != 0)
        {
            auto partyIt = m_PartyLayerMap.find(partyID);
            if (partyIt != m_PartyLayerMap.end())
            {
                LayerID layerID = partyIt->second;
                auto* server = GetLayerServer(layerID);
                if (server)
                {
                    server->AddPlayer(clientID);
                    m_PlayerLayerMap[clientID] = layerID;
                    m_LayerInfos[layerID].PlayerCount++;
                    return layerID;
                }
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
        auto it = m_PlayerLayerMap.find(clientID);
        if (it == m_PlayerLayerMap.end())
        {
            return 0;
        }
        return it->second;
    }

    ZoneServer* LayerManager::GetLayerServer(LayerID layerID)
    {
        auto it = m_Layers.find(layerID);
        if (it == m_Layers.end())
        {
            return nullptr;
        }
        return &it->second;
    }

    std::vector<LayerID> LayerManager::GetLayersForZone(ZoneID zoneID) const
    {
        auto it = m_ZoneLayers.find(zoneID);
        if (it == m_ZoneLayers.end())
        {
            return {};
        }
        return it->second;
    }

    u32 LayerManager::GetLayerCount() const
    {
        return static_cast<u32>(m_Layers.size());
    }

    u32 LayerManager::TryMergeLayers()
    {
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

                // Move all players from source to target
                std::vector<u32> playersToMove;
                for (auto& [clientID, layerID] : m_PlayerLayerMap)
                {
                    if (layerID == source)
                    {
                        playersToMove.push_back(clientID);
                    }
                }

                auto* targetServer = GetLayerServer(target);
                for (u32 clientID : playersToMove)
                {
                    if (targetServer)
                    {
                        targetServer->AddPlayer(clientID);
                    }
                    m_PlayerLayerMap[clientID] = target;
                }

                // Update counts
                auto targetInfoIt = m_LayerInfos.find(target);
                auto sourceInfoIt = m_LayerInfos.find(source);
                if (targetInfoIt != m_LayerInfos.end() && sourceInfoIt != m_LayerInfos.end())
                {
                    targetInfoIt->second.PlayerCount += sourceInfoIt->second.PlayerCount;
                }

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
        for (auto& [id, server] : m_Layers)
        {
            if (server.IsRunning())
            {
                server.Tick(dt);
            }
        }
    }
} // namespace OloEngine
