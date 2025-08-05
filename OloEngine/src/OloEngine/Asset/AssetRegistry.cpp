#include "AssetRegistry.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Asset/AssetExtensions.h"
#include <fstream>

namespace OloEngine
{
    void AssetRegistry::AddAsset(const AssetMetadata& metadata)
    {
        std::unique_lock lock(m_Mutex);
        
        if (metadata.Handle == 0)
        {
            OLO_CORE_WARN("AssetRegistry::AddAsset - Invalid asset handle");
            return;
        }

        // Update main storage
        m_AssetMetadata[metadata.Handle] = metadata;
        
        // Update path lookup if path is valid
        if (!metadata.FilePath.empty())
        {
            m_PathToHandle[metadata.FilePath] = metadata.Handle;
        }
    }

    bool AssetRegistry::RemoveAsset(AssetHandle handle)
    {
        std::unique_lock lock(m_Mutex);
        
        auto it = m_AssetMetadata.find(handle);
        if (it == m_AssetMetadata.end())
            return false;

        // Remove from path lookup
        if (!it->second.FilePath.empty())
        {
            m_PathToHandle.erase(it->second.FilePath);
        }

        // Remove from main storage
        m_AssetMetadata.erase(it);
        return true;
    }

    AssetMetadata AssetRegistry::GetMetadata(AssetHandle handle) const
    {
        std::shared_lock lock(m_Mutex);
        
        auto it = m_AssetMetadata.find(handle);
        return (it != m_AssetMetadata.end()) ? it->second : AssetMetadata{};
    }

    AssetMetadata AssetRegistry::GetMetadata(const std::filesystem::path& path) const
    {
        std::shared_lock lock(m_Mutex);
        
        auto it = m_PathToHandle.find(path);
        if (it != m_PathToHandle.end())
        {
            auto metaIt = m_AssetMetadata.find(it->second);
            if (metaIt != m_AssetMetadata.end())
                return metaIt->second;
        }
        
        return AssetMetadata{};
    }

    bool AssetRegistry::Exists(AssetHandle handle) const
    {
        std::shared_lock lock(m_Mutex);
        return m_AssetMetadata.find(handle) != m_AssetMetadata.end();
    }

    bool AssetRegistry::Exists(const std::filesystem::path& path) const
    {
        std::shared_lock lock(m_Mutex);
        return m_PathToHandle.find(path) != m_PathToHandle.end();
    }

    AssetHandle AssetRegistry::GetHandleFromPath(const std::filesystem::path& path) const
    {
        std::shared_lock lock(m_Mutex);
        
        auto it = m_PathToHandle.find(path);
        return (it != m_PathToHandle.end()) ? it->second : AssetHandle(0);
    }

    std::vector<AssetMetadata> AssetRegistry::GetAssetsOfType(AssetType type) const
    {
        std::shared_lock lock(m_Mutex);
        
        std::vector<AssetMetadata> result;
        for (const auto& [handle, metadata] : m_AssetMetadata)
        {
            if (metadata.Type == type)
                result.push_back(metadata);
        }
        
        return result;
    }

    std::unordered_set<AssetHandle> AssetRegistry::GetAssetHandlesOfType(AssetType type) const
    {
        std::shared_lock lock(m_Mutex);
        
        std::unordered_set<AssetHandle> result;
        for (const auto& [handle, metadata] : m_AssetMetadata)
        {
            if (metadata.Type == type)
                result.insert(handle);
        }
        
        return result;
    }

    std::vector<AssetMetadata> AssetRegistry::GetAllAssets() const
    {
        std::shared_lock lock(m_Mutex);
        
        std::vector<AssetMetadata> result;
        result.reserve(m_AssetMetadata.size());
        
        for (const auto& [handle, metadata] : m_AssetMetadata)
        {
            result.push_back(metadata);
        }
        
        return result;
    }

    size_t AssetRegistry::GetAssetCount() const
    {
        std::shared_lock lock(m_Mutex);
        return m_AssetMetadata.size();
    }

    void AssetRegistry::Clear()
    {
        std::unique_lock lock(m_Mutex);
        m_AssetMetadata.clear();
        m_PathToHandle.clear();
        m_HandleCounter = 1;
    }

    void AssetRegistry::UpdateMetadata(AssetHandle handle, const AssetMetadata& metadata)
    {
        std::unique_lock lock(m_Mutex);
        
        auto it = m_AssetMetadata.find(handle);
        if (it == m_AssetMetadata.end())
        {
            OLO_CORE_WARN("AssetRegistry::UpdateMetadata - Asset handle {} not found", handle);
            return;
        }

        // Remove old path mapping
        if (!it->second.FilePath.empty())
        {
            m_PathToHandle.erase(it->second.FilePath);
        }

        // Update metadata (preserve handle)
        AssetMetadata updatedMetadata = metadata;
        updatedMetadata.Handle = handle;
        it->second = updatedMetadata;

        // Add new path mapping
        if (!updatedMetadata.FilePath.empty())
        {
            m_PathToHandle[updatedMetadata.FilePath] = handle;
        }
    }

    AssetHandle AssetRegistry::GenerateHandle()
    {
        std::unique_lock lock(m_Mutex);
        return GetNextHandle();
    }

    bool AssetRegistry::Serialize(const std::filesystem::path& filepath) const
    {
        try
        {
            std::shared_lock lock(m_Mutex);
            
            std::ofstream file(filepath, std::ios::binary);
            if (!file.is_open())
            {
                OLO_CORE_ERROR("AssetRegistry::Serialize - Failed to open file: {}", filepath.string());
                return false;
            }

            // Write header
            const u32 version = 1;
            file.write(reinterpret_cast<const char*>(&version), sizeof(version));
            
            // Write asset count
            const u32 assetCount = static_cast<u32>(m_AssetMetadata.size());
            file.write(reinterpret_cast<const char*>(&assetCount), sizeof(assetCount));

            // Write each asset metadata
            for (const auto& [handle, metadata] : m_AssetMetadata)
            {
                file.write(reinterpret_cast<const char*>(&metadata.Handle), sizeof(metadata.Handle));
                file.write(reinterpret_cast<const char*>(&metadata.Type), sizeof(metadata.Type));
                file.write(reinterpret_cast<const char*>(&metadata.Status), sizeof(metadata.Status));
                file.write(reinterpret_cast<const char*>(&metadata.FileLastWriteTime), sizeof(metadata.FileLastWriteTime));
                
                // Write path string
                const std::string pathStr = metadata.FilePath.string();
                const u32 pathLength = static_cast<u32>(pathStr.length());
                file.write(reinterpret_cast<const char*>(&pathLength), sizeof(pathLength));
                file.write(pathStr.c_str(), pathLength);
            }

            OLO_CORE_INFO("AssetRegistry serialized to: {}", filepath.string());
            return true;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("AssetRegistry::Serialize - Exception: {}", e.what());
            return false;
        }
    }

    bool AssetRegistry::Deserialize(const std::filesystem::path& filepath)
    {
        try
        {
            std::ifstream file(filepath, std::ios::binary);
            if (!file.is_open())
            {
                OLO_CORE_WARN("AssetRegistry::Deserialize - File not found: {}", filepath.string());
                return false;
            }

            std::unique_lock lock(m_Mutex);
            
            // Clear existing data
            m_AssetMetadata.clear();
            m_PathToHandle.clear();

            // Read header
            u32 version;
            file.read(reinterpret_cast<char*>(&version), sizeof(version));
            
            if (version != 1)
            {
                OLO_CORE_ERROR("AssetRegistry::Deserialize - Unsupported version: {}", version);
                return false;
            }

            // Read asset count
            u32 assetCount;
            file.read(reinterpret_cast<char*>(&assetCount), sizeof(assetCount));

            // Read each asset metadata
            for (u32 i = 0; i < assetCount; ++i)
            {
                AssetMetadata metadata;
                
                file.read(reinterpret_cast<char*>(&metadata.Handle), sizeof(metadata.Handle));
                file.read(reinterpret_cast<char*>(&metadata.Type), sizeof(metadata.Type));
                file.read(reinterpret_cast<char*>(&metadata.Status), sizeof(metadata.Status));
                file.read(reinterpret_cast<char*>(&metadata.FileLastWriteTime), sizeof(metadata.FileLastWriteTime));
                
                // Read path string
                u32 pathLength;
                file.read(reinterpret_cast<char*>(&pathLength), sizeof(pathLength));
                
                std::string pathStr(pathLength, '\0');
                file.read(pathStr.data(), pathLength);
                metadata.FilePath = pathStr;

                // Add to registry
                m_AssetMetadata[metadata.Handle] = metadata;
                if (!metadata.FilePath.empty())
                {
                    m_PathToHandle[metadata.FilePath] = metadata.Handle;
                }

                // Update handle counter
                if (metadata.Handle >= m_HandleCounter)
                {
                    m_HandleCounter = metadata.Handle + 1;
                }
            }

            OLO_CORE_INFO("AssetRegistry deserialized from: {} ({} assets)", filepath.string(), assetCount);
            return true;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("AssetRegistry::Deserialize - Exception: {}", e.what());
            return false;
        }
    }

    bool AssetRegistry::Empty() const
    {
        std::shared_lock lock(m_Mutex);
        return m_AssetMetadata.empty();
    }

    AssetHandle AssetRegistry::GetNextHandle()
    {
        return m_HandleCounter++;
    }

}
