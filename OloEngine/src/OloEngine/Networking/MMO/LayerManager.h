#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/MMO/ZoneDefinition.h"
#include "OloEngine/Networking/MMO/ZoneServer.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OloEngine
{
    using LayerID = u32;

    struct LayerConfig
    {
        u32 SoftCap = 200;  // When to create a new layer
        u32 MergeCap = 50;  // When to merge layers
    };

    struct LayerInfo
    {
        LayerID ID = 0;
        ZoneID ZoneTemplateID = 0;
        u32 PlayerCount = 0;
    };

    // Manages transparent copies (layers) of overworld zones for population balancing.
    // Players in a party are always kept on the same layer.
    class LayerManager
    {
      public:
        LayerManager() = default;

        void SetConfig(const LayerConfig& config);

        // Create an initial layer for a zone.
        LayerID CreateLayer(const ZoneDefinition& templateZone);

        // Assign a player to a layer for a zone. Creates new layer if all are at SoftCap.
        // If partyID != 0, ensures all party members stay on the same layer.
        LayerID AssignPlayerToLayer(ZoneID zoneID, u32 clientID, u32 partyID = 0);

        // Remove a player from their layer.
        void RemovePlayerFromLayer(u32 clientID);

        // Get the layer a player is on.
        [[nodiscard]] LayerID GetPlayerLayer(u32 clientID) const;

        // Get the zone server for a specific layer.
        [[nodiscard]] ZoneServer* GetLayerServer(LayerID layerID);

        // Get all layers for a zone.
        [[nodiscard]] std::vector<LayerID> GetLayersForZone(ZoneID zoneID) const;

        // Get number of active layers.
        [[nodiscard]] u32 GetLayerCount() const;

        // Attempt to merge under-populated layers.
        // Returns number of merges performed.
        u32 TryMergeLayers();

        // Tick all layers.
        void TickAll(f32 dt);

      private:
        LayerConfig m_Config;
        std::unordered_map<LayerID, ZoneServer> m_Layers;
        std::unordered_map<LayerID, LayerInfo> m_LayerInfos;
        std::unordered_map<ZoneID, std::vector<LayerID>> m_ZoneLayers; // zoneID → layers
        std::unordered_map<u32, LayerID> m_PlayerLayerMap;             // clientID → layerID
        std::unordered_map<u32, LayerID> m_PartyLayerMap;              // partyID → layerID
        LayerID m_NextLayerID = 1;
    };
} // namespace OloEngine
