#include "OloEnginePCH.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Scene.h" // AssetSerializer.h forward-declares Scene; pulling it in resolves Ref<Scene>::~Ref instantiation chains.
#include "OloEngine/Tilemap/Tileset.h"

#include <fstream>
#include <sstream>

#include <yaml-cpp/yaml.h>

namespace OloEngine
{
    std::string TilesetSerializer::SerializeToYAML(const Ref<Tileset>& tileset) const
    {
        OLO_PROFILE_FUNCTION();

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "Tileset" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Texture" << YAML::Value << static_cast<u64>(tileset->GetTextureHandle());
        out << YAML::Key << "TileWidth" << YAML::Value << tileset->GetTileWidth();
        out << YAML::Key << "TileHeight" << YAML::Value << tileset->GetTileHeight();
        out << YAML::Key << "Spacing" << YAML::Value << tileset->GetSpacing();
        out << YAML::Key << "Margin" << YAML::Value << tileset->GetMargin();

        out << YAML::Key << "Tiles" << YAML::Value << YAML::BeginSeq;
        const auto& tiles = tileset->GetTiles();
        for (sizet i = 0; i < tiles.size(); ++i)
        {
            const auto& info = tiles[i];
            // Skip fully-default records: a 4096-tile atlas with a handful of solid
            // tiles should not write 4096 near-empty maps. The Index key is what
            // makes the sparse form readable back.
            if (!info.Solid && info.Flags == 0 && info.Type.empty())
                continue;
            out << YAML::BeginMap;
            out << YAML::Key << "Index" << YAML::Value << static_cast<u32>(i);
            out << YAML::Key << "Solid" << YAML::Value << info.Solid;
            if (info.Flags != 0)
                out << YAML::Key << "Flags" << YAML::Value << info.Flags;
            if (!info.Type.empty())
                out << YAML::Key << "Type" << YAML::Value << info.Type;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        out << YAML::EndMap;
        out << YAML::EndMap;
        return std::string(out.c_str());
    }

    bool TilesetSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<Tileset>& tileset) const
    {
        OLO_PROFILE_FUNCTION();

        // A tile index this far out is authoring corruption, not a big atlas: the
        // metadata vector is dense up to the highest index, so an unbounded value
        // would be an allocation bomb. 1M tiles is a 1024x1024-tile atlas.
        constexpr u32 kMaxTileIndex = 1u << 20;

        try
        {
            YAML::Node root = YAML::Load(yamlString);
            auto section = root["Tileset"];
            if (!section)
                return false;

            if (auto n = section["Texture"]; n)
                tileset->SetTextureHandle(n.as<u64>());

            const u32 tileWidth = section["TileWidth"] ? section["TileWidth"].as<u32>() : 16u;
            const u32 tileHeight = section["TileHeight"] ? section["TileHeight"].as<u32>() : 16u;
            // A zero tile dimension makes the slicer report an empty atlas; refuse
            // the file outright rather than loading a tileset that can never
            // produce a UV.
            if (tileWidth == 0 || tileHeight == 0)
            {
                OLO_CORE_ERROR("TilesetSerializer - tile size must be non-zero (got {}x{}).", tileWidth, tileHeight);
                return false;
            }
            tileset->SetTileSize(tileWidth, tileHeight);

            if (auto n = section["Spacing"]; n)
                tileset->SetSpacing(n.as<u32>());
            if (auto n = section["Margin"]; n)
                tileset->SetMargin(n.as<u32>());

            if (auto tiles = section["Tiles"]; tiles && tiles.IsSequence())
            {
                for (const auto& node : tiles)
                {
                    auto indexNode = node["Index"];
                    if (!indexNode)
                        continue;
                    const u32 index = indexNode.as<u32>();
                    if (index > kMaxTileIndex)
                    {
                        OLO_CORE_WARN("TilesetSerializer - dropping tile metadata at out-of-range index {}.", index);
                        continue;
                    }
                    TileInfo info;
                    if (auto n = node["Solid"]; n)
                        info.Solid = n.as<bool>();
                    if (auto n = node["Flags"]; n)
                        info.Flags = n.as<u32>();
                    if (auto n = node["Type"]; n)
                        info.Type = n.as<std::string>();
                    tileset->SetTileInfo(index, info);
                }
            }
            return true;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("TilesetSerializer::DeserializeFromYAML - {}", e.what());
            return false;
        }
    }

    void TilesetSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto tileset = asset.As<Tileset>();
        if (!tileset)
        {
            OLO_CORE_ERROR("TilesetSerializer::Serialize - asset cast failed ({})", metadata.FilePath.string());
            return;
        }

        const std::string yamlString = SerializeToYAML(tileset);
        // metadata.FilePath is stored PROJECT-relative and already carries the
        // "Assets/" segment (see the registry paths in AssetRegistry.oar), so it
        // joins onto the project directory. Joining onto GetAssetDirectory()
        // instead yields ".../Assets/Assets/..." and every load misses.
        auto fullPath = Project::GetProjectDirectory() / metadata.FilePath;

        std::error_code ec;
        std::filesystem::create_directories(fullPath.parent_path(), ec);
        if (ec)
        {
            OLO_CORE_ERROR("TilesetSerializer::Serialize - mkdir failed ({}): {}", fullPath.string(), ec.message());
            return;
        }

        std::ofstream fout(fullPath);
        if (!fout.is_open())
        {
            OLO_CORE_ERROR("TilesetSerializer::Serialize - open-for-write failed ({})", fullPath.string());
            return;
        }
        fout << yamlString;
    }

    bool TilesetSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto path = Project::GetProjectDirectory() / metadata.FilePath;
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_WARN("TilesetSerializer::TryLoadData - file missing ({})", path.string());
            return false;
        }

        std::ifstream file(path);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("TilesetSerializer::TryLoadData - open failed ({})", path.string());
            return false;
        }

        std::stringstream ss;
        ss << file.rdbuf();

        auto tileset = Ref<Tileset>::Create();
        if (!DeserializeFromYAML(ss.str(), tileset))
        {
            OLO_CORE_ERROR("TilesetSerializer::TryLoadData - YAML parse failed ({})", path.string());
            return false;
        }

        tileset->SetHandle(metadata.Handle);
        asset = tileset;
        return true;
    }

    bool TilesetSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        auto tileset = AssetManager::GetAsset<Tileset>(handle);
        if (!tileset)
        {
            OLO_CORE_ERROR("TilesetSerializer::SerializeToAssetPack - get-asset failed ({})", static_cast<u64>(handle));
            return false;
        }
        const std::string yamlString = SerializeToYAML(tileset);
        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yamlString);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> TilesetSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        stream.SetStreamPosition(assetInfo.PackedOffset);
        std::string yamlString;
        stream.ReadString(yamlString);

        auto tileset = Ref<Tileset>::Create();
        if (!DeserializeFromYAML(yamlString, tileset))
        {
            OLO_CORE_ERROR("TilesetSerializer::DeserializeFromAssetPack - YAML parse failed (handle: {})", static_cast<u64>(assetInfo.Handle));
            return nullptr;
        }
        tileset->SetHandle(assetInfo.Handle);
        return tileset;
    }
} // namespace OloEngine
