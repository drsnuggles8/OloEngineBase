#include "OloEnginePCH.h"
#include "AssetPack.h"

#include "OloEngine/Serialization/FileStream.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine
{
    bool AssetPack::Load(const std::filesystem::path& path)
    {
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_ERROR("AssetPack::Load - File does not exist: {}", path.string());
            return false;
        }

        m_PackPath = path;
        
        // Open file for reading
        FileStreamReader stream(path);
        if (!stream.IsStreamGood())
        {
            OLO_CORE_ERROR("AssetPack::Load - Failed to open file: {}", path.string());
            return false;
        }

        // Read file header
        stream.ReadRaw(m_AssetPackFile.Header.MagicNumber);
        stream.ReadRaw(m_AssetPackFile.Header.Version);
        stream.ReadRaw(m_AssetPackFile.Header.BuildVersion);
        stream.ReadRaw(m_AssetPackFile.Header.IndexOffset);

        // Validate magic number
        if (m_AssetPackFile.Header.MagicNumber != AssetPackFile::MagicNumber)
        {
            OLO_CORE_ERROR("AssetPack::Load - Invalid magic number: {:#x}", m_AssetPackFile.Header.MagicNumber);
            return false;
        }

        // Validate version
        if (m_AssetPackFile.Header.Version != AssetPackFile::Version)
        {
            OLO_CORE_WARN("AssetPack::Load - Version mismatch. Expected: {}, Got: {}", 
                         AssetPackFile::Version, m_AssetPackFile.Header.Version);
            // Continue loading - might be compatible
        }

        // Seek to index table
        stream.SetStreamPosition(m_AssetPackFile.Header.IndexOffset);

        // Read index table header
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

        m_IsLoaded = true;
        
        OLO_CORE_INFO("AssetPack::Load - Successfully loaded pack: {} ({} assets, {} scenes)", 
                     path.string(), m_AssetPackFile.Index.AssetCount, m_AssetPackFile.Index.SceneCount);
        
        return true;
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

    std::unique_ptr<FileStreamReader> AssetPack::GetAssetStreamReader() const
    {
        if (!m_IsLoaded || m_PackPath.empty())
            return nullptr;

        return std::make_unique<FileStreamReader>(m_PackPath);
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
