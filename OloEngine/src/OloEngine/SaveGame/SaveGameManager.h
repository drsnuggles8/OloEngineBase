#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/SaveGame/SaveGameTypes.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine
{
    class Scene;

    // Describes a save file on disk (from enumeration)
    struct SaveFileInfo
    {
        std::filesystem::path FilePath;
        SaveGameMetadata Metadata;
        u64 FileSizeBytes = 0;
        bool HasThumbnail = false;
    };

    // Save/Load operation result
    enum class SaveLoadResult : u8
    {
        Success = 0,
        FileNotFound,
        CorruptedFile,
        ChecksumMismatch,
        DecompressionFailed,
        SerializationFailed,
        NoActiveScene,
        IOError,
        InvalidInput
    };

    // Callback for async save/load completion
    using SaveLoadCompletionCallback = std::function<void(SaveLoadResult result, const std::string& slotName)>;

    // High-level save game manager — public API for saving and loading game state.
    // Handles file naming, directory management, auto-save, quick-save slots,
    // thumbnail capture, and async I/O via the Task system.
    class SaveGameManager
    {
      public:
        // Initialize the manager (call once at startup)
        static void Initialize();

        // Shutdown and cleanup (call once at exit)
        static void Shutdown();

        // --- Save Operations ---

        // Save to a named slot (async by default)
        static SaveLoadResult Save(Scene& scene,
                                   const std::string& slotName,
                                   const std::string& displayName = {},
                                   const std::vector<u8>& thumbnailPNG = {},
                                   SaveLoadCompletionCallback callback = nullptr);

        // Quick-save to rotating quick-save slot
        static SaveLoadResult QuickSave(Scene& scene,
                                        const std::vector<u8>& thumbnailPNG = {},
                                        SaveLoadCompletionCallback callback = nullptr);

        // Auto-save to rotating auto-save slot
        static SaveLoadResult AutoSave(Scene& scene,
                                       const std::vector<u8>& thumbnailPNG = {},
                                       SaveLoadCompletionCallback callback = nullptr);

        // --- Load Operations ---

        // Load from a named slot
        static SaveLoadResult Load(Scene& scene, const std::string& slotName);

        // Load from the most recent quick-save
        static SaveLoadResult QuickLoad(Scene& scene);

        // --- Enumeration ---

        // Enumerate all save files in the save directory
        static std::vector<SaveFileInfo> EnumerateSaves();

        // Get info for a specific slot
        static bool GetSaveInfo(const std::string& slotName, SaveFileInfo& outInfo);

        // Read thumbnail PNG from a save file
        static bool ReadThumbnail(const std::string& slotName, std::vector<u8>& outPNG);

        // --- Deletion ---

        // Delete a save file
        static bool DeleteSave(const std::string& slotName);

        // --- Auto-Save Configuration ---

        // Set auto-save interval in seconds (0 = disabled)
        static void SetAutoSaveInterval(f32 intervalSeconds);

        // Get current auto-save interval
        static f32 GetAutoSaveInterval();

        // Call every frame during play mode to handle auto-save timing
        static void Tick(f32 deltaTime, Scene& scene);

        // --- Utility ---

        // Get the save directory path
        static std::filesystem::path GetSaveDirectory();

        // Get the file path for a slot name
        static std::filesystem::path GetSaveFilePath(const std::string& slotName);

        // Check if a save file has a valid checksum
        static bool ValidateSave(const std::string& slotName);

      private:
        // Internal save implementation
        static SaveLoadResult SaveInternal(Scene& scene,
                                           const std::string& slotName,
                                           const std::string& displayName,
                                           SaveSlotType slotType,
                                           const std::vector<u8>& thumbnailPNG);

        // Ensure save directory exists
        static void EnsureSaveDirectory();

        // Generate rotating slot name (e.g. "quicksave_0", "quicksave_1")
        static std::string GetRotatingSlotName(const std::string& prefix, u32 maxSlots);

        static std::atomic<f32> s_AutoSaveInterval;
        static std::atomic<f32> s_AutoSaveTimer;
        static std::atomic<bool> s_Initialized;

        static constexpr u32 kMaxQuickSaveSlots = 3;
        static constexpr u32 kMaxAutoSaveSlots = 3;
        static constexpr std::string_view kSaveFileExtension = ".olosave";
    };

} // namespace OloEngine
