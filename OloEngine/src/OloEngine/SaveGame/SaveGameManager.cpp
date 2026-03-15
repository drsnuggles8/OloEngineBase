#include "OloEnginePCH.h"
#include "SaveGameManager.h"

#include "OloEngine/SaveGame/SaveGameFile.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Task/Task.h"

#include <chrono>

namespace OloEngine
{
    // Static member definitions
    std::atomic<f32> SaveGameManager::s_AutoSaveInterval{ 0.0f };
    std::atomic<f32> SaveGameManager::s_AutoSaveTimer{ 0.0f };
    std::atomic<bool> SaveGameManager::s_Initialized{ false };

    // Reject slot names containing path separators, "..", or other dangerous patterns
    static bool IsValidSlotName(const std::string& slotName)
    {
        if (slotName.empty())
        {
            return false;
        }
        if (slotName.find("..") != std::string::npos)
        {
            return false;
        }
        for (char c : slotName)
        {
            if (c == '/' || c == '\\' || c == ':' || c == '\0')
            {
                return false;
            }
        }
        return true;
    }

    // ========================================================================
    // Initialize / Shutdown
    // ========================================================================

    void SaveGameManager::Initialize()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Initialized.load(std::memory_order_acquire))
        {
            return;
        }

        s_AutoSaveInterval.store(0.0f, std::memory_order_relaxed);
        s_AutoSaveTimer.store(0.0f, std::memory_order_relaxed);
        s_Initialized.store(true, std::memory_order_release);

        EnsureSaveDirectory();

        OLO_CORE_INFO("[SaveGameManager] Initialized. Save directory: {}", GetSaveDirectory().string());
    }

    void SaveGameManager::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        s_Initialized.store(false, std::memory_order_release);
        OLO_CORE_INFO("[SaveGameManager] Shutdown");
    }

    // ========================================================================
    // Save Operations
    // ========================================================================

    SaveLoadResult SaveGameManager::Save(Scene& scene,
                                         const std::string& slotName,
                                         const std::string& displayName,
                                         const std::vector<u8>& thumbnailPNG,
                                         SaveLoadCompletionCallback callback)
    {
        OLO_PROFILE_FUNCTION();

        if (!IsValidSlotName(slotName))
        {
            OLO_CORE_ERROR("[SaveGameManager] Invalid slot name: '{}'", slotName);
            if (callback)
            {
                callback(SaveLoadResult::InvalidInput, slotName);
            }
            return SaveLoadResult::InvalidInput;
        }

        std::string name = displayName.empty() ? slotName : displayName;
        SaveLoadResult result = SaveInternal(scene, slotName, name, SaveSlotType::Manual, thumbnailPNG);

        if (callback)
        {
            callback(result, slotName);
        }

        return result;
    }

    SaveLoadResult SaveGameManager::QuickSave(Scene& scene,
                                              const std::vector<u8>& thumbnailPNG,
                                              SaveLoadCompletionCallback callback)
    {
        OLO_PROFILE_FUNCTION();

        std::string slotName = GetRotatingSlotName("quicksave", kMaxQuickSaveSlots);
        SaveLoadResult result = SaveInternal(scene, slotName, "Quick Save", SaveSlotType::QuickSave, thumbnailPNG);

        if (callback)
        {
            callback(result, slotName);
        }

        return result;
    }

    SaveLoadResult SaveGameManager::AutoSave(Scene& scene,
                                             const std::vector<u8>& thumbnailPNG,
                                             SaveLoadCompletionCallback callback)
    {
        OLO_PROFILE_FUNCTION();

        std::string slotName = GetRotatingSlotName("autosave", kMaxAutoSaveSlots);
        SaveLoadResult result = SaveInternal(scene, slotName, "Auto Save", SaveSlotType::AutoSave, thumbnailPNG);

        if (callback)
        {
            callback(result, slotName);
        }

        return result;
    }

    // ========================================================================
    // Load Operations
    // ========================================================================

    SaveLoadResult SaveGameManager::Load(Scene& scene, const std::string& slotName)
    {
        OLO_PROFILE_FUNCTION();

        if (!IsValidSlotName(slotName))
        {
            OLO_CORE_ERROR("[SaveGameManager] Invalid slot name: '{}'", slotName);
            return SaveLoadResult::InvalidInput;
        }

        auto path = GetSaveFilePath(slotName);
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_ERROR("[SaveGameManager] Save file not found: {}", path.string());
            return SaveLoadResult::FileNotFound;
        }

        // Validate checksum first
        if (!SaveGameFile::ValidateChecksum(path))
        {
            OLO_CORE_ERROR("[SaveGameManager] Checksum validation failed: {}", path.string());
            return SaveLoadResult::ChecksumMismatch;
        }

        // Read payload
        std::vector<u8> payload;
        if (!SaveGameFile::ReadPayload(path, payload))
        {
            OLO_CORE_ERROR("[SaveGameManager] Failed to read/decompress payload: {}", path.string());
            return SaveLoadResult::IOError;
        }

        // Restore scene state
        if (!SaveGameSerializer::RestoreSceneState(scene, payload))
        {
            OLO_CORE_ERROR("[SaveGameManager] Failed to restore scene state from: {}", path.string());
            return SaveLoadResult::SerializationFailed;
        }

        OLO_CORE_INFO("[SaveGameManager] Loaded save: {}", slotName);
        return SaveLoadResult::Success;
    }

    SaveLoadResult SaveGameManager::QuickLoad(Scene& scene)
    {
        OLO_PROFILE_FUNCTION();

        // Find the most recent quick-save
        std::string bestSlot;
        i64 bestTimestamp = 0;

        for (u32 i = 0; i < kMaxQuickSaveSlots; ++i)
        {
            std::string slotName = "quicksave_" + std::to_string(i);
            SaveFileInfo info;
            if (GetSaveInfo(slotName, info) && info.Metadata.TimestampUTC > bestTimestamp)
            {
                if (!ValidateSave(slotName))
                {
                    OLO_CORE_WARN("[SaveGameManager] Skipping corrupt quick-save: {}", slotName);
                    continue;
                }
                bestTimestamp = info.Metadata.TimestampUTC;
                bestSlot = slotName;
            }
        }

        if (bestSlot.empty())
        {
            OLO_CORE_WARN("[SaveGameManager] No quick-save found");
            return SaveLoadResult::FileNotFound;
        }

        return Load(scene, bestSlot);
    }

    // ========================================================================
    // Enumeration
    // ========================================================================

    std::vector<SaveFileInfo> SaveGameManager::EnumerateSaves()
    {
        OLO_PROFILE_FUNCTION();

        std::vector<SaveFileInfo> saves;
        auto saveDir = GetSaveDirectory();

        if (!std::filesystem::exists(saveDir))
        {
            return saves;
        }

        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(saveDir, ec))
        {
            if (!entry.is_regular_file(ec))
            {
                continue;
            }

            if (entry.path().extension() != kSaveFileExtension)
            {
                continue;
            }

            SaveGameHeader header;
            SaveGameMetadata metadata;
            if (SaveGameFile::ReadMetadata(entry.path(), header, metadata))
            {
                SaveFileInfo info;
                info.FilePath = entry.path();
                info.Metadata = metadata;
                info.FileSizeBytes = entry.file_size(ec);
                info.HasThumbnail = metadata.ThumbnailAvailable;
                saves.push_back(std::move(info));
            }
        }

        // Sort by timestamp, most recent first
        std::sort(saves.begin(), saves.end(),
                  [](const SaveFileInfo& a, const SaveFileInfo& b)
                  {
                      return a.Metadata.TimestampUTC > b.Metadata.TimestampUTC;
                  });

        return saves;
    }

    bool SaveGameManager::GetSaveInfo(const std::string& slotName, SaveFileInfo& outInfo)
    {
        OLO_PROFILE_FUNCTION();

        if (!IsValidSlotName(slotName))
        {
            return false;
        }

        auto path = GetSaveFilePath(slotName);
        if (!std::filesystem::exists(path))
        {
            return false;
        }

        SaveGameHeader header;
        if (!SaveGameFile::ReadMetadata(path, header, outInfo.Metadata))
        {
            return false;
        }

        std::error_code ec;
        outInfo.FilePath = path;
        outInfo.FileSizeBytes = std::filesystem::file_size(path, ec);
        outInfo.HasThumbnail = outInfo.Metadata.ThumbnailAvailable;
        return true;
    }

    bool SaveGameManager::ReadThumbnail(const std::string& slotName, std::vector<u8>& outPNG)
    {
        OLO_PROFILE_FUNCTION();

        if (!IsValidSlotName(slotName))
        {
            return false;
        }

        auto path = GetSaveFilePath(slotName);
        return SaveGameFile::ReadThumbnail(path, outPNG);
    }

    // ========================================================================
    // Deletion
    // ========================================================================

    bool SaveGameManager::DeleteSave(const std::string& slotName)
    {
        OLO_PROFILE_FUNCTION();

        if (!IsValidSlotName(slotName))
        {
            OLO_CORE_ERROR("[SaveGameManager] Invalid slot name: '{}'", slotName);
            return false;
        }

        auto path = GetSaveFilePath(slotName);
        if (!std::filesystem::exists(path))
        {
            return false;
        }

        std::error_code ec;
        bool removed = std::filesystem::remove(path, ec);
        if (removed)
        {
            OLO_CORE_INFO("[SaveGameManager] Deleted save: {}", slotName);
        }
        return removed;
    }

    // ========================================================================
    // Auto-Save
    // ========================================================================

    void SaveGameManager::SetAutoSaveInterval(f32 intervalSeconds)
    {
        OLO_PROFILE_FUNCTION();
        s_AutoSaveInterval.store(std::max(0.0f, intervalSeconds), std::memory_order_relaxed);
        s_AutoSaveTimer.store(0.0f, std::memory_order_relaxed);
    }

    f32 SaveGameManager::GetAutoSaveInterval()
    {
        OLO_PROFILE_FUNCTION();
        return s_AutoSaveInterval.load(std::memory_order_relaxed);
    }

    void SaveGameManager::Tick(f32 deltaTime, Scene& scene)
    {
        OLO_PROFILE_FUNCTION();

        const f32 interval = s_AutoSaveInterval.load(std::memory_order_relaxed);
        if (interval <= 0.0f)
        {
            return;
        }

        f32 timer = s_AutoSaveTimer.load(std::memory_order_relaxed) + deltaTime;
        if (timer >= interval)
        {
            timer = 0.0f;
            AutoSave(scene);
        }
        s_AutoSaveTimer.store(timer, std::memory_order_relaxed);
    }

    // ========================================================================
    // Utility
    // ========================================================================

    std::filesystem::path SaveGameManager::GetSaveDirectory()
    {
        return Project::GetProjectDirectory() / "Saves";
    }

    std::filesystem::path SaveGameManager::GetSaveFilePath(const std::string& slotName)
    {
        OLO_CORE_ASSERT(IsValidSlotName(slotName), "GetSaveFilePath called with invalid slot name");
        return GetSaveDirectory() / (slotName + std::string(kSaveFileExtension));
    }

    bool SaveGameManager::ValidateSave(const std::string& slotName)
    {
        OLO_PROFILE_FUNCTION();

        if (!IsValidSlotName(slotName))
        {
            return false;
        }

        auto path = GetSaveFilePath(slotName);
        return SaveGameFile::ValidateChecksum(path);
    }

    // ========================================================================
    // Internal Implementation
    // ========================================================================

    SaveLoadResult SaveGameManager::SaveInternal(Scene& scene,
                                                 const std::string& slotName,
                                                 const std::string& displayName,
                                                 SaveSlotType slotType,
                                                 const std::vector<u8>& thumbnailPNG)
    {
        OLO_PROFILE_FUNCTION();

        if (!IsValidSlotName(slotName))
        {
            OLO_CORE_ERROR("[SaveGameManager] SaveInternal called with invalid slot name: '{}'", slotName);
            return SaveLoadResult::InvalidInput;
        }

        EnsureSaveDirectory();

        // Capture scene state to binary
        std::vector<u8> payload = SaveGameSerializer::CaptureSceneState(scene);
        if (payload.empty())
        {
            OLO_CORE_ERROR("[SaveGameManager] Failed to capture scene state");
            return SaveLoadResult::SerializationFailed;
        }

        // Compress payload
        std::vector<u8> compressedPayload;
        bool compressed = SaveGameFile::Compress(payload, compressedPayload);

        // Only use compression if it actually reduces size
        if (compressed && compressedPayload.size() >= payload.size())
        {
            compressed = false;
        }

        // Build metadata
        SaveGameMetadata metadata;
        metadata.DisplayName = displayName;
        metadata.SceneName = scene.GetName();
        metadata.TimestampUTC = std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
        metadata.SlotType = slotType;
        metadata.ThumbnailAvailable = !thumbnailPNG.empty();

        // Count entities
        u32 entityCount = 0;
        auto view = scene.GetAllEntitiesWith<IDComponent>();
        for ([[maybe_unused]] auto e : view)
        {
            ++entityCount;
        }
        metadata.EntityCount = entityCount;

        // Build header
        SaveGameHeader header;
        header.EntityCount = entityCount;
        if (compressed)
        {
            header.SetCompression(SaveGameCompression::Zlib);
            header.PayloadUncompressedSize = payload.size();
        }

        // Write to disk
        const auto& writePayload = compressed ? compressedPayload : payload;
        auto path = GetSaveFilePath(slotName);
        if (!SaveGameFile::Write(path, header, metadata, thumbnailPNG, writePayload))
        {
            OLO_CORE_ERROR("[SaveGameManager] Failed to write save file: {}", path.string());
            return SaveLoadResult::IOError;
        }

        {
            std::error_code ec;
            auto fileSize = std::filesystem::file_size(path, ec);
            if (ec)
            {
                OLO_CORE_WARN("[SaveGameManager] Could not read file size for '{}': {}", path.string(), ec.message());
            }
            OLO_CORE_INFO("[SaveGameManager] Saved '{}' ({} entities, {:.1f} KB)",
                          slotName, entityCount,
                          ec ? 0.0f : static_cast<f32>(fileSize) / 1024.0f);
        }

        return SaveLoadResult::Success;
    }

    void SaveGameManager::EnsureSaveDirectory()
    {
        OLO_PROFILE_FUNCTION();

        auto saveDir = GetSaveDirectory();
        if (!std::filesystem::exists(saveDir))
        {
            std::error_code ec;
            std::filesystem::create_directories(saveDir, ec);
            if (ec)
            {
                OLO_CORE_ERROR("[SaveGameManager] Failed to create save directory '{}': {}", saveDir.string(), ec.message());
            }
        }
    }

    std::string SaveGameManager::GetRotatingSlotName(const std::string& prefix, u32 maxSlots)
    {
        OLO_PROFILE_FUNCTION();

        if (maxSlots == 0)
        {
            OLO_CORE_ERROR("[SaveGameManager] GetRotatingSlotName called with maxSlots == 0");
            return {};
        }

        // Find the oldest slot to overwrite
        i64 oldestTimestamp = std::numeric_limits<i64>::max();
        std::string oldestSlot;
        std::string emptySlot;

        for (u32 i = 0; i < maxSlots; ++i)
        {
            std::string slotName = prefix + "_" + std::to_string(i);
            SaveFileInfo info;
            if (!GetSaveInfo(slotName, info))
            {
                // Slot doesn't exist yet — use it
                if (emptySlot.empty())
                {
                    emptySlot = slotName;
                }
                continue;
            }

            if (info.Metadata.TimestampUTC < oldestTimestamp)
            {
                oldestTimestamp = info.Metadata.TimestampUTC;
                oldestSlot = slotName;
            }
        }

        // Prefer empty slot, then oldest
        return emptySlot.empty() ? oldestSlot : emptySlot;
    }

} // namespace OloEngine
