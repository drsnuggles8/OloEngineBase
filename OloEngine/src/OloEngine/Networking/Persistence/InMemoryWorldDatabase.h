#pragma once

#include "OloEngine/Networking/Persistence/IWorldDatabase.h"
#include "OloEngine/Threading/Mutex.h"

#include <unordered_map>

namespace OloEngine
{
    // In-memory implementation of IWorldDatabase for testing and development.
    // Stores all data in hash maps — no external dependencies required.
    // For production, replace with SQLiteWorldDatabase or similar.
    class InMemoryWorldDatabase final : public IWorldDatabase
    {
      public:
        InMemoryWorldDatabase() = default;
        ~InMemoryWorldDatabase() override = default;

        // IWorldDatabase interface
        bool SavePlayerState(u32 accountID, const PlayerStatePacket& state) override;
        bool LoadPlayerState(u32 accountID, PlayerStatePacket& outState) override;
        bool DeletePlayerState(u32 accountID) override;

        bool SaveEntityState(u64 uuid, ZoneID zoneID, const std::vector<u8>& data) override;
        bool LoadEntitiesForZone(ZoneID zoneID, std::vector<std::pair<u64, std::vector<u8>>>& outEntities) override;
        bool DeleteEntityState(u64 uuid) override;

        bool SetWorldState(const std::string& key, const std::string& value) override;
        bool GetWorldState(const std::string& key, std::string& outValue) override;

        bool Initialize(const std::string& connectionString) override;
        void Shutdown() override;
        [[nodiscard]] bool IsInitialized() const override;

        // Debug/test helpers
        [[nodiscard]] u32 GetPlayerCount() const;
        [[nodiscard]] u32 GetEntityCount() const;
        void Clear();

      private:
        bool m_Initialized = false;
        mutable FMutex m_Mutex;

        std::unordered_map<u32, PlayerStatePacket> m_PlayerStates;

        struct EntityRecord
        {
            ZoneID Zone = 0;
            std::vector<u8> Data;
        };
        std::unordered_map<u64, EntityRecord> m_EntityStates;

        std::unordered_map<std::string, std::string> m_WorldState;
    };
} // namespace OloEngine
