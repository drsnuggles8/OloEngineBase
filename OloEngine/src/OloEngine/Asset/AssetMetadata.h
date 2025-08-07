#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Asset/AssetTypes.h"
#include <string>
#include <filesystem>

namespace OloEngine
{
    enum class AssetStatus : u8
    {
        None = 0,
        Ready,
        Invalid,
        Loading
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

        bool IsValid() const { return Handle != 0; }
        bool IsReady() const { return Status == AssetStatus::Ready; }
        bool IsLoading() const { return Status == AssetStatus::Loading; }
        bool IsInvalid() const { return Status == AssetStatus::Invalid; }
    };

    /**
     * @brief Editor load response structure for asset loading operations
     */
    struct EditorAssetLoadResponse
    {
        AssetMetadata Metadata;
        Ref<Asset> AssetRef;
        
        EditorAssetLoadResponse() = default;
        EditorAssetLoadResponse(const AssetMetadata& metadata, Ref<Asset> asset = nullptr)
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
    struct RuntimeAssetLoadResponse
    {
        bool Success = false;
        AssetHandle Handle;
        u32 LoadTime = 0; // Load time in milliseconds
        std::string ErrorMessage;
        
        RuntimeAssetLoadResponse() = default;
        RuntimeAssetLoadResponse(bool success, AssetHandle handle = 0, u32 loadTime = 0)
            : Success(success), Handle(handle), LoadTime(loadTime) {}
        RuntimeAssetLoadResponse(bool success, const std::string& error)
            : Success(success), ErrorMessage(error) {}
    };

}
