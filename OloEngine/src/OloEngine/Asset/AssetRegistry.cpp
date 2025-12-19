#include "AssetRegistry.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Asset/AssetExtensions.h"
#include <cstdint>
#include <fstream>
#include <filesystem>

namespace OloEngine
{
    // Static empty metadata for invalid lookups
    static const AssetMetadata s_EmptyMetadata{};
    void AssetRegistry::AddAsset(const AssetMetadata& metadata)
    {
        std::unique_lock lock(m_Mutex);

        if (metadata.Handle == 0)
        {
            OLO_CORE_WARN("AssetRegistry::AddAsset - Invalid asset handle");
            return;
        }

        // Check for existing asset with same handle
        auto handleIt = m_AssetMetadata.find(metadata.Handle);
        if (handleIt != m_AssetMetadata.end())
        {
            // Handle already exists - log warning about potential overwrite
            if (handleIt->second.FilePath != metadata.FilePath || handleIt->second.Type != metadata.Type)
            {
                OLO_CORE_WARN("AssetRegistry::AddAsset - Handle {} already exists for different asset (existing: {}, new: {}). Overwriting existing asset.",
                              metadata.Handle, handleIt->second.FilePath.string(), metadata.FilePath.string());
            }
            else
            {
                OLO_CORE_WARN("AssetRegistry::AddAsset - Handle {} already exists for same asset. Updating metadata.", metadata.Handle);
            }
        }

        // Check for existing asset with same path (if path is provided)
        if (!metadata.FilePath.empty())
        {
            auto pathIt = m_PathToHandle.find(metadata.FilePath);
            if (pathIt != m_PathToHandle.end() && pathIt->second != metadata.Handle)
            {
                OLO_CORE_WARN("AssetRegistry::AddAsset - Path {} already mapped to different handle {} (new handle: {}). Overwriting existing path mapping.",
                              metadata.FilePath.string(), pathIt->second, metadata.Handle);
            }
        }

        // Update main storage
        m_AssetMetadata[metadata.Handle] = metadata;

        // Update path lookup if path is valid
        if (!metadata.FilePath.empty())
        {
            m_PathToHandle[metadata.FilePath] = metadata.Handle;
        }

        // Update handle counter to maintain monotonic sequence and avoid duplicates
        u64 currentCounter = m_HandleCounter.load(std::memory_order_relaxed);
        if (metadata.Handle >= currentCounter)
        {
            m_HandleCounter.store(metadata.Handle + 1, std::memory_order_relaxed);
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
        OLO_PROFILE_FUNCTION();

        std::shared_lock lock(m_Mutex);

        auto it = m_AssetMetadata.find(handle);
        return (it != m_AssetMetadata.end()) ? it->second : s_EmptyMetadata;
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

        return s_EmptyMetadata;
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

    sizet AssetRegistry::GetAssetCount() const noexcept
    {
        std::shared_lock lock(m_Mutex);
        return m_AssetMetadata.size();
    }

    void AssetRegistry::Clear()
    {
        std::unique_lock lock(m_Mutex);
        m_AssetMetadata.clear();
        m_PathToHandle.clear();
        m_HandleCounter.store(1, std::memory_order_relaxed);
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

        // Add new path mapping (check for collision first)
        if (!updatedMetadata.FilePath.empty())
        {
            // Check if the new path is already mapped to a different handle
            auto pathIt = m_PathToHandle.find(updatedMetadata.FilePath);
            if (pathIt != m_PathToHandle.end() && pathIt->second != handle)
            {
                OLO_CORE_WARN("AssetRegistry::UpdateMetadata - Path {} already mapped to different handle {} (current handle: {}). Overwriting existing path mapping.",
                              updatedMetadata.FilePath.string(), pathIt->second, handle);
            }

            m_PathToHandle[updatedMetadata.FilePath] = handle;
        }
    }

    AssetHandle AssetRegistry::GenerateHandle()
    {
        // No lock needed - GetNextHandle uses atomic operations for thread safety
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

            // Enable exceptions for better error detection on I/O failures
            file.exceptions(std::ofstream::failbit | std::ofstream::badbit);

            // Write header
            const u32 version = 2;
            file.write(reinterpret_cast<const char*>(&version), sizeof(version));

            // Write asset count
            const u32 assetCount = static_cast<u32>(m_AssetMetadata.size());
            file.write(reinterpret_cast<const char*>(&assetCount), sizeof(assetCount));

            // Write each asset metadata
            for (const auto& [handle, metadata] : m_AssetMetadata)
            {
                // Cast to fixed-width types for cross-platform compatibility
                const uint64_t handleValue = static_cast<uint64_t>(metadata.Handle);
                const uint32_t typeValue = static_cast<uint32_t>(metadata.Type);
                const uint32_t statusValue = static_cast<uint32_t>(metadata.Status);

                file.write(reinterpret_cast<const char*>(&handleValue), sizeof(handleValue));
                file.write(reinterpret_cast<const char*>(&typeValue), sizeof(typeValue));
                file.write(reinterpret_cast<const char*>(&statusValue), sizeof(statusValue));
                // Note: LastWriteTime serialization temporarily skipped - std::filesystem::file_time_type is complex to serialize

                // Write path string
                const std::string pathStr = metadata.FilePath.string();
                const u32 pathLength = static_cast<u32>(pathStr.length());
                file.write(reinterpret_cast<const char*>(&pathLength), sizeof(pathLength));
                file.write(pathStr.c_str(), pathLength);
            }

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

            // Enable exceptions for better error detection on I/O failures
            file.exceptions(std::ifstream::failbit | std::ifstream::badbit);

            std::unique_lock lock(m_Mutex);

            // Clear existing data
            m_AssetMetadata.clear();
            m_PathToHandle.clear();
            m_HandleCounter.store(1, std::memory_order_relaxed); // Reset handle counter to prevent stale handle values

            // Read header
            u32 version;
            file.read(reinterpret_cast<char*>(&version), sizeof(version));

            if (version != 1 && version != 2)
            {
                OLO_CORE_ERROR("AssetRegistry::Deserialize - Unsupported version: {}", version);
                return false;
            }

            // Read asset count
            u32 assetCount;
            file.read(reinterpret_cast<char*>(&assetCount), sizeof(assetCount));

            // Validate assetCount to prevent excessive memory usage or infinite loops
            const u32 MAX_ASSET_COUNT = 1000000; // 1 million assets maximum
            if (assetCount > MAX_ASSET_COUNT)
            {
                OLO_CORE_ERROR("AssetRegistry::Deserialize - Invalid asset count: {} exceeds maximum {}", assetCount, MAX_ASSET_COUNT);
                return false;
            }

            // Read each asset metadata
            for (u32 i = 0; i < assetCount; ++i)
            {
                AssetMetadata metadata;

                if (version == 1)
                {
                    // Legacy format - read raw types directly (may have endianness issues)
                    file.read(reinterpret_cast<char*>(&metadata.Handle), sizeof(metadata.Handle));
                    file.read(reinterpret_cast<char*>(&metadata.Type), sizeof(metadata.Type));
                    file.read(reinterpret_cast<char*>(&metadata.Status), sizeof(metadata.Status));
                }
                else // version == 2
                {
                    // New format - read fixed-width types for cross-platform compatibility
                    uint64_t handleValue;
                    uint32_t typeValue;
                    uint32_t statusValue;

                    file.read(reinterpret_cast<char*>(&handleValue), sizeof(handleValue));
                    file.read(reinterpret_cast<char*>(&typeValue), sizeof(typeValue));
                    file.read(reinterpret_cast<char*>(&statusValue), sizeof(statusValue));

                    metadata.Handle = static_cast<AssetHandle>(handleValue);
                    metadata.Type = static_cast<AssetType>(typeValue);
                    metadata.Status = static_cast<AssetStatus>(statusValue);
                }
                // Note: LastWriteTime deserialization temporarily skipped - will be refreshed from filesystem

                // Read path string
                u32 pathLength;
                file.read(reinterpret_cast<char*>(&pathLength), sizeof(pathLength));

                // Validate pathLength to prevent excessive memory allocation
                const u32 MAX_PATH_LENGTH = 32768; // 32KB limit for path strings
                if (pathLength > MAX_PATH_LENGTH)
                {
                    OLO_CORE_ERROR("AssetRegistry::Deserialize - Invalid path length: {} exceeds maximum {}", pathLength, MAX_PATH_LENGTH);
                    return false;
                }

                std::string pathStr(pathLength, '\0');
                file.read(pathStr.data(), pathLength);
                metadata.FilePath = pathStr;

                // Refresh LastWriteTime from filesystem if file exists
                if (std::filesystem::exists(metadata.FilePath))
                {
                    metadata.LastWriteTime = std::filesystem::last_write_time(metadata.FilePath);
                }

                // Add to registry
                m_AssetMetadata[metadata.Handle] = metadata;
                if (!metadata.FilePath.empty())
                {
                    m_PathToHandle[metadata.FilePath] = metadata.Handle;
                }

                // Update handle counter
                u64 currentCounter = m_HandleCounter.load(std::memory_order_relaxed);
                if (metadata.Handle >= currentCounter)
                {
                    m_HandleCounter.store(metadata.Handle + 1, std::memory_order_relaxed);
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
        // Thread-safe atomic increment - no additional locking needed
        return m_HandleCounter.fetch_add(1, std::memory_order_relaxed);
    }

} // namespace OloEngine
