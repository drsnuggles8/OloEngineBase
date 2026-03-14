#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/MMO/ZoneDefinition.h"
#include "OloEngine/Networking/Replication/NetworkInterestManager.h"
#include "OloEngine/Networking/Replication/SpatialGrid.h"

#include <string>
#include <unordered_set>

namespace OloEngine
{
    class Scene;
    class InterZoneMessageBus;

    // Manages one zone: owns entity tracking, spatial grid, and interest management.
    // In a full implementation, each ZoneServer would run its own tick loop in a
    // dedicated thread with its own Scene. For now, it shares the main Scene and
    // manages a subset of entities within its bounds.
    class ZoneServer
    {
      public:
        ZoneServer() = default;

        // Initialize the zone with a definition.
        void Initialize(const ZoneDefinition& definition);

        // Start/stop the zone's simulation.
        void Start();
        void Stop();

        // Called each frame. Updates spatial grid and processes zone logic.
        void Tick(f32 dt);

        // Set the inter-zone message bus for this zone to process messages from.
        void SetMessageBus(InterZoneMessageBus* bus);

        // Player management
        bool AddPlayer(u32 clientID);
        void RemovePlayer(u32 clientID);
        [[nodiscard]] bool HasPlayer(u32 clientID) const;
        [[nodiscard]] u32 GetPlayerCount() const;
        [[nodiscard]] bool IsFull() const;

        // Mark player as transitioning (during handoff, stops full updates)
        void SetPlayerTransitioning(u32 clientID, bool transitioning);
        [[nodiscard]] bool IsPlayerTransitioning(u32 clientID) const;

        // Ghost entities — read-only copies of entities near zone boundaries
        void AddGhostEntity(u64 uuid, const glm::vec3& position);
        void RemoveGhostEntity(u64 uuid);
        [[nodiscard]] bool IsGhostEntity(u64 uuid) const;
        [[nodiscard]] u32 GetGhostEntityCount() const;

        // Query
        [[nodiscard]] bool IsRunning() const;
        [[nodiscard]] ZoneID GetZoneID() const;
        [[nodiscard]] const std::string& GetName() const;
        [[nodiscard]] const ZoneDefinition& GetDefinition() const;
        [[nodiscard]] const SpatialGrid& GetSpatialGrid() const;
        [[nodiscard]] NetworkInterestManager& GetInterestManager();

        // Update an entity's position in this zone's spatial grid.
        void UpdateEntityPosition(u64 uuid, const glm::vec3& position);

        // Remove an entity from this zone's spatial grid.
        void RemoveEntity(u64 uuid);

        // Get entity count tracked in this zone's spatial grid.
        [[nodiscard]] u32 GetEntityCount() const;

      private:
        ZoneDefinition m_Definition;
        bool m_Running = false;
        std::unordered_set<u32> m_Players;
        std::unordered_set<u32> m_TransitioningPlayers;
        std::unordered_set<u64> m_GhostEntities;
        SpatialGrid m_SpatialGrid;
        NetworkInterestManager m_InterestManager;
        InterZoneMessageBus* m_MessageBus = nullptr;
    };
} // namespace OloEngine
