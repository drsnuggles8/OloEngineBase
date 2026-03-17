#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphSerializer.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphAsset.h"
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
        std::string PinValueToYAMLString(const ShaderGraphPinValue& value)
        {
            return std::visit([](const auto& v) -> std::string
                              {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::monostate>)
                    return "";
                else if constexpr (std::is_same_v<T, f32>)
                    return std::to_string(v);
                else if constexpr (std::is_same_v<T, glm::vec2>)
                    return std::to_string(v.x) + "," + std::to_string(v.y);
                else if constexpr (std::is_same_v<T, glm::vec3>)
                    return std::to_string(v.x) + "," + std::to_string(v.y) + "," + std::to_string(v.z);
                else if constexpr (std::is_same_v<T, glm::vec4>)
                    return std::to_string(v.x) + "," + std::to_string(v.y) + "," + std::to_string(v.z) + "," + std::to_string(v.w);
                else if constexpr (std::is_same_v<T, bool>)
                    return v ? "true" : "false";
                else
                    return ""; }, value);
        }

        ShaderGraphPinValue ParsePinValue(ShaderGraphPinType type, const std::string& str)
        {
            try
            {
                switch (type)
                {
                    case ShaderGraphPinType::Float:
                        return std::stof(str);
                    case ShaderGraphPinType::Bool:
                        return str == "true";
                    case ShaderGraphPinType::Vec2:
                    {
                        auto comma = str.find(',');
                        if (comma != std::string::npos)
                            return glm::vec2(std::stof(str.substr(0, comma)), std::stof(str.substr(comma + 1)));
                        return glm::vec2(0.0f);
                    }
                    case ShaderGraphPinType::Vec3:
                    {
                        auto c1 = str.find(',');
                        auto c2 = str.find(',', c1 + 1);
                        if (c1 != std::string::npos && c2 != std::string::npos)
                            return glm::vec3(std::stof(str.substr(0, c1)), std::stof(str.substr(c1 + 1, c2 - c1 - 1)), std::stof(str.substr(c2 + 1)));
                        return glm::vec3(0.0f);
                    }
                    case ShaderGraphPinType::Vec4:
                    {
                        auto c1 = str.find(',');
                        if (c1 == std::string::npos)
                            return glm::vec4(0.0f);
                        auto c2 = str.find(',', c1 + 1);
                        if (c2 == std::string::npos)
                            return glm::vec4(0.0f);
                        auto c3 = str.find(',', c2 + 1);
                        if (c3 == std::string::npos)
                            return glm::vec4(0.0f);
                        return glm::vec4(std::stof(str.substr(0, c1)), std::stof(str.substr(c1 + 1, c2 - c1 - 1)),
                                         std::stof(str.substr(c2 + 1, c3 - c2 - 1)), std::stof(str.substr(c3 + 1)));
                    }
                    default:
                        return std::monostate{};
                }
            }
            catch (const std::exception& e)
            {
                OLO_CORE_WARN("ShaderGraphSerializer - Failed to parse pin value '{}': {}", str, e.what());
                return std::monostate{};
            }
        }
    } // namespace

    // ─────────────────────────────────────────────────────────────
    //  Serialize / TryLoadData  (AssetSerializer interface)
    // ─────────────────────────────────────────────────────────────

    void ShaderGraphSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto graphAsset = asset.As<ShaderGraphAsset>();
        if (!graphAsset)
        {
            OLO_CORE_ERROR("ShaderGraphSerializer::Serialize - Failed to cast asset to ShaderGraphAsset ({})", metadata.FilePath.string());
            return;
        }

        std::string yamlString = SerializeToYAML(graphAsset);
        auto fullPath = Project::GetAssetDirectory() / metadata.FilePath;

        std::error_code ec;
        std::filesystem::create_directories(fullPath.parent_path(), ec);
        if (ec)
        {
            OLO_CORE_ERROR("ShaderGraphSerializer::Serialize - Failed to create directories for ({}): {}", fullPath.string(), ec.message());
            return;
        }

        std::ofstream fout(fullPath);
        if (!fout.is_open())
        {
            OLO_CORE_ERROR("ShaderGraphSerializer::Serialize - Failed to open file for writing ({})", fullPath.string());
            return;
        }
        fout << yamlString;
    }

    bool ShaderGraphSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto path = Project::GetAssetDirectory() / metadata.FilePath;
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_WARN("ShaderGraphSerializer::TryLoadData - File does not exist ({})", path.string());
            return false;
        }

        std::ifstream file(path);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("ShaderGraphSerializer::TryLoadData - Failed to open file ({})", path.string());
            return false;
        }

        std::stringstream ss;
        ss << file.rdbuf();

        auto graphAsset = Ref<ShaderGraphAsset>::Create();
        if (!DeserializeFromYAML(ss.str(), graphAsset))
        {
            OLO_CORE_ERROR("ShaderGraphSerializer::TryLoadData - Failed to deserialize ({})", path.string());
            return false;
        }

        graphAsset->SetHandle(metadata.Handle);
        asset = graphAsset;
        return true;
    }

    // ─────────────────────────────────────────────────────────────
    //  Asset Pack
    // ─────────────────────────────────────────────────────────────

    bool ShaderGraphSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        auto graphAsset = AssetManager::GetAsset<ShaderGraphAsset>(handle);
        if (!graphAsset)
        {
            OLO_CORE_ERROR("ShaderGraphSerializer::SerializeToAssetPack - Failed to get asset ({})", static_cast<u64>(handle));
            return false;
        }

        std::string yamlString = SerializeToYAML(graphAsset);
        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yamlString);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> ShaderGraphSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        stream.SetStreamPosition(assetInfo.PackedOffset);
        std::string yamlString;
        stream.ReadString(yamlString);

        auto graphAsset = Ref<ShaderGraphAsset>::Create();
        if (!DeserializeFromYAML(yamlString, graphAsset))
        {
            OLO_CORE_ERROR("ShaderGraphSerializer::DeserializeFromAssetPack - Failed to deserialize YAML (handle: {})", static_cast<u64>(assetInfo.Handle));
            return nullptr;
        }

        graphAsset->SetHandle(assetInfo.Handle);
        return graphAsset;
    }

    // ─────────────────────────────────────────────────────────────
    //  YAML Serialization
    // ─────────────────────────────────────────────────────────────

    std::string ShaderGraphSerializer::SerializeToYAML(const Ref<ShaderGraphAsset>& graphAsset) const
    {
        OLO_PROFILE_FUNCTION();

        const auto& graph = graphAsset->m_Graph;
        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "ShaderGraph" << YAML::Value;
        out << YAML::BeginMap;

        out << YAML::Key << "Name" << YAML::Value << graph.GetName();

        // Nodes
        out << YAML::Key << "Nodes" << YAML::Value << YAML::BeginSeq;
        for (const auto& node : graph.GetNodes())
        {
            out << YAML::BeginMap;
            out << YAML::Key << "ID" << YAML::Value << static_cast<u64>(node->ID);
            out << YAML::Key << "TypeName" << YAML::Value << node->TypeName;
            out << YAML::Key << "Category" << YAML::Value << NodeCategoryToString(node->Category);
            out << YAML::Key << "EditorPosition" << YAML::Value << YAML::Flow << YAML::BeginSeq
                << node->EditorPosition.x << node->EditorPosition.y << YAML::EndSeq;

            if (!node->ParameterName.empty())
                out << YAML::Key << "ParameterName" << YAML::Value << node->ParameterName;

            if (!node->CustomFunctionBody.empty())
                out << YAML::Key << "CustomFunctionBody" << YAML::Value << node->CustomFunctionBody;

            if (node->WorkgroupSize != glm::ivec3(16, 16, 1))
                out << YAML::Key << "WorkgroupSize" << YAML::Value << YAML::Flow << YAML::BeginSeq
                    << node->WorkgroupSize.x << node->WorkgroupSize.y << node->WorkgroupSize.z << YAML::EndSeq;

            if (node->BufferBinding != 0)
                out << YAML::Key << "BufferBinding" << YAML::Value << node->BufferBinding;

            // Serialize pin IDs and default values
            auto serializePins = [&](const std::string& key, const std::vector<ShaderGraphPin>& pins)
            {
                out << YAML::Key << key << YAML::Value << YAML::BeginSeq;
                for (const auto& pin : pins)
                {
                    out << YAML::BeginMap;
                    out << YAML::Key << "ID" << YAML::Value << static_cast<u64>(pin.ID);
                    out << YAML::Key << "Name" << YAML::Value << pin.Name;
                    out << YAML::Key << "Type" << YAML::Value << PinTypeToString(pin.Type);

                    std::string valStr = PinValueToYAMLString(pin.DefaultValue);
                    if (!valStr.empty())
                        out << YAML::Key << "DefaultValue" << YAML::Value << valStr;

                    out << YAML::EndMap;
                }
                out << YAML::EndSeq;
            };

            serializePins("Inputs", node->Inputs);
            serializePins("Outputs", node->Outputs);

            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        // Links
        out << YAML::Key << "Links" << YAML::Value << YAML::BeginSeq;
        for (const auto& link : graph.GetLinks())
        {
            out << YAML::BeginMap;
            out << YAML::Key << "ID" << YAML::Value << static_cast<u64>(link.ID);
            out << YAML::Key << "OutputPinID" << YAML::Value << static_cast<u64>(link.OutputPinID);
            out << YAML::Key << "InputPinID" << YAML::Value << static_cast<u64>(link.InputPinID);
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        out << YAML::EndMap; // ShaderGraph
        out << YAML::EndMap; // Root

        return out.c_str();
    }

    // ─────────────────────────────────────────────────────────────
    //  YAML Deserialization
    // ─────────────────────────────────────────────────────────────

    bool ShaderGraphSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<ShaderGraphAsset>& graphAsset) const
    {
        OLO_PROFILE_FUNCTION();

        YAML::Node data;
        try
        {
            data = YAML::Load(yamlString);
        }
        catch (const YAML::ParserException& e)
        {
            OLO_CORE_ERROR("ShaderGraphSerializer - YAML parse error: {}", e.what());
            return false;
        }

        auto sgNode = data["ShaderGraph"];
        if (!sgNode)
        {
            OLO_CORE_ERROR("ShaderGraphSerializer - Missing 'ShaderGraph' root node");
            return false;
        }

        auto& graph = graphAsset->m_Graph;

        // Clear any existing state so re-deserialization doesn't duplicate
        graph.m_Nodes.clear();
        graph.m_Links.clear();

        graph.SetName(sgNode["Name"].as<std::string>("Untitled"));

        // Nodes
        std::unordered_set<u64> nodeIDs;
        if (auto nodesYAML = sgNode["Nodes"]; nodesYAML && nodesYAML.IsSequence())
        {
            for (const auto& nodeYAML : nodesYAML)
            {
                try
                {
                    if (!nodeYAML["ID"] || !nodeYAML["TypeName"])
                    {
                        OLO_CORE_ERROR("ShaderGraphSerializer - Node missing required ID or TypeName");
                        return false;
                    }

                    u64 nodeID = nodeYAML["ID"].as<u64>();
                    std::string typeName = nodeYAML["TypeName"].as<std::string>();

                    if (nodeIDs.contains(nodeID))
                    {
                        OLO_CORE_ERROR("ShaderGraphSerializer - Duplicate node ID: {}", nodeID);
                        return false;
                    }
                    nodeIDs.insert(nodeID);

                    // Create the node via factory to get default pins
                    auto node = CreateShaderGraphNode(typeName);
                    if (!node)
                    {
                        OLO_CORE_WARN("ShaderGraphSerializer - Unknown node type '{}', skipping", typeName);
                        continue;
                    }

                    node->ID = UUID(nodeID);

                    if (auto posYAML = nodeYAML["EditorPosition"]; posYAML && posYAML.IsSequence() && posYAML.size() >= 2)
                    {
                        node->EditorPosition.x = posYAML[0].as<f32>();
                        node->EditorPosition.y = posYAML[1].as<f32>();
                    }

                    if (nodeYAML["ParameterName"])
                        node->ParameterName = nodeYAML["ParameterName"].as<std::string>();

                    if (nodeYAML["CustomFunctionBody"])
                        node->CustomFunctionBody = nodeYAML["CustomFunctionBody"].as<std::string>();

                    if (auto wgYAML = nodeYAML["WorkgroupSize"]; wgYAML && wgYAML.IsSequence() && wgYAML.size() == 3)
                        node->WorkgroupSize = glm::ivec3(wgYAML[0].as<int>(), wgYAML[1].as<int>(), wgYAML[2].as<int>());

                    if (nodeYAML["BufferBinding"])
                        node->BufferBinding = nodeYAML["BufferBinding"].as<int>();

                    // Restore pin IDs and default values from YAML
                    auto restorePins = [&](const std::string& key, std::vector<ShaderGraphPin>& pins)
                    {
                        if (auto pinsYAML = nodeYAML[key]; pinsYAML && pinsYAML.IsSequence())
                        {
                            if (pinsYAML.size() != pins.size())
                                OLO_CORE_WARN("ShaderGraphSerializer - Node '{}' (ID {}): YAML {} count ({}) differs from factory ({})",
                                              typeName, nodeID, key, pinsYAML.size(), pins.size());

                            for (size_t yi = 0; yi < pinsYAML.size(); ++yi)
                            {
                                auto pinYAML = pinsYAML[yi];
                                // Try to find the matching pin: by ID first, then by Name, then by index
                                ShaderGraphPin* targetPin = nullptr;

                                if (pinYAML["ID"])
                                {
                                    u64 yamlPinID = pinYAML["ID"].as<u64>();
                                    for (auto& p : pins)
                                    {
                                        if (static_cast<u64>(p.ID) == yamlPinID)
                                        {
                                            targetPin = &p;
                                            break;
                                        }
                                    }
                                }
                                if (!targetPin && pinYAML["Name"])
                                {
                                    std::string yamlPinName = pinYAML["Name"].as<std::string>();
                                    for (auto& p : pins)
                                    {
                                        if (p.Name == yamlPinName)
                                        {
                                            targetPin = &p;
                                            break;
                                        }
                                    }
                                }
                                if (!targetPin && yi < pins.size())
                                    targetPin = &pins[yi];

                                if (!targetPin)
                                    continue;

                                if (pinYAML["ID"])
                                    targetPin->ID = UUID(pinYAML["ID"].as<u64>());
                                targetPin->NodeID = node->ID;

                                if (pinYAML["DefaultValue"])
                                {
                                    targetPin->DefaultValue = ParsePinValue(targetPin->Type, pinYAML["DefaultValue"].as<std::string>());
                                }
                            }
                        }
                    };

                    restorePins("Inputs", node->Inputs);
                    restorePins("Outputs", node->Outputs);

                    graph.m_Nodes.push_back(std::move(node));
                }
                catch (const YAML::Exception& e)
                {
                    OLO_CORE_ERROR("ShaderGraphSerializer - Failed to parse node: {}", e.what());
                    return false;
                }
            }
        }

        // Links — reconstruct directly (bypassing AddLink validation which checks types/cycles)
        if (auto linksYAML = sgNode["Links"]; linksYAML && linksYAML.IsSequence())
        {
            for (const auto& linkYAML : linksYAML)
            {
                try
                {
                    ShaderGraphLink link;
                    link.ID = UUID(linkYAML["ID"].as<u64>());
                    link.OutputPinID = UUID(linkYAML["OutputPinID"].as<u64>());
                    link.InputPinID = UUID(linkYAML["InputPinID"].as<u64>());

                    // Validate that both endpoints exist in deserialized nodes
                    if (!graph.FindNodeByPinID(link.OutputPinID) || !graph.FindNodeByPinID(link.InputPinID))
                    {
                        OLO_CORE_WARN("ShaderGraphSerializer - Skipping link {} with dangling endpoint", static_cast<u64>(link.ID));
                        continue;
                    }

                    graph.m_Links.push_back(link);
                }
                catch (const YAML::Exception& e)
                {
                    OLO_CORE_ERROR("ShaderGraphSerializer - Failed to parse link: {}", e.what());
                    return false;
                }
            }
        }

        graphAsset->MarkDirty();
        return true;
    }

} // namespace OloEngine
