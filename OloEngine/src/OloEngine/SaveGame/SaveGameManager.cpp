#include "OloEnginePCH.h"
#include "SaveGameManager.h"

#include "OloEngine/SaveGame/SaveGameFile.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Task/Task.h"
#include "OloEngine/Task/NamedThreads.h"
#include "Platform/Steam/SteamManager.h"

#include <cctype>
#include <chrono>
#include <fstream>
#include <mutex>
#include <span>
#include <unordered_map>

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

    // --- Steam Cloud mirror (#644) --------------------------------------------------------
    //
    // The local atomic-rename write in SaveGameFile stays EXACTLY as it is and remains the source
    // of truth; the cloud copy is a mirror pushed afterwards. Local-first is deliberate: Steam
    // Cloud is unavailable for a large fraction of sessions (offline, Cloud switched off
    // account-wide or per-app, no Steam at all), and a save must never depend on it.
    //
    // Every function here is a no-op when Steam or Cloud is unavailable, so the SaveGame system
    // behaves identically on a machine that has never had Steam.
    namespace
    {
        // Steam Cloud is a FLAT namespace — no directories — so the on-disk
        // "<project>/Saves/<slot>.olosave" becomes just "<slot>.olosave" in the cloud.
        [[nodiscard]] std::string CloudNameForSlot(const std::string& slotName)
        {
            return slotName + std::string(SaveGameManager::kSaveFileExtension);
        }

        [[nodiscard]] std::string SlotForCloudName(const std::string& cloudName)
        {
            const std::string ext{ SaveGameManager::kSaveFileExtension };
            if (cloudName.size() > ext.size() && cloudName.ends_with(ext))
            {
                return cloudName.substr(0, cloudName.size() - ext.size());
            }
            return {};
        }

        // Per-slot generation counter for cloud mirroring (#644).
        //
        // Saves complete on WORKER threads, and two saves to the same slot can finish out of
        // order — the general Save() has no per-slot in-flight guard (only the quick/auto rotating
        // slots do). Without a guard the older worker's mirror task could run last and overwrite
        // the newer save in the cloud, leaving cloud and local disagreeing with no error anywhere.
        //
        // Each dispatch takes a ticket; the mirror task uploads only if its ticket is still the
        // latest for that slot. A superseded task drops its bytes silently, which is correct: the
        // newer save's own mirror task is already queued behind it.
        //
        // Guards the CLOUD side only. Local writes are unaffected — they were already
        // last-writer-wins through SaveGameFile's atomic rename, which is pre-existing behaviour
        // and not something this change should quietly alter.
        std::mutex s_CloudGenerationMutex;
        std::unordered_map<std::string, u64> s_CloudGeneration;

        [[nodiscard]] bool IsLatestCloudGeneration(const std::string& slotName, u64 generation)
        {
            const std::lock_guard lock(s_CloudGenerationMutex);
            const auto found = s_CloudGeneration.find(slotName);
            return found != s_CloudGeneration.end() && found->second == generation;
        }

        [[nodiscard]] bool ReadWholeFile(const std::filesystem::path& path, std::vector<u8>& outBytes)
        {
            std::ifstream in(path, std::ios::binary | std::ios::ate);
            if (!in)
            {
                return false;
            }
            const std::streamoff size = in.tellg();
            if (size <= 0)
            {
                return false;
            }
            in.seekg(0, std::ios::beg);
            outBytes.resize(static_cast<sizet>(size));
            return static_cast<bool>(in.read(reinterpret_cast<char*>(outBytes.data()), size));
        }

        // Read the just-written save AND take its cloud ticket as ONE atomic step.
        //
        // Doing these separately reintroduces the very reordering the ticket exists to prevent,
        // because the ticket order then need not match the byte order:
        //
        //   worker A  writes v1, reads v1
        //   worker B  writes v2, reads v2, takes ticket 1
        //   worker A                        takes ticket 2   <-- A now "wins" carrying v1
        //
        // A's older bytes hold the higher ticket, so the newest upload is the stale one and cloud
        // silently disagrees with local — exactly the failure the generation counter was added for.
        // Holding the lock across both makes "read last" and "ticket highest" the same event.
        //
        // The lock spans a file read, which serialises concurrent mirrors across slots too. That
        // is accepted: saves are infrequent, this is a worker thread and not frame-critical, and a
        // per-slot lock would buy contention we have no evidence of needing.
        [[nodiscard]] bool ReadSaveAndTakeCloudTicket(const std::filesystem::path& path, const std::string& slotName,
                                                      std::vector<u8>& outBytes, u64& outGeneration)
        {
            const std::lock_guard lock(s_CloudGenerationMutex);
            if (!ReadWholeFile(path, outBytes))
            {
                return false;
            }
            outGeneration = ++s_CloudGeneration[slotName];
            return true;
        }

        // MUST run on the game thread — SteamManager is game-thread-only. The save write finishes
        // on a WORKER thread, so the caller there hops via EnqueueGameThreadTask rather than
        // calling this directly.
        //
        // Takes the bytes ALREADY READ rather than a path, deliberately. Re-reading the file here
        // would put save-sized disk I/O on the frame thread for no reason: the worker that just
        // wrote the file is a perfectly good place to read it back, and it is not frame-critical.
        // Only the Steam call itself is forced onto this thread.
        //
        // Remaining caveat, stated rather than hidden: ISteamRemoteStorage::FileWrite is itself a
        // synchronous call on the game thread. It was not observably slow against App ID 480, but
        // that quota is 4096 bytes, so this is NOT evidence about realistic save sizes. If a game
        // with real saves sees a hitch here, the fix is an async backend write, not moving the
        // call off-thread — Steamworks would not tolerate that.
        void MirrorSaveToCloud(std::vector<u8> bytes, const std::string& slotName, u64 generation)
        {
            if (!SteamManager::IsCloudEnabled() || bytes.empty())
            {
                return;
            }

            // A newer save for this slot was dispatched while this task waited. Uploading now
            // would push the OLDER bytes over the newer ones, so drop them — the newer save's own
            // mirror task is already queued and will carry the current state.
            if (!IsLatestCloudGeneration(slotName, generation))
            {
                OLO_CORE_TRACE("[SaveGameManager] Skipping a superseded Steam Cloud mirror of '{}'.", slotName);
                return;
            }

            const SteamResult result = SteamManager::CloudWrite(CloudNameForSlot(slotName), std::span<const u8>{ bytes });
            if (SteamSucceeded(result))
            {
                OLO_CORE_TRACE("[SaveGameManager] Mirrored '{}' to Steam Cloud ({} bytes).", slotName, bytes.size());
            }
            else
            {
                // Not an error for the player: the local save already succeeded. Worth a warning
                // because the usual cause is an exhausted cloud quota, which is silent otherwise.
                OLO_CORE_WARN("[SaveGameManager] Steam Cloud mirror of '{}' failed ({}). The local save is intact.",
                              slotName, SteamResultToString(result));
            }
        }

        // Pull a cloud-only save down to disk so the normal local load path can handle it
        // unchanged. Returns true when the file now exists locally.
        [[nodiscard]] bool RestoreSaveFromCloud(const std::filesystem::path& path, const std::string& slotName)
        {
            if (!SteamManager::IsCloudEnabled())
            {
                return false;
            }

            const std::string cloudName = CloudNameForSlot(slotName);
            if (!SteamManager::CloudExists(cloudName))
            {
                return false;
            }

            std::vector<u8> bytes;
            if (!SteamSucceeded(SteamManager::CloudRead(cloudName, bytes)) || bytes.empty())
            {
                return false;
            }

            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);

            // Write via a temp file + rename, matching SaveGameFile's own atomicity: a process
            // killed mid-restore must not leave a half-written save that then fails its checksum.
            const std::filesystem::path temp = path.string() + ".cloudtmp";
            {
                std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                if (!out || !out.write(reinterpret_cast<const char*>(bytes.data()),
                                       static_cast<std::streamsize>(bytes.size())))
                {
                    std::filesystem::remove(temp, ec);
                    return false;
                }

                // Close EXPLICITLY and check the result. Letting the destructor do it discards
                // any error from the final flush, so a disk-full or I/O failure at that moment
                // would leave a truncated temp file that the rename below then promotes to the
                // real save — defeating the entire point of writing via temp+rename.
                out.close();
                if (!out)
                {
                    OLO_CORE_WARN("[SaveGameManager] Failed to flush the Steam Cloud restore of '{}'; "
                                  "discarding the partial file.",
                                  slotName);
                    std::filesystem::remove(temp, ec);
                    return false;
                }
            }

            // VALIDATE BEFORE THE RENAME. Cloud bytes are untrusted — a partially-synced file, a
            // save from a newer build, or plain corruption all arrive looking like a normal blob.
            //
            // Validating here rather than after is what keeps a bad copy from being permanently
            // stuck: SaveGameManager::Load tests `exists(path)` FIRST and only falls back to
            // cloud when there is no local file. So a corrupt blob renamed into place would be
            // found by every later Load, fail its checksum, and return ChecksumMismatch forever —
            // shadowing a cloud copy that might since have finished syncing and become good. By
            // failing here the local path stays empty and the fallback remains available to retry.
            //
            // The same two checks Load applies, in the same order.
            if (!SaveGameFile::ValidateChecksum(temp))
            {
                OLO_CORE_WARN("[SaveGameManager] Steam Cloud copy of '{}' failed checksum validation; discarding it "
                              "rather than letting it shadow the cloud copy on future loads.",
                              slotName);
                std::filesystem::remove(temp, ec);
                return false;
            }

            SaveGameHeader header;
            SaveGameMetadata metadata;
            if (!SaveGameFile::ReadMetadata(temp, header, metadata))
            {
                OLO_CORE_WARN("[SaveGameManager] Steam Cloud copy of '{}' has an unreadable header; discarding it.",
                              slotName);
                std::filesystem::remove(temp, ec);
                return false;
            }

            std::filesystem::rename(temp, path, ec);
            if (ec)
            {
                std::filesystem::remove(temp, ec);
                return false;
            }

            OLO_CORE_INFO("[SaveGameManager] Restored '{}' from Steam Cloud ({} bytes).", slotName, bytes.size());
            return true;
        }
    } // namespace

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

        if (!s_Initialized.exchange(false, std::memory_order_acq_rel))
        {
            return;
        }

        // Wait for all in-flight save workers to finish so no callbacks are
        // enqueued to the game thread after Shutdown returns.
        auto allDrained = []()
        {
            for (const auto& flag : s_QuickSaveInFlight)
            {
                if (flag.load(std::memory_order_acquire))
                {
                    return false;
                }
            }
            for (const auto& flag : s_AutoSaveInFlight)
            {
                if (flag.load(std::memory_order_acquire))
                {
                    return false;
                }
            }
            return true;
        };

        while (!allDrained())
        {
            std::this_thread::yield();
        }

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

        // Reject manager-reserved slot prefixes (case-insensitive)
        std::string lowerSlot;
        lowerSlot.reserve(slotName.size());
        for (char ch : slotName)
        {
            lowerSlot += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (lowerSlot.starts_with("quicksave_") || lowerSlot.starts_with("autosave_"))
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
                auto result = SaveAsync(scene, slotName, "Quick Save", SaveSlotType::QuickSave, thumbnailPNG, callback,
                                        [slotIndex]()
                                        { s_QuickSaveInFlight[slotIndex].store(false, std::memory_order_release); });
                if (result != SaveLoadResult::Pending)
                {
                    // Early failure before worker launched — release slot
                    s_QuickSaveInFlight[slotIndex].store(false, std::memory_order_release);
                }
                return result;
            }
        }

        OLO_CORE_WARN("[SaveGameManager] All quick-save slots are in-flight, skipping");
        if (callback)
        {
            Tasks::EnqueueGameThreadTask(
                [callback]()
                { callback(SaveLoadResult::IOError, "quicksave"); },
                "QuickSaveAllSlotsInFlight");
        }
        return SaveLoadResult::IOError;
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
                auto result = SaveAsync(scene, slotName, "Auto Save", SaveSlotType::AutoSave, thumbnailPNG, callback,
                                        [slotIndex]()
                                        { s_AutoSaveInFlight[slotIndex].store(false, std::memory_order_release); });
                if (result != SaveLoadResult::Pending)
                {
                    s_AutoSaveInFlight[slotIndex].store(false, std::memory_order_release);
                }
                return result;
            }
        }

        OLO_CORE_WARN("[SaveGameManager] All auto-save slots are in-flight, skipping");
        if (callback)
        {
            Tasks::EnqueueGameThreadTask(
                [callback]()
                { callback(SaveLoadResult::IOError, "autosave"); },
                "AutoSaveAllSlotsInFlight");
        }
        return SaveLoadResult::IOError;
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
            // Steam Cloud fallback (#644). LOCAL IS PREFERRED — this only runs when there is no
            // local file at all, so a cloud copy can never silently overwrite newer local
            // progress. The typical case is a fresh machine where Steam has synced the slot but
            // the player has not saved locally yet.
            //
            // Restores to disk and then falls through to the normal load path rather than
            // deserializing from memory, so checksum validation, header/version gating and every
            // other guard below apply to a cloud save exactly as they do to a local one.
            if (!RestoreSaveFromCloud(path, slotName))
            {
                OLO_CORE_ERROR("[SaveGameManager] Save file not found: {}", path.string());
                return SaveLoadResult::FileNotFound;
            }
        }

        // Validate checksum first
        if (!SaveGameFile::ValidateChecksum(path))
        {
            OLO_CORE_ERROR("[SaveGameManager] Checksum validation failed: {}", path.string());
            return SaveLoadResult::ChecksumMismatch;
        }

        // Read header to learn the save's FormatVersion (may be older than
        // kSaveGameFormatVersion — see SaveGameTypes.h/SaveGameSerializer.h,
        // issue #454) so the component deserializer can gate fields correctly.
        SaveGameHeader header;
        if (!SaveGameFile::ReadHeader(path, header))
        {
            OLO_CORE_ERROR("[SaveGameManager] Failed to read header: {}", path.string());
            return SaveLoadResult::IOError;
        }

        if (header.FormatVersion < kSaveGameFormatVersion)
        {
            OLO_CORE_INFO("[SaveGameManager] Migrating save '{}' from format v{} to v{}",
                          slotName, header.FormatVersion, kSaveGameFormatVersion);
        }

        // Read payload
        std::vector<u8> payload;
        if (!SaveGameFile::ReadPayload(path, payload))
        {
            OLO_CORE_ERROR("[SaveGameManager] Failed to read/decompress payload: {}", path.string());
            return SaveLoadResult::IOError;
        }

        // Restore scene state
        if (!SaveGameSerializer::RestoreSceneState(scene, payload, header.FormatVersion))
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

        std::error_code ec;
        if (std::filesystem::exists(saveDir))
        {
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
        }

        // Union in CLOUD-ONLY slots (#644).
        //
        // Without this, a player on a fresh machine sees an empty load menu even though Steam has
        // their saves — the files exist in the cloud but not yet on disk, so the directory scan
        // above finds nothing. That is the single most visible way a cloud integration can look
        // broken while working perfectly.
        //
        // Cloud entries are added ONLY when the slot has no local file: a local save always wins,
        // and this must never shadow it with a possibly-older cloud copy. Metadata comes from
        // pulling the file down (the same restore the load path uses), because the header lives
        // inside the file and there is no cheaper way to read it — and having done so, the slot is
        // no longer cloud-only, so the next enumerate finds it locally.
        if (SteamManager::IsCloudEnabled())
        {
            for (const std::string& cloudName : SteamManager::CloudEnumerate())
            {
                const std::string slotName = SlotForCloudName(cloudName);
                if (slotName.empty() || !IsValidSlotName(slotName))
                {
                    continue;
                }

                const auto localPath = GetSaveFilePath(slotName);
                if (localPath.empty() || std::filesystem::exists(localPath))
                {
                    continue; // local wins
                }

                if (!RestoreSaveFromCloud(localPath, slotName))
                {
                    continue;
                }

                SaveGameHeader header;
                SaveGameMetadata metadata;
                if (SaveGameFile::ReadMetadata(localPath, header, metadata))
                {
                    SaveFileInfo info;
                    info.FilePath = localPath;
                    info.Metadata = metadata;
                    info.FileSizeBytes = std::filesystem::file_size(localPath, ec);
                    if (ec)
                    {
                        info.FileSizeBytes = 0;
                    }
                    info.HasThumbnail = metadata.ThumbnailAvailable;
                    saves.push_back(std::move(info));
                }
            }
        }

        // Sort by timestamp, most recent first
        std::ranges::sort(saves,
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

        if (SaveGameHeader header; !SaveGameFile::ReadMetadata(path, header, outInfo.Metadata))
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

        // Keep the bytes so a failed LOCAL delete can put the cloud copy back (#644).
        //
        // There is no two-phase commit across the filesystem and Steam Cloud, so some ordering
        // always loses. Deleting cloud-then-local and failing on the local step would report
        // failure while having already destroyed the cloud copy — the player retries, sees it
        // fail again, and their off-machine copy is silently gone. Holding the bytes lets that
        // case be undone, so a reported failure really does mean "nothing was removed".
        //
        // Read before touching anything, and only when there is something to lose. Deletes are
        // rare and user-initiated, so the read cost is acceptable where it would not be on a
        // per-frame path.
        std::vector<u8> localBytesForRollback;
        const bool localExisted = std::filesystem::exists(path);
        if (localExisted)
        {
            // Best-effort: if this fails we simply cannot roll back, which is no worse than
            // before. Do not abort the delete over it.
            (void)ReadWholeFile(path, localBytesForRollback);
        }

        // Delete the cloud copy too (#644), BEFORE the local-existence check below.
        //
        // Without this a deleted save comes straight back: EnumerateSaves unions in cloud-only
        // slots, so the next load menu would re-download the very file the player just deleted.
        // A cloud-only slot must also be deletable, which is why this runs even when there is no
        // local file — hence its placement above the early return.
        bool removedFromCloud = false;
        if (SteamManager::IsCloudEnabled())
        {
            const std::string cloudName = CloudNameForSlot(slotName);
            if (SteamManager::CloudExists(cloudName))
            {
                removedFromCloud = SteamSucceeded(SteamManager::CloudDelete(cloudName));
                if (!removedFromCloud)
                {
                    // ABORT — do NOT fall through and delete the local copy.
                    //
                    // Deleting locally while the cloud copy survives is strictly worse than
                    // failing: EnumerateSaves unions in cloud-only slots, so the very next load
                    // menu re-downloads the save the player just deleted. It comes back from the
                    // dead, which reads as the delete being ignored — and meanwhile the local
                    // file, the only copy they could still have loaded, is gone.
                    //
                    // Failing with both copies intact keeps the two in sync and leaves the player
                    // able to retry.
                    OLO_CORE_ERROR("[SaveGameManager] Could not delete '{}' from Steam Cloud; keeping the local copy "
                                   "too so the two stay in sync (deleting locally would let the cloud copy "
                                   "reappear on the next enumerate).",
                                   slotName);
                    return false;
                }
            }
        }

        if (!std::filesystem::exists(path))
        {
            // Nothing local, but a cloud-only slot really was removed — report success so a load
            // menu refreshes rather than telling the player the delete failed.
            return removedFromCloud;
        }

        std::error_code ec;
        bool removed = std::filesystem::remove(path, ec);
        if (removed)
        {
            OLO_CORE_INFO("[SaveGameManager] Deleted save: {}", slotName);
            return true;
        }

        // Local delete failed — typically the file is locked or permissions changed.
        //
        // If the cloud copy was already removed above, restore it. Otherwise this returns false
        // having destroyed the player's off-machine copy while telling them nothing was deleted,
        // which is the worst of both outcomes: they retry, it fails again, and the cloud copy
        // never comes back.
        if (removedFromCloud && !localBytesForRollback.empty())
        {
            const SteamResult restored =
                SteamManager::CloudWrite(CloudNameForSlot(slotName), std::span<const u8>{ localBytesForRollback });
            if (SteamSucceeded(restored))
            {
                OLO_CORE_WARN("[SaveGameManager] Could not delete '{}' locally ({}); restored the Steam Cloud copy so "
                              "both copies survive and the delete can be retried.",
                              slotName, ec.message());
            }
            else
            {
                // Both the local delete and the rollback failed. Nothing further to try, but say
                // so plainly — this is the one path where the two copies genuinely disagree.
                OLO_CORE_ERROR("[SaveGameManager] Could not delete '{}' locally ({}) AND could not restore its Steam "
                               "Cloud copy ({}). The local save remains; the cloud copy is gone.",
                               slotName, ec.message(), SteamResultToString(restored));
            }
        }
        else if (removedFromCloud)
        {
            OLO_CORE_ERROR("[SaveGameManager] Could not delete '{}' locally ({}) and had no bytes to restore its "
                           "Steam Cloud copy. The local save remains; the cloud copy is gone.",
                           slotName, ec.message());
        }

        return false;
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
            auto result = AutoSave(scene);
            if (result != SaveLoadResult::Pending && result != SaveLoadResult::Success)
            {
                OLO_CORE_TRACE("[SaveGameManager] AutoSave skipped (result: {})", static_cast<int>(result));
            }
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
        return GetSaveDirectory() / (slotName + std::string(SaveGameManager::kSaveFileExtension));
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
                                              std::vector<u8> thumbnailPNG,
                                              SaveLoadCompletionCallback callback,
                                              std::function<void()> onWorkerComplete)
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
             thumbnailPNG = std::move(thumbnailPNG),
             metadata,
             entityCount,
             path,
             slotName,
             callback,
             onWorkerComplete = std::move(onWorkerComplete)]() mutable
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

                    // Steam Cloud mirror (#644). We are on a WORKER thread here and SteamManager
                    // is game-thread-only, so hop rather than calling it inline — this is the one
                    // place the cloud integration could have introduced a data race.
                    //
                    // Read the bytes HERE, on the worker, and move them into the task. The game
                    // thread then only makes the Steam call, never touches the disk: re-reading a
                    // save-sized file on the frame thread would be a hitch for no benefit, since
                    // this thread just finished writing it and is not frame-critical.
                    //
                    // Enqueued without checking Steam availability first — querying it from this
                    // thread would itself violate the game-thread contract — so the task re-checks
                    // and no-ops when Steam is absent. The read is skipped in that case only by
                    // the emptiness guard inside, which is the price of not being able to ask.
                    //
                    // Deliberately AFTER the local write succeeded, and it cannot affect `result`:
                    // a cloud failure must never turn a good local save into a reported error.
                    std::vector<u8> cloudBytes;
                    u64 generation = 0;
                    if (ReadSaveAndTakeCloudTicket(path, slotName, cloudBytes, generation))
                    {
                        Tasks::EnqueueGameThreadTask(
                            [bytes = std::move(cloudBytes), slotName, generation]() mutable
                            {
                                MirrorSaveToCloud(std::move(bytes), slotName, generation);
                            },
                            "SteamCloudMirror");
                    }
                    else
                    {
                        OLO_CORE_WARN("[SaveGameManager] Could not re-read '{}' to mirror it to Steam Cloud; "
                                      "the local save is intact.",
                                      path.string());
                    }
                }

                // Worker-thread completion hook (e.g., release in-flight slot)
                if (onWorkerComplete)
                {
                    onWorkerComplete();
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

} // namespace OloEngine
