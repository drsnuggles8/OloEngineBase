#pragma once

#include "OloEngine/Networking/Persistence/IWorldDatabase.h"
#include "OloEngine/Threading/Mutex.h"

#include <filesystem>
#include <unordered_map>

namespace OloEngine
{
    // IWorldDatabase implementation backed by .olosave files on disk.
    // Bridges the single-player SaveGame system into the multiplayer
    // WorldPersistenceManager, allowing the same persistence API for
    // both standalone and server-hosted scenarios.
    //
    // Initialize() takes a slot name (e.g. "world_save_0") as connectionString.
    // Data is held in memory and flushed to disk on Shutdown() or FlushToDisk().
    class SaveFileWorldDatabase final : public IWorldDatabase
    {
      public:
        SaveFileWorldDatabase() = default;
        ~SaveFileWorldDatabase() override;

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

        // Flush current in-memory state to a .olosave file on disk.
        bool FlushToDisk();

      private:
        // Serialize all in-memory data to a binary payload
        [[nodiscard]] std::vector<u8> SerializeToPayload() const;

        // Deserialize a binary payload into in-memory data
        bool DeserializeFromPayload(const std::vector<u8>& payload);

        bool m_Initialized = false;
        bool m_Dirty = false;
        std::string m_SlotName;
        std::filesystem::path m_FilePath;
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
