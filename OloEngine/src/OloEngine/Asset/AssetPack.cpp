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

        m_PackPath = path;
        
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
            stream.ReadRaw(m_AssetPackFile.Header.Version);
            stream.ReadRaw(m_AssetPackFile.Header.BuildVersion);
            stream.ReadRaw(m_AssetPackFile.Header.IndexOffset);
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

        // Seek to index table
        stream.SetStreamPosition(m_AssetPackFile.Header.IndexOffset);

        // Read index table header with error checking
        try
        {
            stream.ReadRaw(m_AssetPackFile.Index.AssetCount);
            stream.ReadRaw(m_AssetPackFile.Index.SceneCount);
            stream.ReadRaw(m_AssetPackFile.Index.PackedAppBinaryOffset);
            stream.ReadRaw(m_AssetPackFile.Index.PackedAppBinarySize);

            // Read asset infos
            m_AssetPackFile.AssetInfos.resize(m_AssetPackFile.Index.AssetCount);
            for (u32 i = 0; i < m_AssetPackFile.Index.AssetCount; i++)
            {
                auto& assetInfo = m_AssetPackFile.AssetInfos[i];
                stream.ReadRaw(assetInfo.Handle);
                stream.ReadRaw(assetInfo.PackedOffset);
                stream.ReadRaw(assetInfo.PackedSize);
                stream.ReadRaw(assetInfo.Type);
                stream.ReadRaw(assetInfo.Flags);
            }

            // Read scene infos
            m_AssetPackFile.SceneInfos.resize(m_AssetPackFile.Index.SceneCount);
            for (u32 i = 0; i < m_AssetPackFile.Index.SceneCount; i++)
            {
                auto& sceneInfo = m_AssetPackFile.SceneInfos[i];
                stream.ReadRaw(sceneInfo.Handle);
                stream.ReadRaw(sceneInfo.PackedOffset);
                stream.ReadRaw(sceneInfo.PackedSize);
                stream.ReadRaw(sceneInfo.Flags);
                
                // Read scene assets map
                u32 assetCount;
                stream.ReadRaw(assetCount);
                for (u32 j = 0; j < assetCount; j++)
                {
                    AssetHandle assetHandle;
                    AssetPackFile::AssetInfo assetInfo;
                    
                    stream.ReadRaw(assetHandle);
                    stream.ReadRaw(assetInfo.Handle);
                    stream.ReadRaw(assetInfo.PackedOffset);
                    stream.ReadRaw(assetInfo.PackedSize);
                    stream.ReadRaw(assetInfo.Type);
                    stream.ReadRaw(assetInfo.Flags);
                    
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

    void AssetPack::Unload()
    {
        m_AssetPackFile = {};
        m_PackPath.clear();
        m_IsLoaded = false;
        
        OLO_CORE_INFO("AssetPack::Unload - Asset pack unloaded");
    }

    bool AssetPack::IsAssetAvailable(AssetHandle handle) const
    {
        if (!m_IsLoaded)
            return false;

        for (const auto& assetInfo : m_AssetPackFile.AssetInfos)
        {
            if (assetInfo.Handle == handle)
                return true;
        }
        
        return false;
    }

    AssetType AssetPack::GetAssetType(AssetHandle handle) const
    {
        if (!m_IsLoaded)
            return AssetType::None;

        for (const auto& assetInfo : m_AssetPackFile.AssetInfos)
        {
            if (assetInfo.Handle == handle)
                return assetInfo.Type;
        }
        
        return AssetType::None;
    }

    std::optional<AssetPackFile::AssetInfo> AssetPack::GetAssetInfo(AssetHandle handle) const
    {
        if (!m_IsLoaded)
            return std::nullopt;

        for (const auto& assetInfo : m_AssetPackFile.AssetInfos)
        {
            if (assetInfo.Handle == handle)
                return assetInfo;
        }
        
        return std::nullopt;
    }

    FileStreamReaderPtr AssetPack::GetAssetStreamReader() const
    {
        if (!m_IsLoaded || m_PackPath.empty())
            return nullptr;

        return FileStreamReaderPtr(new FileStreamReader(m_PackPath));
    }

    const std::vector<AssetPackFile::AssetInfo>& AssetPack::GetAllAssetInfos() const
    {
        return m_AssetPackFile.AssetInfos;
    }

    const std::vector<AssetPackFile::SceneInfo>& AssetPack::GetAllSceneInfos() const
    {
        return m_AssetPackFile.SceneInfos;
    }

} // namespace OloEngine
