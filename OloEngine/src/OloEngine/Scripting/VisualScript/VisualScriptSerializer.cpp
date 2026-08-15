#include "OloEnginePCH.h"
#include "VisualScriptSerializer.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptGraph.h"

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <sstream>

namespace OloEngine::VisualScript
{
    namespace
    {
        constexpr u32 kFormatVersion = 1;
        constexpr u32 kMaxNodesPerGraph = 100000;

        // Positions are the only raw floats in the file. Emit them through the
        // same shortest-round-trip path as pin values so the byte-identity
        // round-trip holds for a node dragged to an awkward coordinate.
        std::string Vec2ToString(const glm::vec2& value)
        {
            return PinValue::MakeVec2(value).ToStorageString();
        }

        glm::vec2 Vec2FromNode(const YAML::Node& node)
        {
            if (!node || !node.IsScalar())
            {
                return glm::vec2(0.0f);
            }
            return PinValue::FromStorageString(PinType::Vec2, node.as<std::string>("")).AsVec2();
        }
    } // namespace

    void VisualScriptSerializer::SerializeVariable(YAML::Emitter& out, const VisualScriptVariable& variable)
    {
        out << YAML::BeginMap;
        out << YAML::Key << "Name" << YAML::Value << variable.m_Name;
        out << YAML::Key << "Type" << YAML::Value << PinTypeToString(variable.m_Type);
        out << YAML::Key << "Default" << YAML::Value << variable.m_DefaultValue.ToStorageString();
        out << YAML::EndMap;
    }

    void VisualScriptSerializer::SerializeGraph(YAML::Emitter& out, const VisualScriptGraph& graph)
    {
        out << YAML::BeginMap;
        out << YAML::Key << "Name" << YAML::Value << graph.m_Name;
        out << YAML::Key << "NextNodeId" << YAML::Value << graph.m_NextNodeId;
        out << YAML::Key << "NextLinkId" << YAML::Value << graph.m_NextLinkId;

        out << YAML::Key << "Inputs" << YAML::Value << YAML::BeginSeq;
        for (const VisualScriptVariable& parameter : graph.m_Inputs)
        {
            SerializeVariable(out, parameter);
        }
        out << YAML::EndSeq;

        out << YAML::Key << "Outputs" << YAML::Value << YAML::BeginSeq;
        for (const VisualScriptVariable& result : graph.m_Outputs)
        {
            SerializeVariable(out, result);
        }
        out << YAML::EndSeq;

        out << YAML::Key << "Nodes" << YAML::Value << YAML::BeginSeq;
        for (const VisualScriptNode& node : graph.m_Nodes)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Id" << YAML::Value << node.m_Id;
            out << YAML::Key << "Type" << YAML::Value << node.m_TypeName;
            out << YAML::Key << "Position" << YAML::Value << Vec2ToString(node.m_Position);

            if (!node.m_Properties.empty())
            {
                out << YAML::Key << "Properties" << YAML::Value << YAML::BeginMap;
                for (const auto& [key, value] : node.m_Properties)
                {
                    out << YAML::Key << key << YAML::Value << value;
                }
                out << YAML::EndMap;
            }

            if (!node.m_PinDefaults.empty())
            {
                // Pin defaults carry their TYPE because the node's registry
                // signature may legitimately change between save and load; the
                // reader coerces, and without the stored type it could not.
                out << YAML::Key << "PinDefaults" << YAML::Value << YAML::BeginSeq;
                for (const auto& [pin, value] : node.m_PinDefaults)
                {
                    out << YAML::BeginMap;
                    out << YAML::Key << "Pin" << YAML::Value << pin;
                    out << YAML::Key << "Type" << YAML::Value << PinTypeToString(value.GetType());
                    out << YAML::Key << "Value" << YAML::Value << value.ToStorageString();
                    out << YAML::EndMap;
                }
                out << YAML::EndSeq;
            }
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        out << YAML::Key << "Links" << YAML::Value << YAML::BeginSeq;
        for (const VisualScriptLink& link : graph.m_Links)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Id" << YAML::Value << link.m_Id;
            out << YAML::Key << "SourceNode" << YAML::Value << link.m_SourceNode;
            out << YAML::Key << "SourcePin" << YAML::Value << link.m_SourcePin;
            out << YAML::Key << "TargetNode" << YAML::Value << link.m_TargetNode;
            out << YAML::Key << "TargetPin" << YAML::Value << link.m_TargetPin;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        out << YAML::EndMap;
    }

    std::string VisualScriptSerializer::SerializeToString(const VisualScriptAsset& asset)
    {
        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "VisualScript" << YAML::Value << kFormatVersion;
        out << YAML::Key << "NodeBudgetPerTick" << YAML::Value << asset.m_NodeBudgetPerTick;

        out << YAML::Key << "Variables" << YAML::Value << YAML::BeginSeq;
        for (const VisualScriptVariable& variable : asset.m_Variables)
        {
            SerializeVariable(out, variable);
        }
        out << YAML::EndSeq;

        out << YAML::Key << "EventGraph" << YAML::Value;
        SerializeGraph(out, asset.m_EventGraph);

        out << YAML::Key << "Functions" << YAML::Value << YAML::BeginSeq;
        for (const VisualScriptGraph& function : asset.m_Functions)
        {
            SerializeGraph(out, function);
        }
        out << YAML::EndSeq;

        out << YAML::EndMap;
        return std::string(out.c_str());
    }

    bool VisualScriptSerializer::DeserializeVariable(const YAML::Node& node, VisualScriptVariable& outVariable)
    {
        if (!node || !node.IsMap())
        {
            return false;
        }
        outVariable.m_Name = node["Name"].as<std::string>("");
        if (outVariable.m_Name.empty())
        {
            return false;
        }
        outVariable.m_Type = PinTypeFromString(node["Type"].as<std::string>("Float"));
        outVariable.m_DefaultValue = PinValue::FromStorageString(outVariable.m_Type, node["Default"].as<std::string>(""));
        // Every float that enters the engine from a file is finiteness-checked;
        // a NaN default would otherwise be indistinguishable from an authored 0
        // until it reached a transform.
        if (outVariable.m_DefaultValue.SanitizeNonFinite())
        {
            OLO_CORE_WARN("[VisualScript] Non-finite default for variable '{}' clamped to 0", outVariable.m_Name);
        }
        return true;
    }

    bool VisualScriptSerializer::DeserializeGraph(const YAML::Node& node, VisualScriptGraph& outGraph)
    {
        if (!node || !node.IsMap())
        {
            return false;
        }

        outGraph = VisualScriptGraph{};
        outGraph.m_Name = node["Name"].as<std::string>("EventGraph");

        if (const YAML::Node inputs = node["Inputs"]; inputs && inputs.IsSequence())
        {
            for (const YAML::Node& entry : inputs)
            {
                if (VisualScriptVariable parameter; DeserializeVariable(entry, parameter))
                {
                    outGraph.m_Inputs.push_back(std::move(parameter));
                }
            }
        }
        if (const YAML::Node outputs = node["Outputs"]; outputs && outputs.IsSequence())
        {
            for (const YAML::Node& entry : outputs)
            {
                if (VisualScriptVariable result; DeserializeVariable(entry, result))
                {
                    outGraph.m_Outputs.push_back(std::move(result));
                }
            }
        }

        if (const YAML::Node nodes = node["Nodes"]; nodes && nodes.IsSequence())
        {
            if (nodes.size() > kMaxNodesPerGraph)
            {
                OLO_CORE_ERROR("[VisualScript] Graph '{}' declares {} nodes, above the {} cap; refusing to load",
                               outGraph.m_Name, nodes.size(), kMaxNodesPerGraph);
                return false;
            }
            for (const YAML::Node& entry : nodes)
            {
                if (!entry.IsMap())
                {
                    continue;
                }
                VisualScriptNode graphNode;
                graphNode.m_Id = entry["Id"].as<u32>(0);
                graphNode.m_TypeName = entry["Type"].as<std::string>("");
                if (graphNode.m_Id == kInvalidNodeId || graphNode.m_TypeName.empty())
                {
                    OLO_CORE_WARN("[VisualScript] Skipping a node in '{}' with no id or type", outGraph.m_Name);
                    continue;
                }
                graphNode.m_Position = Vec2FromNode(entry["Position"]);

                if (const YAML::Node properties = entry["Properties"]; properties && properties.IsMap())
                {
                    for (const auto& property : properties)
                    {
                        graphNode.m_Properties[property.first.as<std::string>()] = property.second.as<std::string>("");
                    }
                }

                if (const YAML::Node defaults = entry["PinDefaults"]; defaults && defaults.IsSequence())
                {
                    for (const YAML::Node& pinNode : defaults)
                    {
                        if (!pinNode.IsMap())
                        {
                            continue;
                        }
                        const std::string pin = pinNode["Pin"].as<std::string>("");
                        if (pin.empty())
                        {
                            continue;
                        }
                        const PinType type = PinTypeFromString(pinNode["Type"].as<std::string>("Float"));
                        PinValue value = PinValue::FromStorageString(type, pinNode["Value"].as<std::string>(""));
                        (void)value.SanitizeNonFinite();
                        graphNode.m_PinDefaults[pin] = std::move(value);
                    }
                }
                outGraph.m_Nodes.push_back(std::move(graphNode));
            }
        }

        if (const YAML::Node links = node["Links"]; links && links.IsSequence())
        {
            for (const YAML::Node& entry : links)
            {
                if (!entry.IsMap())
                {
                    continue;
                }
                VisualScriptLink link;
                link.m_Id = entry["Id"].as<u32>(0);
                link.m_SourceNode = entry["SourceNode"].as<u32>(0);
                link.m_SourcePin = entry["SourcePin"].as<std::string>("");
                link.m_TargetNode = entry["TargetNode"].as<u32>(0);
                link.m_TargetPin = entry["TargetPin"].as<std::string>("");
                if (link.m_Id == kInvalidLinkId || link.m_SourceNode == kInvalidNodeId || link.m_TargetNode == kInvalidNodeId)
                {
                    OLO_CORE_WARN("[VisualScript] Skipping a malformed link in '{}'", outGraph.m_Name);
                    continue;
                }
                outGraph.m_Links.push_back(std::move(link));
            }
        }

        // Trust the stored counters only as a floor: a file hand-edited to a
        // smaller value would make the next AddNode collide with an existing id.
        outGraph.m_NextNodeId = node["NextNodeId"].as<u32>(1);
        outGraph.m_NextLinkId = node["NextLinkId"].as<u32>(1);
        for (const VisualScriptNode& graphNode : outGraph.m_Nodes)
        {
            outGraph.m_NextNodeId = std::max(outGraph.m_NextNodeId, graphNode.m_Id + 1);
        }
        for (const VisualScriptLink& link : outGraph.m_Links)
        {
            outGraph.m_NextLinkId = std::max(outGraph.m_NextLinkId, link.m_Id + 1);
        }
        return true;
    }

    bool VisualScriptSerializer::DeserializeFromString(VisualScriptAsset& asset, const std::string& yamlText)
    {
        YAML::Node root;
        try
        {
            root = YAML::Load(yamlText);
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("[VisualScript] Failed to parse graph YAML: {}", e.what());
            return false;
        }

        if (!root || !root.IsMap() || !root["VisualScript"])
        {
            OLO_CORE_ERROR("[VisualScript] YAML is not a visual-script graph (missing the 'VisualScript' key)");
            return false;
        }
        if (const u32 version = root["VisualScript"].as<u32>(0); version == 0 || version > kFormatVersion)
        {
            OLO_CORE_ERROR("[VisualScript] Unsupported graph format version {} (this build reads up to {})", version, kFormatVersion);
            return false;
        }

        asset.m_Variables.clear();
        asset.m_Functions.clear();
        asset.m_EventGraph = VisualScriptGraph{};
        asset.m_NodeBudgetPerTick = std::max(root["NodeBudgetPerTick"].as<u32>(10000), 1u);

        if (const YAML::Node variables = root["Variables"]; variables && variables.IsSequence())
        {
            for (const YAML::Node& entry : variables)
            {
                if (VisualScriptVariable variable; DeserializeVariable(entry, variable))
                {
                    asset.m_Variables.push_back(std::move(variable));
                }
            }
        }

        if (!DeserializeGraph(root["EventGraph"], asset.m_EventGraph))
        {
            OLO_CORE_ERROR("[VisualScript] Graph YAML has no readable EventGraph");
            return false;
        }

        if (const YAML::Node functions = root["Functions"]; functions && functions.IsSequence())
        {
            for (const YAML::Node& entry : functions)
            {
                if (VisualScriptGraph function; DeserializeGraph(entry, function))
                {
                    asset.m_Functions.push_back(std::move(function));
                }
            }
        }
        return true;
    }

    bool VisualScriptSerializer::Serialize(const VisualScriptAsset& asset, const std::filesystem::path& path)
    {
        std::ofstream stream(path);
        if (!stream)
        {
            OLO_CORE_ERROR("[VisualScript] Cannot open '{}' for writing", path.string());
            return false;
        }
        stream << SerializeToString(asset);
        return stream.good();
    }

    bool VisualScriptSerializer::Deserialize(VisualScriptAsset& asset, const std::filesystem::path& path)
    {
        std::ifstream stream(path);
        if (!stream)
        {
            OLO_CORE_ERROR("[VisualScript] Cannot open '{}' for reading", path.string());
            return false;
        }
        std::stringstream buffer;
        buffer << stream.rdbuf();
        return DeserializeFromString(asset, buffer.str());
    }

} // namespace OloEngine::VisualScript
