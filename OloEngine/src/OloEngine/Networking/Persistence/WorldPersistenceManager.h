#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Persistence/IWorldDatabase.h"

#include <functional>
#include <unordered_set>

namespace OloEngine
{
    // Callback to retrieve entity data for saving. Parameters: (uuid) → (zoneID, data).
    // Returns false if the entity cannot be serialized.
    using EntityDataProvider = std::function<bool(u64 uuid, ZoneID& outZone, std::vector<u8>& outData)>;

    // Periodically saves dirty entities to the world database.
    // Entities are marked dirty when their state changes, and saved at a configurable interval.
    class WorldPersistenceManager
    {
      public:
        WorldPersistenceManager() = default;

        void Initialize(IWorldDatabase* database, f32 saveIntervalSeconds = 300.0f);
        void Shutdown();

        // Mark an entity as needing to be saved.
        void MarkDirty(u64 uuid);

        // Check if an entity is dirty.
        [[nodiscard]] bool IsDirty(u64 uuid) const;

        // Get count of dirty entities.
        [[nodiscard]] u32 GetDirtyCount() const;

        // Called each frame. Saves dirty entities when interval elapses.
        void Tick(f32 dt);

        // Force save all dirty entities immediately.
        void SaveAll();

        // Set the callback that provides entity data for saving.
        void SetEntityDataProvider(EntityDataProvider provider);

        // Save a specific entity.
        bool SaveEntity(u64 uuid, ZoneID zoneID, const std::vector<u8>& data);

        // Load entities for a zone from the database.
        bool LoadEntitiesForZone(ZoneID zoneID, std::vector<std::pair<u64, std::vector<u8>>>& outEntities);

        // Player state
        bool SavePlayer(u32 accountID, const PlayerStatePacket& state);
        bool LoadPlayer(u32 accountID, PlayerStatePacket& outState);

        // Get save interval.
        [[nodiscard]] f32 GetSaveInterval() const;

      private:
        IWorldDatabase* m_Database = nullptr;
        EntityDataProvider m_DataProvider;
        f32 m_SaveInterval = 300.0f;
        f32 m_TimeSinceLastSave = 0.0f;
        std::unordered_set<u64> m_DirtyEntities;
    };
} // namespace OloEngine
