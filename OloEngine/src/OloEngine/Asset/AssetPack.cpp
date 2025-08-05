#include "olopch.h"
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
        if (!stream.ReadRaw(m_AssetPackFile.FileHeader))
        {
            OLO_CORE_ERROR("AssetPack::Load - Failed to read file header");
            return false;
        }

        // Validate magic number
        if (m_AssetPackFile.FileHeader.MagicNumber != AssetPackFile::MagicNumber)
        {
            OLO_CORE_ERROR("AssetPack::Load - Invalid magic number: {:#x}", m_AssetPackFile.FileHeader.MagicNumber);
            return false;
        }

        // Validate version
        if (m_AssetPackFile.FileHeader.Version != AssetPackFile::Version)
        {
            OLO_CORE_WARN("AssetPack::Load - Version mismatch. Expected: {}, Got: {}", 
                         AssetPackFile::Version, m_AssetPackFile.FileHeader.Version);
            // Continue loading - might be compatible
        }

        // Seek to index table
        stream.SetStreamPosition(m_AssetPackFile.FileHeader.IndexOffset);

        // Read index table header
        if (!stream.ReadRaw(m_AssetPackFile.Index))
        {
            OLO_CORE_ERROR("AssetPack::Load - Failed to read index table");
            return false;
        }

        // Read asset infos
        m_AssetPackFile.AssetInfos.resize(m_AssetPackFile.Index.AssetCount);
        for (uint32_t i = 0; i < m_AssetPackFile.Index.AssetCount; i++)
        {
            if (!stream.ReadRaw(m_AssetPackFile.AssetInfos[i]))
            {
                OLO_CORE_ERROR("AssetPack::Load - Failed to read asset info {}", i);
                return false;
            }
        }

        // Read scene infos
        m_AssetPackFile.SceneInfos.resize(m_AssetPackFile.Index.SceneCount);
        for (uint32_t i = 0; i < m_AssetPackFile.Index.SceneCount; i++)
        {
            if (!stream.ReadRaw(m_AssetPackFile.SceneInfos[i]))
            {
                OLO_CORE_ERROR("AssetPack::Load - Failed to read scene info {}", i);
                return false;
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
