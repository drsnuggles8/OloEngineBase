#include "OloEnginePCH.h"
#include "AssetPack.h"

#include "OloEngine/Serialization/FileStream.h"
#include "OloEngine/Core/Log.h"
#include <chrono>

namespace OloEngine
{
    // Implementation of the custom deleter for FileStreamReader
    void FileStreamReaderDeleter::operator()(FileStreamReader* ptr) const
    {
        delete ptr;
    }
    
    AssetPackLoadResult AssetPack::Load(const std::filesystem::path& path)
    {
        auto startTime = std::chrono::high_resolution_clock::now();
        
        // Check if already loaded with the same path (idempotent behavior)
        if (m_IsLoaded && m_PackPath == path)
        {
            auto endTime = std::chrono::high_resolution_clock::now();
            AssetPackLoadResult result(true);
            result.LoadTimeMs = std::chrono::duration<f64, std::milli>(endTime - startTime).count();
            return result;
        }
        
        // If loading a different pack, unload current one first
        if (m_IsLoaded)
        {
            Unload();
        }

        // Check file existence
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_ERROR("AssetPack::Load - File does not exist: {}", path.string());
            return AssetPackLoadResult(AssetPackLoadError::FileNotFound, 
                                     "File does not exist: " + path.string());
        }

        // Open file for reading
        FileStreamReader stream(path);
        if (!stream.IsStreamGood())
        {
            OLO_CORE_ERROR("AssetPack::Load - Failed to open file: {}", path.string());
            return AssetPackLoadResult(AssetPackLoadError::FileOpenFailed, 
                                     "Failed to open file: " + path.string());
        }

        // Read file header with error checking
        try
        {
            stream.ReadRaw(m_AssetPackFile.Header.MagicNumber);
            if (!stream.IsStreamGood())
            {
                OLO_CORE_ERROR("AssetPack::Load - Failed to read magic number from header");
                return AssetPackLoadResult(AssetPackLoadError::CorruptHeader, 
                                         "Failed to read magic number from file header");
            }
            
            stream.ReadRaw(m_AssetPackFile.Header.Version);
            if (!stream.IsStreamGood())
            {
                OLO_CORE_ERROR("AssetPack::Load - Failed to read version from header");
                return AssetPackLoadResult(AssetPackLoadError::CorruptHeader, 
                                         "Failed to read version from file header");
            }
            
            stream.ReadRaw(m_AssetPackFile.Header.BuildVersion);
            if (!stream.IsStreamGood())
            {
                OLO_CORE_ERROR("AssetPack::Load - Failed to read build version from header");
                return AssetPackLoadResult(AssetPackLoadError::CorruptHeader, 
                                         "Failed to read build version from file header");
            }
            
            stream.ReadRaw(m_AssetPackFile.Header.IndexOffset);
            if (!stream.IsStreamGood())
            {
                OLO_CORE_ERROR("AssetPack::Load - Failed to read index offset from header");
                return AssetPackLoadResult(AssetPackLoadError::CorruptHeader, 
                                         "Failed to read index offset from file header");
            }
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("AssetPack::Load - Failed to read header: {}", e.what());
            return AssetPackLoadResult(AssetPackLoadError::CorruptHeader, 
                                     "Failed to read file header: " + std::string(e.what()));
        }

        // Validate magic number
        if (m_AssetPackFile.Header.MagicNumber != AssetPackFile::MagicNumber)
        {
            OLO_CORE_ERROR("AssetPack::Load - Invalid magic number: {:#x}", m_AssetPackFile.Header.MagicNumber);
            return AssetPackLoadResult(AssetPackLoadError::InvalidMagicNumber, 
                                     "Invalid magic number. This is not a valid asset pack file.");
        }

        // Validate version
        if (m_AssetPackFile.Header.Version != AssetPackFile::Version)
        {
            OLO_CORE_ERROR("AssetPack::Load - Unsupported version. Expected: {}, Got: {}", 
                         AssetPackFile::Version, m_AssetPackFile.Header.Version);
            return AssetPackLoadResult(AssetPackLoadError::UnsupportedVersion, 
                                     "Unsupported pack version. Expected: " + std::to_string(AssetPackFile::Version) + 
                                     ", Got: " + std::to_string(m_AssetPackFile.Header.Version));
        }

        // Get file size for bounds checking
        u64 fileSize = 0;
        try
        {
            fileSize = std::filesystem::file_size(path);
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("AssetPack::Load - Failed to get file size: {}", e.what());
            return AssetPackLoadResult(AssetPackLoadError::FileOpenFailed, 
                                     "Failed to get file size: " + std::string(e.what()));
        }

        // Validate IndexOffset is valid and within file bounds
        // Check for invalid offset (should be after header, which is at least sizeof(AssetPackFile::Header))
        const u64 minimumValidOffset = sizeof(AssetPackFile::Header);
        if (m_AssetPackFile.Header.IndexOffset < minimumValidOffset)
        {
            OLO_CORE_ERROR("AssetPack::Load - Index offset ({}) is too small (minimum: {})", 
                         m_AssetPackFile.Header.IndexOffset, minimumValidOffset);
            return AssetPackLoadResult(AssetPackLoadError::CorruptHeader, 
                                     "Index offset is too small. Expected offset >= " + std::to_string(minimumValidOffset) + 
                                     ", Got: " + std::to_string(m_AssetPackFile.Header.IndexOffset));
        }
        
        // Check that IndexOffset is within file bounds
        if (m_AssetPackFile.Header.IndexOffset >= fileSize)
        {
            OLO_CORE_ERROR("AssetPack::Load - Index offset ({}) is beyond file size ({})", 
                         m_AssetPackFile.Header.IndexOffset, fileSize);
            return AssetPackLoadResult(AssetPackLoadError::CorruptHeader, 
                                     "Index offset is beyond file size. Expected offset < " + std::to_string(fileSize) + 
                                     ", Got: " + std::to_string(m_AssetPackFile.Header.IndexOffset));
        }

        // Seek to index table and verify the operation succeeded
        u64 originalPosition = stream.GetStreamPosition();
        stream.SetStreamPosition(m_AssetPackFile.Header.IndexOffset);
        
        // Check if seek was successful by verifying the stream state and position
        if (!stream.IsStreamGood())
        {
            OLO_CORE_ERROR("AssetPack::Load - Stream became invalid after seeking to index offset");
            return AssetPackLoadResult(AssetPackLoadError::CorruptHeader, 
                                     "Failed to seek to index table - stream became invalid");
        }
        
        // Verify we're at the expected position
        u64 currentPosition = stream.GetStreamPosition();
        if (currentPosition != m_AssetPackFile.Header.IndexOffset)
        {
            OLO_CORE_ERROR("AssetPack::Load - Seek to index offset failed. Expected: {}, Actual: {}", 
                         m_AssetPackFile.Header.IndexOffset, currentPosition);
            return AssetPackLoadResult(AssetPackLoadError::CorruptHeader, 
                                     "Failed to seek to index table - position mismatch");
        }

        // Read index table header with error checking
        try
        {
            stream.ReadRaw(m_AssetPackFile.Index.AssetCount);
            if (!stream.IsStreamGood())
            {
                OLO_CORE_ERROR("AssetPack::Load - Failed to read asset count from index");
                return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                         "Failed to read asset count from index table");
            }
            
            // Validate AssetCount to prevent out-of-memory errors from corrupted/malicious files
            static constexpr u32 MAX_ASSET_COUNT = 1000000; // 1 million assets maximum
            if (m_AssetPackFile.Index.AssetCount == 0)
            {
                OLO_CORE_ERROR("AssetPack::Load - Asset count cannot be zero");
                return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                         "Asset count cannot be zero - invalid asset pack");
            }
            if (m_AssetPackFile.Index.AssetCount > MAX_ASSET_COUNT)
            {
                OLO_CORE_ERROR("AssetPack::Load - Asset count ({}) exceeds maximum allowed ({})", 
                             m_AssetPackFile.Index.AssetCount, MAX_ASSET_COUNT);
                return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                         "Asset count exceeds maximum allowed limit");
            }
            
            stream.ReadRaw(m_AssetPackFile.Index.SceneCount);
            if (!stream.IsStreamGood())
            {
                OLO_CORE_ERROR("AssetPack::Load - Failed to read scene count from index");
                return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                         "Failed to read scene count from index table");
            }
            
            // Validate SceneCount to prevent out-of-memory errors from corrupted/malicious files
            static constexpr u32 MAX_SCENE_COUNT = 10000; // 10 thousand scenes maximum
            if (m_AssetPackFile.Index.SceneCount == 0)
            {
                OLO_CORE_ERROR("AssetPack::Load - Scene count cannot be zero");
                return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                         "Scene count cannot be zero - invalid asset pack");
            }
            if (m_AssetPackFile.Index.SceneCount > MAX_SCENE_COUNT)
            {
                OLO_CORE_ERROR("AssetPack::Load - Scene count ({}) exceeds maximum allowed ({})", 
                             m_AssetPackFile.Index.SceneCount, MAX_SCENE_COUNT);
                return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                         "Scene count exceeds maximum allowed limit");
            }
            
            stream.ReadRaw(m_AssetPackFile.Index.PackedAppBinaryOffset);
            if (!stream.IsStreamGood())
            {
                OLO_CORE_ERROR("AssetPack::Load - Failed to read app binary offset from index");
                return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                         "Failed to read app binary offset from index table");
            }
            
            stream.ReadRaw(m_AssetPackFile.Index.PackedAppBinarySize);
            if (!stream.IsStreamGood())
            {
                OLO_CORE_ERROR("AssetPack::Load - Failed to read app binary size from index");
                return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                         "Failed to read app binary size from index table");
            }

            // Read asset infos
            m_AssetLookupMap.clear(); // Clear lookup map before populating with new assets
            m_AssetPackFile.AssetInfos.resize(m_AssetPackFile.Index.AssetCount);
            for (u32 i = 0; i < m_AssetPackFile.Index.AssetCount; i++)
            {
                auto& assetInfo = m_AssetPackFile.AssetInfos[i];
                stream.ReadRaw(assetInfo.Handle);
                if (!stream.IsStreamGood())
                {
                    OLO_CORE_ERROR("AssetPack::Load - Failed to read Handle for asset {} of {}", i + 1, m_AssetPackFile.Index.AssetCount);
                    return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                             "Failed to read asset Handle at index " + std::to_string(i));
                }
                
                stream.ReadRaw(assetInfo.PackedOffset);
                if (!stream.IsStreamGood())
                {
                    OLO_CORE_ERROR("AssetPack::Load - Failed to read PackedOffset for asset {} of {}", i + 1, m_AssetPackFile.Index.AssetCount);
                    return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                             "Failed to read asset PackedOffset at index " + std::to_string(i));
                }
                
                stream.ReadRaw(assetInfo.PackedSize);
                if (!stream.IsStreamGood())
                {
                    OLO_CORE_ERROR("AssetPack::Load - Failed to read PackedSize for asset {} of {}", i + 1, m_AssetPackFile.Index.AssetCount);
                    return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                             "Failed to read asset PackedSize at index " + std::to_string(i));
                }
                
                stream.ReadRaw(assetInfo.Type);
                if (!stream.IsStreamGood())
                {
                    OLO_CORE_ERROR("AssetPack::Load - Failed to read Type for asset {} of {}", i + 1, m_AssetPackFile.Index.AssetCount);
                    return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                             "Failed to read asset Type at index " + std::to_string(i));
                }
                
                stream.ReadRaw(assetInfo.Flags);
                if (!stream.IsStreamGood())
                {
                    OLO_CORE_ERROR("AssetPack::Load - Failed to read Flags for asset {} of {}", i + 1, m_AssetPackFile.Index.AssetCount);
                    return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                             "Failed to read asset Flags at index " + std::to_string(i));
                }
                
                // Populate lookup map for O(1) asset queries
                m_AssetLookupMap[assetInfo.Handle] = assetInfo;
            }

            // Read scene infos
            m_AssetPackFile.SceneInfos.resize(m_AssetPackFile.Index.SceneCount);
            for (u32 i = 0; i < m_AssetPackFile.Index.SceneCount; i++)
            {
                auto& sceneInfo = m_AssetPackFile.SceneInfos[i];
                stream.ReadRaw(sceneInfo.Handle);
                if (!stream.IsStreamGood())
                {
                    OLO_CORE_ERROR("AssetPack::Load - Failed to read Handle for scene {} of {}", i + 1, m_AssetPackFile.Index.SceneCount);
                    return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                             "Failed to read scene Handle at index " + std::to_string(i));
                }
                
                stream.ReadRaw(sceneInfo.PackedOffset);
                if (!stream.IsStreamGood())
                {
                    OLO_CORE_ERROR("AssetPack::Load - Failed to read PackedOffset for scene {} of {}", i + 1, m_AssetPackFile.Index.SceneCount);
                    return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                             "Failed to read scene PackedOffset at index " + std::to_string(i));
                }
                
                stream.ReadRaw(sceneInfo.PackedSize);
                if (!stream.IsStreamGood())
                {
                    OLO_CORE_ERROR("AssetPack::Load - Failed to read PackedSize for scene {} of {}", i + 1, m_AssetPackFile.Index.SceneCount);
                    return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                             "Failed to read scene PackedSize at index " + std::to_string(i));
                }
                
                stream.ReadRaw(sceneInfo.Flags);
                if (!stream.IsStreamGood())
                {
                    OLO_CORE_ERROR("AssetPack::Load - Failed to read Flags for scene {} of {}", i + 1, m_AssetPackFile.Index.SceneCount);
                    return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                             "Failed to read scene Flags at index " + std::to_string(i));
                }
                
                // Read scene assets map
                u32 assetCount;
                stream.ReadRaw(assetCount);
                if (!stream.IsStreamGood())
                {
                    OLO_CORE_ERROR("AssetPack::Load - Failed to read asset count for scene {} of {}", i + 1, m_AssetPackFile.Index.SceneCount);
                    return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                             "Failed to read asset count for scene at index " + std::to_string(i));
                }
                
                // Validate assetCount to prevent unbounded iterations from corrupted/malicious files
                static constexpr u32 MAX_SCENE_ASSET_COUNT = 100000; // 100 thousand assets per scene maximum
                if (assetCount > MAX_SCENE_ASSET_COUNT)
                {
                    OLO_CORE_ERROR("AssetPack::Load - Asset count ({}) for scene {} exceeds maximum allowed ({})", 
                                 assetCount, i + 1, MAX_SCENE_ASSET_COUNT);
                    return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                             "Asset count for scene exceeds maximum allowed limit");
                }
                
                for (u32 j = 0; j < assetCount; j++)
                {
                    AssetHandle assetHandle;
                    AssetPackFile::AssetInfo assetInfo;
                    
                    stream.ReadRaw(assetHandle);
                    if (!stream.IsStreamGood())
                    {
                        OLO_CORE_ERROR("AssetPack::Load - Failed to read asset handle {} of {} for scene {}", j + 1, assetCount, i + 1);
                        return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                                 "Failed to read asset handle " + std::to_string(j) + " for scene " + std::to_string(i));
                    }
                    
                    stream.ReadRaw(assetInfo.Handle);
                    if (!stream.IsStreamGood())
                    {
                        OLO_CORE_ERROR("AssetPack::Load - Failed to read asset info Handle {} of {} for scene {}", j + 1, assetCount, i + 1);
                        return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                                 "Failed to read asset info Handle " + std::to_string(j) + " for scene " + std::to_string(i));
                    }
                    
                    stream.ReadRaw(assetInfo.PackedOffset);
                    if (!stream.IsStreamGood())
                    {
                        OLO_CORE_ERROR("AssetPack::Load - Failed to read asset info PackedOffset {} of {} for scene {}", j + 1, assetCount, i + 1);
                        return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                                 "Failed to read asset info PackedOffset " + std::to_string(j) + " for scene " + std::to_string(i));
                    }
                    
                    stream.ReadRaw(assetInfo.PackedSize);
                    if (!stream.IsStreamGood())
                    {
                        OLO_CORE_ERROR("AssetPack::Load - Failed to read asset info PackedSize {} of {} for scene {}", j + 1, assetCount, i + 1);
                        return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                                 "Failed to read asset info PackedSize " + std::to_string(j) + " for scene " + std::to_string(i));
                    }
                    
                    stream.ReadRaw(assetInfo.Type);
                    if (!stream.IsStreamGood())
                    {
                        OLO_CORE_ERROR("AssetPack::Load - Failed to read asset info Type {} of {} for scene {}", j + 1, assetCount, i + 1);
                        return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                                 "Failed to read asset info Type " + std::to_string(j) + " for scene " + std::to_string(i));
                    }
                    
                    stream.ReadRaw(assetInfo.Flags);
                    if (!stream.IsStreamGood())
                    {
                        OLO_CORE_ERROR("AssetPack::Load - Failed to read asset info Flags {} of {} for scene {}", j + 1, assetCount, i + 1);
                        return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                                 "Failed to read asset info Flags " + std::to_string(j) + " for scene " + std::to_string(i));
                    }
                    
                    sceneInfo.Assets[assetHandle] = assetInfo;
                }
            }
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("AssetPack::Load - Failed to read index table: {}", e.what());
            return AssetPackLoadResult(AssetPackLoadError::CorruptIndex, 
                                     "Failed to read asset index table: " + std::string(e.what()));
        }
        
        // All validations passed, safe to update object state
        m_PackPath = path;
        m_IsLoaded = true;
        
        auto endTime = std::chrono::high_resolution_clock::now();
        AssetPackLoadResult result(true);
        result.LoadTimeMs = std::chrono::duration<f64, std::milli>(endTime - startTime).count();
        
        OLO_CORE_INFO("AssetPack::Load - Successfully loaded pack: {} ({} assets, {} scenes) in {:.2f}ms", 
                     path.string(), m_AssetPackFile.Index.AssetCount, m_AssetPackFile.Index.SceneCount, result.LoadTimeMs);
        
        return result;
    }

    bool AssetPack::LoadLegacy(const std::filesystem::path& path)
    {
        AssetPackLoadResult result = Load(path);
        if (!result.Success)
        {
            // Log the detailed error for debugging
            OLO_CORE_ERROR("AssetPack::LoadLegacy - Load failed: {} (Code: {})", 
                         result.ErrorMessage, static_cast<int>(result.ErrorCode));
        }
        return result.Success;
    }

    void AssetPack::Unload() noexcept
    {
        m_AssetPackFile = {};
        m_PackPath.clear();
        m_IsLoaded = false;
        m_AssetLookupMap.clear();
        
        OLO_CORE_INFO("AssetPack::Unload - Asset pack unloaded");
    }

    bool AssetPack::IsAssetAvailable(AssetHandle handle) const
    {
        if (!m_IsLoaded)
            return false;

        return m_AssetLookupMap.find(handle) != m_AssetLookupMap.end();
    }

    AssetType AssetPack::GetAssetType(AssetHandle handle) const
    {
        if (!m_IsLoaded)
            return AssetType::None;

        auto it = m_AssetLookupMap.find(handle);
        if (it != m_AssetLookupMap.end())
            return it->second.Type;
        
        return AssetType::None;
    }

    std::optional<AssetPackFile::AssetInfo> AssetPack::GetAssetInfo(AssetHandle handle) const
    {
        if (!m_IsLoaded)
            return std::nullopt;

        auto it = m_AssetLookupMap.find(handle);
        if (it != m_AssetLookupMap.end())
            return it->second;
        
        return std::nullopt;
    }

    FileStreamReaderPtr AssetPack::GetAssetStreamReader() const
    {
        if (!m_IsLoaded || m_PackPath.empty())
            return nullptr;

        // Verify that the file exists before attempting to create a stream reader
        if (!std::filesystem::exists(m_PackPath))
        {
            OLO_CORE_ERROR("AssetPack::GetAssetStreamReader - Asset pack file does not exist at path: {}", m_PackPath.string());
            return nullptr;
        }

        try
        {
            FileStreamReaderPtr reader(new FileStreamReader(m_PackPath));
            
            // Validate that the FileStreamReader was created successfully and is in a good state
            if (!reader || !reader->IsStreamGood())
            {
                OLO_CORE_ERROR("AssetPack::GetAssetStreamReader - FileStreamReader is not in a valid state for path: {}", m_PackPath.string());
                return nullptr;
            }
            
            return reader;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("AssetPack::GetAssetStreamReader - Exception occurred while creating FileStreamReader for path: {} - {}", m_PackPath.string(), e.what());
            return nullptr;
        }
        catch (...)
        {
            OLO_CORE_ERROR("AssetPack::GetAssetStreamReader - Unknown exception occurred while creating FileStreamReader for path: {}", m_PackPath.string());
            return nullptr;
        }
    }

    const std::vector<AssetPackFile::AssetInfo>& AssetPack::GetAllAssetInfos() const
    {
        if (!m_IsLoaded)
        {
            static const std::vector<AssetPackFile::AssetInfo> emptyAssetInfos;
            return emptyAssetInfos;
        }

        return m_AssetPackFile.AssetInfos;
    }

    const std::vector<AssetPackFile::SceneInfo>& AssetPack::GetAllSceneInfos() const
    {
        if (!m_IsLoaded)
        {
            static const std::vector<AssetPackFile::SceneInfo> emptySceneInfos;
            return emptySceneInfos;
        }

        return m_AssetPackFile.SceneInfos;
    }

} // namespace OloEngine
