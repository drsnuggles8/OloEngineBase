#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/MMO/PlayerStatePacket.h"
#include "OloEngine/Networking/MMO/ZoneDefinition.h"
#include "OloEngine/Networking/MMO/ZoneServer.h"

#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    // Central coordinator for all zone servers.
    // Routes players to zones, manages zone lifecycle, and provides zone discovery.
    class ZoneManager
    {
      public:
        ZoneManager() = default;

        // Register a zone definition. Does not start the zone.
        void RegisterZone(const ZoneDefinition& definition);

        // Start all registered zones.
        void StartAll();

        // Stop all running zones.
        void StopAll();

        // Tick all running zones.
        void TickAll(f32 dt);

        // Find the zone that contains the given world position.
        // Returns nullptr if no zone contains the point.
        [[nodiscard]] ZoneServer* GetZoneAt(const glm::vec3& position);

        // Get a zone by ID.
        [[nodiscard]] ZoneServer* GetZone(ZoneID zoneID);

        // Route a player to the appropriate zone based on their position.
        // Returns the ZoneID they were routed to, or 0 if no zone found.
        ZoneID RoutePlayerToZone(u32 clientID, const glm::vec3& position);

        // Explicitly move a player to a specific zone.
        bool TransferPlayerToZone(u32 clientID, ZoneID targetZone);

        // Remove a player from whatever zone they're in.
        void RemovePlayer(u32 clientID);

        // Find which zone a player is currently in. Returns 0 if not in any zone.
        [[nodiscard]] ZoneID GetPlayerZone(u32 clientID) const;

        // Initiate a three-phase handoff.
        // Returns a transaction ID, or 0 on failure.
        u32 BeginHandoff(u32 clientID, ZoneID targetZoneID, const PlayerStatePacket& state);

        // Called by target zone to accept a handoff.
        bool AcceptHandoff(u32 transactionID);

        // Complete a handoff — removes player from source, finalizes in target.
        bool CompleteHandoff(u32 transactionID);

        // Reject a handoff — player stays in source zone.
        void RejectHandoff(u32 transactionID);

        // Get the state of an active handoff transaction.
        [[nodiscard]] const HandoffTransaction* GetHandoff(u32 transactionID) const;

        // Get all registered zone definitions.
        [[nodiscard]] std::vector<const ZoneDefinition*> GetAllZoneDefinitions() const;

        // Get the number of registered zones.
        [[nodiscard]] u32 GetZoneCount() const;

      private:
        std::unordered_map<ZoneID, ZoneServer> m_Zones;
        std::unordered_map<u32, ZoneID> m_PlayerZoneMap; // clientID → zoneID
        std::unordered_map<u32, HandoffTransaction> m_ActiveHandoffs;
        u32 m_NextTransactionID = 1;
    };
} // namespace OloEngine
