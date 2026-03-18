#include "OloEnginePCH.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/AI/BehaviorTree/BehaviorTreeAsset.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Scene.h"

#include <fstream>
#include <sstream>

#include <yaml-cpp/yaml.h>

namespace OloEngine
{
    void BehaviorTreeSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto btAsset = asset.As<BehaviorTreeAsset>();
        if (!btAsset)
        {
            OLO_CORE_ERROR("BehaviorTreeSerializer::Serialize - Failed to cast asset to BehaviorTreeAsset ({})", metadata.FilePath.string());
            return;
        }

        std::string yamlString = SerializeToYAML(btAsset);
        auto fullPath = Project::GetAssetDirectory() / metadata.FilePath;

        std::error_code ec;
        std::filesystem::create_directories(fullPath.parent_path(), ec);
        if (ec)
        {
            OLO_CORE_ERROR("BehaviorTreeSerializer::Serialize - Failed to create directories for ({}): {}", fullPath.string(), ec.message());
            return;
        }

        std::ofstream fout(fullPath);
        if (!fout.is_open())
        {
            OLO_CORE_ERROR("BehaviorTreeSerializer::Serialize - Failed to open file for writing ({})", fullPath.string());
            return;
        }
        fout << yamlString;
    }

    bool BehaviorTreeSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto path = Project::GetAssetDirectory() / metadata.FilePath;
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_WARN("BehaviorTreeSerializer::TryLoadData - File does not exist ({})", path.string());
            return false;
        }

        std::ifstream file(path);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("BehaviorTreeSerializer::TryLoadData - Failed to open file ({})", path.string());
            return false;
        }

        std::stringstream ss;
        ss << file.rdbuf();

        auto btAsset = Ref<BehaviorTreeAsset>::Create();
        if (!DeserializeFromYAML(ss.str(), btAsset))
        {
            OLO_CORE_ERROR("BehaviorTreeSerializer::TryLoadData - Failed to deserialize ({})", path.string());
            return false;
        }

        btAsset->SetHandle(metadata.Handle);
        asset = btAsset;
        return true;
    }

    bool BehaviorTreeSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        auto btAsset = AssetManager::GetAsset<BehaviorTreeAsset>(handle);
        if (!btAsset)
        {
            OLO_CORE_ERROR("BehaviorTreeSerializer::SerializeToAssetPack - Failed to get asset ({})", static_cast<u64>(handle));
            return false;
        }

        std::string yamlString = SerializeToYAML(btAsset);
        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yamlString);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> BehaviorTreeSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        stream.SetStreamPosition(assetInfo.PackedOffset);
        std::string yamlString;
        stream.ReadString(yamlString);

        auto btAsset = Ref<BehaviorTreeAsset>::Create();
        if (!DeserializeFromYAML(yamlString, btAsset))
        {
            OLO_CORE_ERROR("BehaviorTreeSerializer::DeserializeFromAssetPack - Failed to deserialize YAML (handle: {})", static_cast<u64>(assetInfo.Handle));
            return nullptr;
        }

        btAsset->SetHandle(assetInfo.Handle);
        return btAsset;
    }

    std::string BehaviorTreeSerializer::SerializeToYAML(const Ref<BehaviorTreeAsset>& btAsset) const
    {
        OLO_PROFILE_FUNCTION();

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "BehaviorTree" << YAML::Value;
        out << YAML::BeginMap;

        out << YAML::Key << "RootNodeID" << YAML::Value << btAsset->GetRootNodeID();

        // Blackboard key definitions
        if (!btAsset->BlackboardKeyDefs.empty())
        {
            out << YAML::Key << "BlackboardKeyDefs" << YAML::Value << YAML::BeginMap;
            for (const auto& [key, typeHint] : btAsset->BlackboardKeyDefs)
            {
                out << YAML::Key << key << YAML::Value << typeHint;
            }
            out << YAML::EndMap;
        }

        // Nodes
        out << YAML::Key << "Nodes" << YAML::Value << YAML::BeginSeq;
        for (const auto& node : btAsset->GetNodes())
        {
            out << YAML::BeginMap;
            out << YAML::Key << "ID" << YAML::Value << node.ID;
            out << YAML::Key << "TypeName" << YAML::Value << node.TypeName;
            out << YAML::Key << "Name" << YAML::Value << node.Name;

            if (!node.ChildIDs.empty())
            {
                out << YAML::Key << "ChildIDs" << YAML::Value << YAML::Flow << YAML::BeginSeq;
                for (const auto& childID : node.ChildIDs)
                {
                    out << childID;
                }
                out << YAML::EndSeq;
            }

            if (!node.Properties.empty())
            {
                out << YAML::Key << "Properties" << YAML::Value << YAML::BeginMap;
                for (const auto& [key, value] : node.Properties)
                {
                    out << YAML::Key << key << YAML::Value << value;
                }
                out << YAML::EndMap;
            }

            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        out << YAML::EndMap; // BehaviorTree
        out << YAML::EndMap; // Root

        return out.c_str();
    }

    bool BehaviorTreeSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<BehaviorTreeAsset>& btAsset) const
    {
        OLO_PROFILE_FUNCTION();

        YAML::Node data;
        try
        {
            data = YAML::Load(yamlString);
        }
        catch (const YAML::ParserException& e)
        {
            OLO_CORE_ERROR("BehaviorTreeSerializer - YAML parse error: {}", e.what());
            return false;
        }

        auto btNode = data["BehaviorTree"];
        if (!btNode)
        {
            OLO_CORE_ERROR("BehaviorTreeSerializer - Missing 'BehaviorTree' root node");
            return false;
        }

        if (!btNode["RootNodeID"])
        {
            OLO_CORE_ERROR("BehaviorTreeSerializer - Missing 'RootNodeID'");
            return false;
        }

        try
        {
            btAsset->SetRootNodeID(btNode["RootNodeID"].as<u64>());
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("BehaviorTreeSerializer - Failed to parse RootNodeID: {}", e.what());
            return false;
        }

        // Blackboard key definitions
        if (auto bbDefs = btNode["BlackboardKeyDefs"]; bbDefs && bbDefs.IsMap())
        {
            for (const auto& pair : bbDefs)
            {
                btAsset->BlackboardKeyDefs[pair.first.as<std::string>()] = pair.second.as<std::string>();
            }
        }

        // Nodes
        if (auto nodesNode = btNode["Nodes"]; nodesNode && nodesNode.IsSequence())
        {
            for (const auto& nodeYAML : nodesNode)
            {
                BTNodeData nodeData;
                try
                {
                    if (!nodeYAML["ID"] || !nodeYAML["TypeName"])
                    {
                        OLO_CORE_ERROR("BehaviorTreeSerializer - Node missing required ID or TypeName field");
                        return false;
                    }
                    nodeData.ID = nodeYAML["ID"].as<u64>();
                    nodeData.TypeName = nodeYAML["TypeName"].as<std::string>();
                }
                catch (const YAML::Exception& e)
                {
                    OLO_CORE_ERROR("BehaviorTreeSerializer - Failed to parse node: {}", e.what());
                    return false;
                }

                nodeData.Name = nodeYAML["Name"].as<std::string>("");

                if (auto childIDs = nodeYAML["ChildIDs"]; childIDs && childIDs.IsSequence())
                {
                    for (const auto& childID : childIDs)
                    {
                        nodeData.ChildIDs.push_back(childID.as<u64>());
                    }
                }

                if (auto props = nodeYAML["Properties"]; props && props.IsMap())
                {
                    for (const auto& pair : props)
                    {
                        nodeData.Properties[pair.first.as<std::string>()] = pair.second.as<std::string>();
                    }
                }

                btAsset->AddNode(std::move(nodeData));
            }
        }

        return true;
    }
} // namespace OloEngine
