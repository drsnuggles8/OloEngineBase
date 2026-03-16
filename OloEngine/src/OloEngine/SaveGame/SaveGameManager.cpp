#include "OloEnginePCH.h"
#include "SaveGameManager.h"

#include "OloEngine/SaveGame/SaveGameFile.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Task/Task.h"
#include "OloEngine/Task/NamedThreads.h"

#include <cctype>
#include <chrono>

namespace OloEngine
{
    // Static member definitions
    std::atomic<f32> SaveGameManager::s_AutoSaveInterval{ 0.0f };
    std::atomic<f32> SaveGameManager::s_AutoSaveTimer{ 0.0f };
    std::atomic<bool> SaveGameManager::s_Initialized{ false };
    std::atomic<u32> SaveGameManager::s_QuickSaveSlotIndex{ 0 };
    std::atomic<u32> SaveGameManager::s_AutoSaveSlotIndex{ 0 };
    std::array<std::atomic<bool>, SaveGameManager::kMaxQuickSaveSlots> SaveGameManager::s_QuickSaveInFlight{};
    std::array<std::atomic<bool>, SaveGameManager::kMaxAutoSaveSlots> SaveGameManager::s_AutoSaveInFlight{};

    // Reject slot names containing path separators, "..", reserved Windows names, or other dangerous patterns
    static bool IsValidSlotName(const std::string& slotName)
    {
        OLO_PROFILE_FUNCTION();

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
            // Reject control characters, path separators, and Windows-invalid filename characters
            if (c <= 31 || c == '/' || c == '\\' || c == ':' || c == '\0' || c == '?' || c == '*' || c == '<' || c == '>' || c == '"' || c == '|')
            {
                return false;
            }
        }

        // Reject trailing dot or space (Windows silently strips them)
        if (slotName.back() == '.' || slotName.back() == ' ')
        {
            return false;
        }

        // Extract stem (part before first dot) for reserved-name check
        std::string stem = slotName.substr(0, slotName.find('.'));
        // Case-insensitive comparison
        std::string upperStem;
        upperStem.reserve(stem.size());
        for (char ch : stem)
        {
            upperStem += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }

        // Windows reserved device names
        static constexpr std::array kReserved = {
            "CON", "PRN", "AUX", "NUL",
            "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
            "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
        };
        for (const char* reserved : kReserved)
        {
            if (upperStem == reserved)
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
        s_QuickSaveSlotIndex.store(0, std::memory_order_relaxed);
        s_AutoSaveSlotIndex.store(0, std::memory_order_relaxed);
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
                Tasks::EnqueueGameThreadTask(
                    [callback, slotName]()
                    { callback(SaveLoadResult::InvalidInput, slotName); },
                    "SaveValidationFailed");
            }
            return SaveLoadResult::InvalidInput;
        }

        // Reject manager-reserved slot prefixes
        if (slotName.starts_with("quicksave_") || slotName.starts_with("autosave_"))
        {
            OLO_CORE_ERROR("[SaveGameManager] Slot name '{}' uses a reserved prefix", slotName);
            if (callback)
            {
                Tasks::EnqueueGameThreadTask(
                    [callback, slotName]()
                    { callback(SaveLoadResult::InvalidInput, slotName); },
                    "SaveReservedPrefix");
            }
            return SaveLoadResult::InvalidInput;
        }

        std::string name = displayName.empty() ? slotName : displayName;
        return SaveAsync(scene, slotName, name, SaveSlotType::Manual, thumbnailPNG, callback);
    }

    SaveLoadResult SaveGameManager::QuickSave(Scene& scene,
                                              const std::vector<u8>& thumbnailPNG,
                                              SaveLoadCompletionCallback callback)
    {
        OLO_PROFILE_FUNCTION();

        u32 startIndex = s_QuickSaveSlotIndex.fetch_add(1, std::memory_order_relaxed);
        for (u32 attempt = 0; attempt < kMaxQuickSaveSlots; ++attempt)
        {
            u32 slotIndex = (startIndex + attempt) % kMaxQuickSaveSlots;
            bool expected = false;
            if (s_QuickSaveInFlight[slotIndex].compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            {
                std::string slotName = "quicksave_" + std::to_string(slotIndex);
                return SaveAsync(scene, slotName, "Quick Save", SaveSlotType::QuickSave, thumbnailPNG,
                                 [callback, slotIndex](SaveLoadResult result, const std::string& slot)
                                 {
                                     s_QuickSaveInFlight[slotIndex].store(false, std::memory_order_release);
                                     if (callback)
                                     {
                                         callback(result, slot);
                                     }
                                 });
            }
        }

        OLO_CORE_WARN("[SaveGameManager] All quick-save slots are in-flight, skipping");
        return SaveLoadResult::Pending;
    }

    SaveLoadResult SaveGameManager::AutoSave(Scene& scene,
                                             const std::vector<u8>& thumbnailPNG,
                                             SaveLoadCompletionCallback callback)
    {
        OLO_PROFILE_FUNCTION();

        u32 startIndex = s_AutoSaveSlotIndex.fetch_add(1, std::memory_order_relaxed);
        for (u32 attempt = 0; attempt < kMaxAutoSaveSlots; ++attempt)
        {
            u32 slotIndex = (startIndex + attempt) % kMaxAutoSaveSlots;
            bool expected = false;
            if (s_AutoSaveInFlight[slotIndex].compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            {
                std::string slotName = "autosave_" + std::to_string(slotIndex);
                return SaveAsync(scene, slotName, "Auto Save", SaveSlotType::AutoSave, thumbnailPNG,
                                 [callback, slotIndex](SaveLoadResult result, const std::string& slot)
                                 {
                                     s_AutoSaveInFlight[slotIndex].store(false, std::memory_order_release);
                                     if (callback)
                                     {
                                         callback(result, slot);
                                     }
                                 });
            }
        }

        OLO_CORE_WARN("[SaveGameManager] All auto-save slots are in-flight, skipping");
        return SaveLoadResult::Pending;
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
                if (ec)
                {
                    info.FileSizeBytes = 0;
                }
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
        if (ec)
        {
            outInfo.FileSizeBytes = 0;
        }
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
        OLO_PROFILE_FUNCTION();

        return Project::GetProjectDirectory() / "Saves";
    }

    std::filesystem::path SaveGameManager::GetSaveFilePath(const std::string& slotName)
    {
        OLO_PROFILE_FUNCTION();

        if (!IsValidSlotName(slotName))
        {
            OLO_CORE_ERROR("[SaveGameManager] GetSaveFilePath called with invalid slot name: '{}'", slotName);
            return {};
        }
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

    SaveLoadResult SaveGameManager::SaveAsync(Scene& scene,
                                              const std::string& slotName,
                                              const std::string& displayName,
                                              SaveSlotType slotType,
                                              const std::vector<u8>& thumbnailPNG,
                                              SaveLoadCompletionCallback callback)
    {
        OLO_PROFILE_FUNCTION();

        if (!IsValidSlotName(slotName))
        {
            OLO_CORE_ERROR("[SaveGameManager] SaveAsync called with invalid slot name: '{}'", slotName);
            if (callback)
            {
                Tasks::EnqueueGameThreadTask(
                    [callback, slotName]()
                    { callback(SaveLoadResult::InvalidInput, slotName); },
                    "SaveAsyncValidationFailed");
            }
            return SaveLoadResult::InvalidInput;
        }

        EnsureSaveDirectory();

        // --- Main-thread work: capture scene state to binary ---
        std::vector<u8> payload = SaveGameSerializer::CaptureSceneState(scene);
        if (payload.empty())
        {
            OLO_CORE_ERROR("[SaveGameManager] Failed to capture scene state");
            if (callback)
            {
                Tasks::EnqueueGameThreadTask(
                    [callback, slotName]()
                    { callback(SaveLoadResult::SerializationFailed, slotName); },
                    "SaveAsyncCaptureFailed");
            }
            return SaveLoadResult::SerializationFailed;
        }

        // Build metadata (needs Scene access, so must be on caller thread)
        SaveGameMetadata metadata;
        metadata.DisplayName = displayName;
        metadata.SceneName = scene.GetName();
        metadata.TimestampUTC = std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
        metadata.SlotType = slotType;
        metadata.ThumbnailAvailable = !thumbnailPNG.empty();

        auto view = scene.GetAllEntitiesWith<IDComponent>();
        u32 entityCount = static_cast<u32>(view.size());
        metadata.EntityCount = entityCount;

        auto path = GetSaveFilePath(slotName);

        // --- Dispatch compression + I/O to background thread ---
        Tasks::Launch(
            "SaveGameToDisk",
            [payload = std::move(payload),
             thumbnailPNG,
             metadata,
             entityCount,
             path,
             slotName,
             callback]() mutable
            {
                OLO_PROFILE_SCOPE("SaveGameToDisk");

                // Compress payload
                std::vector<u8> compressedPayload;
                bool compressed = SaveGameFile::Compress(payload, compressedPayload);
                if (compressed && compressedPayload.size() >= payload.size())
                {
                    compressed = false;
                }

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
                SaveLoadResult result = SaveLoadResult::Success;
                if (!SaveGameFile::Write(path, header, metadata, thumbnailPNG, writePayload))
                {
                    OLO_CORE_ERROR("[SaveGameManager] Failed to write save file: {}", path.string());
                    result = SaveLoadResult::IOError;
                }
                else
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

                // Dispatch callback back to game thread
                if (callback)
                {
                    Tasks::EnqueueGameThreadTask(
                        [callback, result, slotName]()
                        {
                            callback(result, slotName);
                        },
                        "SaveGameComplete");
                }
            });

        return SaveLoadResult::Pending;
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
            OLO_CORE_ASSERT(false, "GetRotatingSlotName called with maxSlots == 0");
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
