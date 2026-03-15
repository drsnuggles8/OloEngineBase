#include "OloEnginePCH.h"
#include "SaveFileWorldDatabase.h"

#include "OloEngine/SaveGame/SaveGameFile.h"
#include "OloEngine/SaveGame/SaveGameManager.h"
#include "OloEngine/SaveGame/SaveGameTypes.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"
#include "OloEngine/Threading/UniqueLock.h"

#include <chrono>

namespace OloEngine
{
    SaveFileWorldDatabase::~SaveFileWorldDatabase()
    {
        if (m_Initialized)
        {
            Shutdown();
        }
    }

    // ========================================================================
    // Lifecycle
    // ========================================================================

    bool SaveFileWorldDatabase::Initialize(const std::string& connectionString)
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        if (m_Initialized)
        {
            return false;
        }

        m_SlotName = connectionString;
        m_FilePath = SaveGameManager::GetSaveFilePath(m_SlotName);

        // Attempt to load existing data from disk
        if (std::filesystem::exists(m_FilePath))
        {
            std::vector<u8> payload;
            if (SaveGameFile::ReadPayload(m_FilePath, payload) && !payload.empty())
            {
                if (!DeserializeFromPayload(payload))
                {
                    OLO_CORE_WARN("[SaveFileWorldDatabase] Failed to deserialize existing save '{}', starting fresh", m_SlotName);
                    m_PlayerStates.clear();
                    m_EntityStates.clear();
                    m_WorldState.clear();
                }
                else
                {
                    OLO_CORE_INFO("[SaveFileWorldDatabase] Loaded existing data from '{}'", m_SlotName);
                }
            }
        }

        m_Initialized = true;
        m_Dirty = false;
        OLO_CORE_INFO("[SaveFileWorldDatabase] Initialized with slot '{}'", m_SlotName);
        return true;
    }

    void SaveFileWorldDatabase::Shutdown()
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        if (!m_Initialized)
        {
            return;
        }

        // Flush while still initialized so FlushToDiskLocked can serialize
        if (m_Dirty)
        {
            FlushToDiskLocked();
        }

        m_Initialized = false;
        m_PlayerStates.clear();
        m_EntityStates.clear();
        m_WorldState.clear();
        OLO_CORE_INFO("[SaveFileWorldDatabase] Shut down");
    }

    bool SaveFileWorldDatabase::IsInitialized() const
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        return m_Initialized;
    }

    // ========================================================================
    // Player State
    // ========================================================================

    bool SaveFileWorldDatabase::SavePlayerState(u32 accountID, const PlayerStatePacket& state)
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        if (!m_Initialized)
        {
            return false;
        }
        m_PlayerStates[accountID] = state;
        m_Dirty = true;
        return true;
    }

    bool SaveFileWorldDatabase::LoadPlayerState(u32 accountID, PlayerStatePacket& outState)
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        if (!m_Initialized)
        {
            return false;
        }
        auto it = m_PlayerStates.find(accountID);
        if (it == m_PlayerStates.end())
        {
            return false;
        }
        outState = it->second;
        return true;
    }

    bool SaveFileWorldDatabase::DeletePlayerState(u32 accountID)
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        if (!m_Initialized)
        {
            return false;
        }
        bool erased = m_PlayerStates.erase(accountID) > 0;
        if (erased)
        {
            m_Dirty = true;
        }
        return erased;
    }

    // ========================================================================
    // Entity State
    // ========================================================================

    bool SaveFileWorldDatabase::SaveEntityState(u64 uuid, ZoneID zoneID, const std::vector<u8>& data)
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        if (!m_Initialized)
        {
            return false;
        }
        m_EntityStates[uuid] = { zoneID, data };
        m_Dirty = true;
        return true;
    }

    bool SaveFileWorldDatabase::LoadEntitiesForZone(ZoneID zoneID, std::vector<std::pair<u64, std::vector<u8>>>& outEntities)
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        if (!m_Initialized)
        {
            return false;
        }
        outEntities.clear();
        for (auto const& [uuid, record] : m_EntityStates)
        {
            if (record.Zone == zoneID)
            {
                outEntities.emplace_back(uuid, record.Data);
            }
        }
        return true;
    }

    bool SaveFileWorldDatabase::DeleteEntityState(u64 uuid)
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        if (!m_Initialized)
        {
            return false;
        }
        bool erased = m_EntityStates.erase(uuid) > 0;
        if (erased)
        {
            m_Dirty = true;
        }
        return erased;
    }

    // ========================================================================
    // World State (key-value)
    // ========================================================================

    bool SaveFileWorldDatabase::SetWorldState(const std::string& key, const std::string& value)
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        if (!m_Initialized)
        {
            return false;
        }
        m_WorldState[key] = value;
        m_Dirty = true;
        return true;
    }

    bool SaveFileWorldDatabase::GetWorldState(const std::string& key, std::string& outValue)
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        if (!m_Initialized)
        {
            return false;
        }
        auto it = m_WorldState.find(key);
        if (it == m_WorldState.end())
        {
            return false;
        }
        outValue = it->second;
        return true;
    }

    // ========================================================================
    // Disk Flush
    // ========================================================================

    bool SaveFileWorldDatabase::FlushToDisk()
    {
        OLO_PROFILE_FUNCTION();

        TUniqueLock<FMutex> lock(m_Mutex);
        if (!m_Initialized)
        {
            return false;
        }

        return FlushToDiskLocked();
    }

    bool SaveFileWorldDatabase::FlushToDiskLocked()
    {
        auto payload = SerializeToPayload();

        // Compress
        std::vector<u8> compressed;
        if (!SaveGameFile::Compress(payload, compressed))
        {
            OLO_CORE_ERROR("[SaveFileWorldDatabase] Compression failed");
            return false;
        }

        // Build metadata
        SaveGameMetadata metadata;
        metadata.DisplayName = m_SlotName;
        metadata.SceneName = "WorldDatabase";
        metadata.TimestampUTC = std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
        metadata.SlotType = SaveSlotType::Manual;
        metadata.EntityCount = static_cast<u32>(m_EntityStates.size());

        // Build header
        SaveGameHeader header;
        header.EntityCount = metadata.EntityCount;
        header.SetCompression(SaveGameCompression::Zlib);
        header.PayloadUncompressedSize = payload.size();

        // Ensure directory exists
        std::error_code ec;
        std::filesystem::create_directories(m_FilePath.parent_path(), ec);

        if (!SaveGameFile::Write(m_FilePath, header, metadata, {}, compressed))
        {
            OLO_CORE_ERROR("[SaveFileWorldDatabase] Failed to write '{}'", m_FilePath.string());
            return false;
        }

        m_Dirty = false;
        OLO_CORE_TRACE("[SaveFileWorldDatabase] Flushed to disk: {} players, {} entities, {} world keys",
                       m_PlayerStates.size(), m_EntityStates.size(), m_WorldState.size());
        return true;
    }

    // ========================================================================
    // Serialization
    // ========================================================================

    std::vector<u8> SaveFileWorldDatabase::SerializeToPayload() const
    {
        std::vector<u8> buffer;
        FMemoryWriter ar(buffer);

        // Section marker
        u32 worldDbMagic = 0x57444200; // "WDB\0"
        ar << worldDbMagic;

        // Player states
        u32 playerCount = static_cast<u32>(m_PlayerStates.size());
        ar << playerCount;
        for (auto const& [accountID, state] : m_PlayerStates)
        {
            u32 id = accountID;
            ar << id;
            auto blob = state.Serialize();
            ar << blob;
        }

        // Entity states
        u32 entityCount = static_cast<u32>(m_EntityStates.size());
        ar << entityCount;
        for (auto const& [uuid, record] : m_EntityStates)
        {
            u64 id = uuid;
            ZoneID zone = record.Zone;
            auto data = record.Data;
            ar << id;
            ar << zone;
            ar << data;
        }

        // World state key-values
        u32 worldCount = static_cast<u32>(m_WorldState.size());
        ar << worldCount;
        for (auto const& [key, value] : m_WorldState)
        {
            auto k = key;
            auto v = value;
            ar << k;
            ar << v;
        }

        return buffer;
    }

    bool SaveFileWorldDatabase::DeserializeFromPayload(const std::vector<u8>& payload)
    {
        static constexpr u32 kMaxPlayers = 100000;
        static constexpr u32 kMaxEntities = 1000000;
        static constexpr u32 kMaxWorldKeys = 1000000;

        auto dataCopy = payload;
        FMemoryReader ar(dataCopy);

        // Verify marker
        u32 magic = 0;
        ar << magic;
        constexpr u32 kWorldDbMagic = 0x57444200;
        if (ar.IsError() || magic != kWorldDbMagic)
        {
            OLO_CORE_ERROR("[SaveFileWorldDatabase] Invalid world database marker");
            return false;
        }

        // Player states
        u32 playerCount = 0;
        ar << playerCount;
        if (ar.IsError() || playerCount > kMaxPlayers)
        {
            OLO_CORE_ERROR("[SaveFileWorldDatabase] Invalid player count: {}", playerCount);
            return false;
        }
        m_PlayerStates.clear();
        m_PlayerStates.reserve(playerCount);
        for (u32 i = 0; i < playerCount; ++i)
        {
            u32 accountID = 0;
            ar << accountID;

            std::vector<u8> blob;
            ar << blob;

            if (ar.IsError())
            {
                OLO_CORE_ERROR("[SaveFileWorldDatabase] Read error at player {}", i);
                return false;
            }

            if (blob.empty())
            {
                OLO_CORE_ERROR("[SaveFileWorldDatabase] Empty player state blob for account {}", accountID);
                return false;
            }

            auto state = PlayerStatePacket::Deserialize(blob.data(), static_cast<i64>(blob.size()));
            if (!state)
            {
                OLO_CORE_ERROR("[SaveFileWorldDatabase] Failed to deserialize player state for account {}", accountID);
                return false;
            }
            m_PlayerStates[accountID] = std::move(*state);
        }

        // Entity states
        u32 entityCount = 0;
        ar << entityCount;
        if (ar.IsError() || entityCount > kMaxEntities)
        {
            OLO_CORE_ERROR("[SaveFileWorldDatabase] Invalid entity count: {}", entityCount);
            return false;
        }
        m_EntityStates.clear();
        m_EntityStates.reserve(entityCount);
        for (u32 i = 0; i < entityCount; ++i)
        {
            u64 uuid = 0;
            ZoneID zone = 0;
            std::vector<u8> data;
            ar << uuid;
            ar << zone;
            ar << data;
            if (ar.IsError())
            {
                OLO_CORE_ERROR("[SaveFileWorldDatabase] Read error at entity {}", i);
                return false;
            }
            m_EntityStates[uuid] = { zone, std::move(data) };
        }

        // World state key-values
        u32 worldCount = 0;
        ar << worldCount;
        if (ar.IsError() || worldCount > kMaxWorldKeys)
        {
            OLO_CORE_ERROR("[SaveFileWorldDatabase] Invalid world state count: {}", worldCount);
            return false;
        }
        m_WorldState.clear();
        m_WorldState.reserve(worldCount);
        for (u32 i = 0; i < worldCount; ++i)
        {
            std::string key;
            std::string value;
            ar << key;
            ar << value;
            if (ar.IsError())
            {
                OLO_CORE_ERROR("[SaveFileWorldDatabase] Read error at world state {}", i);
                return false;
            }
            m_WorldState[std::move(key)] = std::move(value);
        }

        return true;
    }

} // namespace OloEngine
