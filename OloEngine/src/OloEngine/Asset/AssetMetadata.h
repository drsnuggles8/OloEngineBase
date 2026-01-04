#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Asset/AssetTypes.h"
#include <string>
#include <filesystem>
#include <array>
#include <algorithm>
#include <cctype>

namespace OloEngine
{
    enum class AssetStatus : u8
    {
        None = 0,  // Asset metadata exists but no loading attempted
        NotLoaded, // Asset exists but not yet loaded into memory
        Loading,   // Asset is currently being loaded asynchronously
        Loaded,    // Asset successfully loaded and ready to use
        Failed,    // Asset loading failed (file corruption, format error, etc.)
        Missing,   // Asset file does not exist on disk
        Invalid    // Asset metadata is corrupted or asset type mismatch
    };

    /**
     * @brief Asset metadata structure containing information about an asset
     *
     * This structure stores metadata for assets including their handle, type,
     * file path, and loading status. Used by the asset management system for
     * tracking and loading assets.
     */
    struct AssetMetadata
    {
        AssetHandle Handle = 0;
        AssetType Type = AssetType::None;
        std::filesystem::path FilePath;
        AssetStatus Status = AssetStatus::None;

        // File modification tracking for hot-reload
        std::filesystem::file_time_type LastWriteTime;

        bool IsDataLoaded = false;

        AssetMetadata() = default;
        AssetMetadata(AssetHandle handle, AssetType type)
            : Handle(handle), Type(type) {}
        AssetMetadata(AssetHandle handle, AssetType type, const std::filesystem::path& path)
            : Handle(handle), Type(type), FilePath(path) {}

        bool IsValid() const
        {
            return Handle != 0;
        }
        bool IsReady() const
        {
            return Status == AssetStatus::Loaded;
        }
        bool IsLoading() const
        {
            return Status == AssetStatus::Loading;
        }
        bool IsInvalid() const
        {
            return Status == AssetStatus::Invalid;
        }
        bool IsFailed() const
        {
            return Status == AssetStatus::Failed;
        }
        bool IsMissing() const
        {
            return Status == AssetStatus::Missing;
        }
        bool IsLoaded() const
        {
            return Status == AssetStatus::Loaded;
        }
        bool IsNotLoaded() const
        {
            return Status == AssetStatus::NotLoaded || Status == AssetStatus::None;
        }
    };

    /**
     * @brief Editor load response structure for asset loading operations
     *
     * For assets that support async loading (SupportsAsyncLoading() == true),
     * the response may contain RawData instead of AssetRef. The caller must
     * then call FinalizeFromRawData on the main thread to create the GPU asset.
     */
    struct [[nodiscard]] EditorAssetLoadResponse
    {
        AssetMetadata Metadata;
        Ref<Asset> AssetRef;       ///< Finalized asset (null if NeedsGPUFinalization)
        bool NeedsGPUFinalization = false;  ///< True if raw data needs GPU finalization

        EditorAssetLoadResponse() = default;
        explicit EditorAssetLoadResponse(const AssetMetadata& metadata, Ref<Asset> asset = nullptr)
            : Metadata(metadata), AssetRef(asset) {}
    };

    /**
     * @brief Runtime asset load request structure
     */
    struct RuntimeAssetLoadRequest
    {
        AssetHandle SceneHandle = 0;
        AssetHandle Handle = 0;

        RuntimeAssetLoadRequest() = default;
        RuntimeAssetLoadRequest(AssetHandle sceneHandle, AssetHandle handle)
            : SceneHandle(sceneHandle), Handle(handle) {}
    };

    /**
     * @brief Runtime load response structure for asset loading operations
     */
    struct [[nodiscard]] RuntimeAssetLoadResponse
    {
        bool Success = false;
        AssetHandle Handle = 0;
        u32 LoadTime = 0; // Load time in milliseconds
        std::string ErrorMessage;

        // Static factory method for successful loads
        static RuntimeAssetLoadResponse Ok(AssetHandle handle, u32 loadTime = 0)
        {
            RuntimeAssetLoadResponse response;
            response.Success = true;
            response.Handle = handle;
            response.LoadTime = loadTime;
            return response;
        }

        // Static factory method for failed loads
        static RuntimeAssetLoadResponse Failure(const std::string& error)
        {
            RuntimeAssetLoadResponse response;
            response.Success = false;
            response.ErrorMessage = error;
            return response;
        }

      private:
        // Private constructors to enforce use of factory methods
        RuntimeAssetLoadResponse() = default;
    };

    /**
     * @brief Utility functions for AssetStatus
     */
    namespace AssetStatusUtils
    {
        inline constexpr const char* AssetStatusToString(AssetStatus status) noexcept
        {
            static constexpr std::array<const char*, 7> statusStrings = {
                "None",       // AssetStatus::None = 0
                "Not Loaded", // AssetStatus::NotLoaded = 1
                "Loading",    // AssetStatus::Loading = 2
                "Loaded",     // AssetStatus::Loaded = 3
                "Failed",     // AssetStatus::Failed = 4
                "Missing",    // AssetStatus::Missing = 5
                "Invalid"     // AssetStatus::Invalid = 6
            };

            const auto index = static_cast<std::size_t>(status);
            return (index < statusStrings.size()) ? statusStrings[index] : "Unknown";
        }

        inline AssetStatus AssetStatusFromString(const std::string& statusStr)
        {
            std::string lowerStr = statusStr;
            std::transform(lowerStr.begin(), lowerStr.end(), lowerStr.begin(),
                           [](unsigned char c)
                           { return std::tolower(c); });

            if (lowerStr == "none")
                return AssetStatus::None;
            if (lowerStr == "not loaded" || lowerStr == "notloaded")
                return AssetStatus::NotLoaded;
            if (lowerStr == "loading")
                return AssetStatus::Loading;
            if (lowerStr == "loaded")
                return AssetStatus::Loaded;
            if (lowerStr == "failed")
                return AssetStatus::Failed;
            if (lowerStr == "missing")
                return AssetStatus::Missing;
            if (lowerStr == "invalid")
                return AssetStatus::Invalid;
            return AssetStatus::None;
        }

        inline bool IsStatusError(AssetStatus status)
        {
            return status == AssetStatus::Failed ||
                   status == AssetStatus::Missing ||
                   status == AssetStatus::Invalid;
        }

        inline bool IsStatusSuccess(AssetStatus status)
        {
            return status == AssetStatus::Loaded;
        }
    } // namespace AssetStatusUtils

} // namespace OloEngine
