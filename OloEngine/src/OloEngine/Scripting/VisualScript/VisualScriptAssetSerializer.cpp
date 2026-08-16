#include "OloEnginePCH.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptGraph.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptSerializer.h"
#include "OloEngine/Serialization/FileStream.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace OloEngine
{
    using VisualScript::VisualScriptAsset;
    using VisualScript::VisualScriptSerializer;

    void VisualScriptAssetSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto graph = asset.As<VisualScriptAsset>();
        if (!graph)
        {
            OLO_CORE_ERROR("VisualScriptAssetSerializer::Serialize - asset is not a VisualScriptAsset ({})", metadata.FilePath.string());
            return;
        }

        const auto fullPath = Project::GetProjectDirectory() / metadata.FilePath;
        std::error_code ec;
        std::filesystem::create_directories(fullPath.parent_path(), ec);
        if (ec)
        {
            OLO_CORE_ERROR("VisualScriptAssetSerializer::Serialize - cannot create directories for ({}): {}", fullPath.string(), ec.message());
            return;
        }

        if (!VisualScriptSerializer::Serialize(*graph, fullPath))
        {
            OLO_CORE_ERROR("VisualScriptAssetSerializer::Serialize - write failed ({})", fullPath.string());
        }
    }

    bool VisualScriptAssetSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        const auto path = Project::GetProjectDirectory() / metadata.FilePath;
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_WARN("VisualScriptAssetSerializer::TryLoadData - file does not exist ({})", path.string());
            return false;
        }

        auto graph = Ref<VisualScriptAsset>::Create();
        if (!VisualScriptSerializer::Deserialize(*graph, path))
        {
            OLO_CORE_ERROR("VisualScriptAssetSerializer::TryLoadData - deserialize failed ({})", path.string());
            return false;
        }

        graph->SetHandle(metadata.Handle);
        asset = graph;
        return true;
    }

    bool VisualScriptAssetSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        auto graph = AssetManager::GetAsset<VisualScriptAsset>(handle);
        if (!graph)
        {
            OLO_CORE_ERROR("VisualScriptAssetSerializer::SerializeToAssetPack - cannot resolve asset ({})", static_cast<u64>(handle));
            return false;
        }

        const std::string yaml = VisualScriptSerializer::SerializeToString(*graph);
        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yaml);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> VisualScriptAssetSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        stream.SetStreamPosition(assetInfo.PackedOffset);
        std::string yaml;
        stream.ReadString(yaml);

        auto graph = Ref<VisualScriptAsset>::Create();
        if (!VisualScriptSerializer::DeserializeFromString(*graph, yaml))
        {
            OLO_CORE_ERROR("VisualScriptAssetSerializer::DeserializeFromAssetPack - deserialize failed (handle: {})", static_cast<u64>(assetInfo.Handle));
            return nullptr;
        }

        graph->SetHandle(assetInfo.Handle);
        return graph;
    }

} // namespace OloEngine
