#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/MMO/PlayerStatePacket.h"
#include "OloEngine/Networking/MMO/ZoneDefinition.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace OloEngine
{
    // Abstract database interface for persistent world state.
    // Implementations can use SQLite (local dev), PostgreSQL (production), etc.
    class IWorldDatabase
    {
      public:
        virtual ~IWorldDatabase() = default;

        // Player state persistence
        virtual bool SavePlayerState(u32 accountID, const PlayerStatePacket& state) = 0;
        virtual bool LoadPlayerState(u32 accountID, PlayerStatePacket& outState) = 0;
        virtual bool DeletePlayerState(u32 accountID) = 0;

        // Entity state persistence (NPCs, world objects, etc.)
        virtual bool SaveEntityState(u64 uuid, ZoneID zoneID, const std::vector<u8>& data) = 0;
        virtual bool LoadEntitiesForZone(ZoneID zoneID, std::vector<std::pair<u64, std::vector<u8>>>& outEntities) = 0;
        virtual bool DeleteEntityState(u64 uuid) = 0;

        // Key-value world state
        virtual bool SetWorldState(const std::string& key, const std::string& value) = 0;
        virtual bool GetWorldState(const std::string& key, std::string& outValue) = 0;

        // Lifecycle
        virtual bool Initialize(const std::string& connectionString) = 0;
        virtual void Shutdown() = 0;
        [[nodiscard]] virtual bool IsInitialized() const = 0;
    };
} // namespace OloEngine
