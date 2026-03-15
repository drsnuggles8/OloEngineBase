#include "OloEnginePCH.h"
#include "OloEngine/Dialogue/DialogueTreeSerializer.h"
#include "OloEngine/Dialogue/DialogueTreeAsset.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Scene.h"

#include <fstream>
#include <sstream>
#include <unordered_set>

#include <yaml-cpp/yaml.h>

namespace OloEngine
{
    namespace
    {
        std::string PropertyValueToString(const DialoguePropertyValue& value)
        {
            return std::visit([](const auto& v) -> std::string
                              {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, bool>)
                    return v ? "true" : "false";
                else if constexpr (std::is_same_v<T, i32>)
                    return std::to_string(v);
                else if constexpr (std::is_same_v<T, f32>)
                    return std::to_string(v);
                else if constexpr (std::is_same_v<T, std::string>)
                    return v;
                else
                    return ""; }, value);
        }

        std::string PropertyValueTypeString(const DialoguePropertyValue& value)
        {
            return std::visit([](const auto& v) -> std::string
                              {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, bool>)
                    return "bool";
                else if constexpr (std::is_same_v<T, i32>)
                    return "int";
                else if constexpr (std::is_same_v<T, f32>)
                    return "float";
                else if constexpr (std::is_same_v<T, std::string>)
                    return "string";
                else
                    return "unknown"; }, value);
        }

        DialoguePropertyValue ParsePropertyValue(const std::string& typeStr, const std::string& valueStr)
        {
            try
            {
                if (typeStr == "bool")
                    return valueStr == "true";
                if (typeStr == "int")
                    return static_cast<i32>(std::stoi(valueStr));
                if (typeStr == "float")
                    return std::stof(valueStr);
            }
            catch (const std::exception& e)
            {
                OLO_CORE_WARN("DialogueTreeSerializer - Failed to parse property value '{}' as {}: {}", valueStr, typeStr, e.what());
            }
            return valueStr; // default to string
        }
    } // namespace

    void DialogueTreeSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto dialogueAsset = asset.As<DialogueTreeAsset>();
        if (!dialogueAsset)
        {
            OLO_CORE_ERROR("DialogueTreeSerializer::Serialize - Failed to cast asset to DialogueTreeAsset ({})", metadata.FilePath.string());
            return;
        }

        std::string yamlString = SerializeToYAML(dialogueAsset);
        auto fullPath = Project::GetAssetDirectory() / metadata.FilePath;

        std::error_code ec;
        std::filesystem::create_directories(fullPath.parent_path(), ec);
        if (ec)
        {
            OLO_CORE_ERROR("DialogueTreeSerializer::Serialize - Failed to create directories for ({}): {}", fullPath.string(), ec.message());
            return;
        }

        std::ofstream fout(fullPath);
        if (!fout.is_open())
        {
            OLO_CORE_ERROR("DialogueTreeSerializer::Serialize - Failed to open file for writing ({})", fullPath.string());
            return;
        }
        fout << yamlString;
    }

    bool DialogueTreeSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto path = Project::GetAssetDirectory() / metadata.FilePath;
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_WARN("DialogueTreeSerializer::TryLoadData - File does not exist ({})", path.string());
            return false;
        }

        std::ifstream file(path);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("DialogueTreeSerializer::TryLoadData - Failed to open file ({})", path.string());
            return false;
        }

        std::stringstream ss;
        ss << file.rdbuf();

        auto dialogueAsset = Ref<DialogueTreeAsset>::Create();
        if (!DeserializeFromYAML(ss.str(), dialogueAsset))
        {
            OLO_CORE_ERROR("DialogueTreeSerializer::TryLoadData - Failed to deserialize ({})", path.string());
            return false;
        }

        dialogueAsset->SetHandle(metadata.Handle);
        asset = dialogueAsset;
        return true;
    }

    bool DialogueTreeSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        auto dialogueAsset = AssetManager::GetAsset<DialogueTreeAsset>(handle);
        if (!dialogueAsset)
        {
            OLO_CORE_ERROR("DialogueTreeSerializer::SerializeToAssetPack - Failed to get asset ({})", static_cast<u64>(handle));
            return false;
        }

        std::string yamlString = SerializeToYAML(dialogueAsset);
        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yamlString);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> DialogueTreeSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        stream.SetStreamPosition(assetInfo.PackedOffset);
        std::string yamlString;
        stream.ReadString(yamlString);

        auto dialogueAsset = Ref<DialogueTreeAsset>::Create();
        if (!DeserializeFromYAML(yamlString, dialogueAsset))
        {
            OLO_CORE_ERROR("DialogueTreeSerializer::DeserializeFromAssetPack - Failed to deserialize YAML (handle: {})", static_cast<u64>(assetInfo.Handle));
            return nullptr;
        }

        dialogueAsset->SetHandle(assetInfo.Handle);
        return dialogueAsset;
    }

    std::string DialogueTreeSerializer::SerializeToYAML(const Ref<DialogueTreeAsset>& dialogueAsset) const
    {
        OLO_PROFILE_FUNCTION();

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "DialogueTree" << YAML::Value;
        out << YAML::BeginMap;

        out << YAML::Key << "RootNodeID" << YAML::Value << dialogueAsset->m_RootNodeID;

        // Nodes
        out << YAML::Key << "Nodes" << YAML::Value << YAML::BeginSeq;
        for (const auto& node : dialogueAsset->m_Nodes)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "ID" << YAML::Value << node.ID;
            out << YAML::Key << "Type" << YAML::Value << node.Type;
            out << YAML::Key << "Name" << YAML::Value << node.Name;
            out << YAML::Key << "EditorPosition" << YAML::Value << YAML::Flow << YAML::BeginSeq
                << node.EditorPosition.x << node.EditorPosition.y << YAML::EndSeq;

            if (!node.Properties.empty())
            {
                out << YAML::Key << "Properties" << YAML::Value << YAML::BeginMap;
                for (const auto& [key, value] : node.Properties)
                {
                    out << YAML::Key << key << YAML::Value << YAML::BeginMap;
                    out << YAML::Key << "type" << YAML::Value << PropertyValueTypeString(value);
                    out << YAML::Key << "value" << YAML::Value << PropertyValueToString(value);
                    out << YAML::EndMap;
                }
                out << YAML::EndMap;
            }

            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        // Connections
        out << YAML::Key << "Connections" << YAML::Value << YAML::BeginSeq;
        for (const auto& conn : dialogueAsset->m_Connections)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "SourceNodeID" << YAML::Value << conn.SourceNodeID;
            out << YAML::Key << "TargetNodeID" << YAML::Value << conn.TargetNodeID;
            out << YAML::Key << "SourcePort" << YAML::Value << conn.SourcePort;
            out << YAML::Key << "TargetPort" << YAML::Value << conn.TargetPort;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        out << YAML::EndMap; // DialogueTree
        out << YAML::EndMap; // Root

        return out.c_str();
    }

    bool DialogueTreeSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<DialogueTreeAsset>& dialogueAsset) const
    {
        OLO_PROFILE_FUNCTION();

        YAML::Node data;
        try
        {
            data = YAML::Load(yamlString);
        }
        catch (const YAML::ParserException& e)
        {
            OLO_CORE_ERROR("DialogueTreeSerializer - YAML parse error: {}", e.what());
            return false;
        }

        auto dialogueTree = data["DialogueTree"];
        if (!dialogueTree)
        {
            OLO_CORE_ERROR("DialogueTreeSerializer - Missing 'DialogueTree' root node");
            return false;
        }

        // Root node ID
        if (!dialogueTree["RootNodeID"])
        {
            OLO_CORE_ERROR("DialogueTreeSerializer - Missing 'RootNodeID'");
            return false;
        }

        UUID localRootID;
        std::vector<DialogueNodeData> localNodes;
        std::vector<DialogueConnection> localConnections;

        try
        {
            localRootID = dialogueTree["RootNodeID"].as<u64>();
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("DialogueTreeSerializer - Failed to parse RootNodeID: {}", e.what());
            return false;
        }

        // Nodes
        std::unordered_set<u64> nodeIDs;
        if (auto nodesNode = dialogueTree["Nodes"]; nodesNode && nodesNode.IsSequence())
        {
            for (const auto& nodeYAML : nodesNode)
            {
                DialogueNodeData node;
                try
                {
                    if (!nodeYAML["ID"] || !nodeYAML["Type"])
                    {
                        OLO_CORE_ERROR("DialogueTreeSerializer - Node missing required ID or Type field");
                        return false;
                    }
                    node.ID = nodeYAML["ID"].as<u64>();
                    node.Type = nodeYAML["Type"].as<std::string>();
                }
                catch (const YAML::Exception& e)
                {
                    OLO_CORE_ERROR("DialogueTreeSerializer - Failed to parse node ID/Type: {}", e.what());
                    return false;
                }

                // Check for duplicate node IDs
                if (nodeIDs.contains(static_cast<u64>(node.ID)))
                {
                    OLO_CORE_ERROR("DialogueTreeSerializer - Duplicate node ID: {}", static_cast<u64>(node.ID));
                    return false;
                }
                nodeIDs.insert(static_cast<u64>(node.ID));

                node.Name = nodeYAML["Name"] ? nodeYAML["Name"].as<std::string>("") : "";

                if (auto editorPos = nodeYAML["EditorPosition"]; editorPos && editorPos.IsSequence() && editorPos.size() >= 2)
                {
                    try
                    {
                        node.EditorPosition.x = editorPos[0].as<f32>();
                        node.EditorPosition.y = editorPos[1].as<f32>();
                    }
                    catch (const YAML::Exception& e)
                    {
                        OLO_CORE_WARN("DialogueTreeSerializer - Failed to parse EditorPosition for node {}: {}", static_cast<u64>(node.ID), e.what());
                    }
                }

                if (auto propsNode = nodeYAML["Properties"]; propsNode && propsNode.IsMap())
                {
                    for (const auto& prop : propsNode)
                    {
                        try
                        {
                            std::string propKey = prop.first.as<std::string>();
                            auto propVal = prop.second;
                            if (propVal.IsMap() && propVal["type"] && propVal["value"])
                            {
                                std::string typeStr = propVal["type"].as<std::string>();
                                std::string valueStr = propVal["value"].as<std::string>();
                                node.Properties[propKey] = ParsePropertyValue(typeStr, valueStr);
                            }
                            else if (propVal.IsScalar())
                            {
                                node.Properties[propKey] = propVal.as<std::string>();
                            }
                        }
                        catch (const YAML::Exception& e)
                        {
                            OLO_CORE_WARN("DialogueTreeSerializer - Failed to parse property: {}", e.what());
                        }
                    }
                }

                localNodes.push_back(std::move(node));
            }
        }

        // Validate root node exists
        if (!nodeIDs.contains(static_cast<u64>(localRootID)))
        {
            OLO_CORE_ERROR("DialogueTreeSerializer - Root node ID {} not found in nodes", static_cast<u64>(localRootID));
            return false;
        }

        // Connections
        if (auto connectionsNode = dialogueTree["Connections"]; connectionsNode && connectionsNode.IsSequence())
        {
            for (const auto& connYAML : connectionsNode)
            {
                DialogueConnection conn;
                try
                {
                    if (!connYAML["SourceNodeID"] || !connYAML["TargetNodeID"])
                    {
                        OLO_CORE_ERROR("DialogueTreeSerializer - Connection missing required SourceNodeID or TargetNodeID");
                        return false;
                    }
                    conn.SourceNodeID = connYAML["SourceNodeID"].as<u64>();
                    conn.TargetNodeID = connYAML["TargetNodeID"].as<u64>();
                }
                catch (const YAML::Exception& e)
                {
                    OLO_CORE_ERROR("DialogueTreeSerializer - Failed to parse connection: {}", e.what());
                    return false;
                }
                conn.SourcePort = connYAML["SourcePort"] ? connYAML["SourcePort"].as<std::string>("output") : "output";
                conn.TargetPort = connYAML["TargetPort"] ? connYAML["TargetPort"].as<std::string>("input") : "input";

                // Validate connection references
                if (!nodeIDs.contains(static_cast<u64>(conn.SourceNodeID)))
                {
                    OLO_CORE_ERROR("DialogueTreeSerializer - Connection references non-existent source node: {}", static_cast<u64>(conn.SourceNodeID));
                    return false;
                }
                if (!nodeIDs.contains(static_cast<u64>(conn.TargetNodeID)))
                {
                    OLO_CORE_ERROR("DialogueTreeSerializer - Connection references non-existent target node: {}", static_cast<u64>(conn.TargetNodeID));
                    return false;
                }

                localConnections.push_back(std::move(conn));
            }
        }

        // All validation passed — commit to asset
        dialogueAsset->m_RootNodeID = localRootID;
        dialogueAsset->m_Nodes = std::move(localNodes);
        dialogueAsset->m_Connections = std::move(localConnections);
        dialogueAsset->RebuildNodeIndex();
        return true;
    }

} // namespace OloEngine
