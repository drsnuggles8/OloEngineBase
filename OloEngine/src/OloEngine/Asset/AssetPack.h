#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Serialization/AssetPackFile.h"
#include <filesystem>
#include <optional>
#include <vector>

namespace OloEngine
{
    class FileStreamReader;

    /**
     * @brief Runtime asset pack reader for loading assets from binary pack files
     * 
     * The AssetPack class provides runtime access to binary asset pack files,
     * allowing efficient loading of assets in shipping builds. It handles:
     * 
     * - Loading and validation of pack files
     * - Asset lookup and metadata retrieval
     * - Stream reader creation for asset data access
     * - Memory-efficient pack file management
     */
    class AssetPack : public RefCounted
    {
    public:
        static Ref<AssetPack> Create() { return Ref<AssetPack>::Create(); }
        
        /**
         * @brief Load an asset pack from file
         * @param path Path to the asset pack file
         * @return True if loading was successful
         */
        bool Load(const std::filesystem::path& path);
        
        /**
         * @brief Unload the asset pack and free resources
         */
        void Unload();
        
        /**
         * @brief Check if the pack is currently loaded
         * @return True if pack is loaded
         */
        bool IsLoaded() const { return m_IsLoaded; }
        
        /**
         * @brief Check if an asset is available in this pack
         * @param handle Asset handle to check
         * @return True if asset is available
         */
        bool IsAssetAvailable(AssetHandle handle) const;
        
        /**
         * @brief Get the type of an asset in the pack
         * @param handle Asset handle
         * @return Asset type or AssetType::None if not found
         */
        AssetType GetAssetType(AssetHandle handle) const;
        
        /**
         * @brief Get asset information from the pack
         * @param handle Asset handle
         * @return Asset info if found, nullopt otherwise
         */
        std::optional<AssetPackFile::AssetInfo> GetAssetInfo(AssetHandle handle) const;
        
        /**
         * @brief Create a stream reader for reading asset data
         * @return Stream reader or nullptr if failed
         */
        std::unique_ptr<FileStreamReader> GetAssetStreamReader() const;
        
        /**
         * @brief Get all asset infos in the pack
         * @return Vector of asset infos
         */
        const std::vector<AssetPackFile::AssetInfo>& GetAllAssetInfos() const;
        
        /**
         * @brief Get all scene infos in the pack
         * @return Vector of scene infos
         */
        const std::vector<AssetPackFile::SceneInfo>& GetAllSceneInfos() const;
        
        /**
         * @brief Get the underlying asset pack file structure
         * @return Asset pack file structure
         */
        const AssetPackFile& GetAssetPackFile() const { return m_AssetPackFile; }
        
        /**
         * @brief Get the pack file path
         * @return Pack file path
         */
        const std::filesystem::path& GetPackPath() const { return m_PackPath; }
        
    private:
        AssetPackFile m_AssetPackFile;
        std::filesystem::path m_PackPath;
        bool m_IsLoaded = false;
    };

} // namespace OloEngine
