#include "OloEnginePCH.h"
#include "SoundGraphSerializer.h"
#include "OloEngine/Asset/SoundGraphAsset.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/YAMLConverters.h"

#include <yaml-cpp/yaml.h>
#include <fstream>

namespace OloEngine::Audio::SoundGraph
{
    std::string SoundGraphSerializer::SerializeToString(const SoundGraphAsset& asset)
    {
        YAML::Emitter out;
        
        out << YAML::BeginMap;
        out << YAML::Key << "SoundGraph" << YAML::Value << YAML::BeginMap;
        
        // Basic properties
        out << YAML::Key << "Name" << YAML::Value << asset.Name;
        out << YAML::Key << "Description" << YAML::Value << asset.Description;
        out << YAML::Key << "Version" << YAML::Value << asset.Version;
        out << YAML::Key << "ID" << YAML::Value << static_cast<u64>(asset.GetHandle());
        
        // Nodes
        out << YAML::Key << "Nodes" << YAML::Value << YAML::BeginSeq;
        for (const auto& node : asset.Nodes)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "ID" << YAML::Value << static_cast<u64>(node.ID);
            out << YAML::Key << "Name" << YAML::Value << node.Name;
            out << YAML::Key << "Type" << YAML::Value << node.Type;
            out << YAML::Key << "PosX" << YAML::Value << node.PosX;
            out << YAML::Key << "PosY" << YAML::Value << node.PosY;
            
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
        
        // Connections
        out << YAML::Key << "Connections" << YAML::Value << YAML::BeginSeq;
        for (const auto& connection : asset.Connections)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "SourceNodeID" << YAML::Value << static_cast<u64>(connection.SourceNodeID);
            out << YAML::Key << "SourceEndpoint" << YAML::Value << connection.SourceEndpoint;
            out << YAML::Key << "TargetNodeID" << YAML::Value << static_cast<u64>(connection.TargetNodeID);
            out << YAML::Key << "TargetEndpoint" << YAML::Value << connection.TargetEndpoint;
            out << YAML::Key << "IsEvent" << YAML::Value << connection.IsEvent;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
        
        // Graph configuration
        if (!asset.GraphInputs.empty())
        {
            out << YAML::Key << "GraphInputs" << YAML::Value << YAML::BeginMap;
            for (const auto& [key, value] : asset.GraphInputs)
            {
                out << YAML::Key << key << YAML::Value << value;
            }
            out << YAML::EndMap;
        }
        
        if (!asset.GraphOutputs.empty())
        {
            out << YAML::Key << "GraphOutputs" << YAML::Value << YAML::BeginMap;
            for (const auto& [key, value] : asset.GraphOutputs)
            {
                out << YAML::Key << key << YAML::Value << value;
            }
            out << YAML::EndMap;
        }
        
        if (!asset.LocalVariables.empty())
        {
            out << YAML::Key << "LocalVariables" << YAML::Value << YAML::BeginMap;
            for (const auto& [key, value] : asset.LocalVariables)
            {
                out << YAML::Key << key << YAML::Value << value;
            }
            out << YAML::EndMap;
        }
        
        // Wave sources
        if (!asset.WaveSources.empty())
        {
            out << YAML::Key << "WaveSources" << YAML::Value << YAML::BeginSeq;
            for (const auto& waveSource : asset.WaveSources)
            {
                out << static_cast<u64>(waveSource);
            }
            out << YAML::EndSeq;
        }
        
        out << YAML::EndMap; // SoundGraph
        out << YAML::EndMap; // Root
        
        // Check emitter state before using c_str()
        if (out.good())
        {
            return std::string(out.c_str());
        }
        else
        {
            OLO_CORE_ERROR("SoundGraphSerializer: YAML emitter failed during serialization");
            return std::string(); // Return empty string on failure
        }
    }

    bool SoundGraphSerializer::DeserializeFromString(SoundGraphAsset& asset, const std::string& yamlString)
    {
        try
        {
            YAML::Node root = YAML::Load(yamlString);
            
            if (!root["SoundGraph"])
            {
                OLO_CORE_ERROR("SoundGraphSerializer: Missing 'SoundGraph' root node");
                return false;
            }
            
            YAML::Node soundGraph = root["SoundGraph"];
            
            // Parse into temporary variables first to avoid mutating the asset on failure
            std::string tempName;
            std::string tempDescription;
            u32 tempVersion = 0;
            u64 tempAssetID = 0;
            std::vector<SoundGraphNodeData> tempNodes;
            std::vector<SoundGraphConnection> tempConnections;
            std::unordered_map<std::string, std::string> tempGraphInputs;
            std::unordered_map<std::string, std::string> tempGraphOutputs;
            std::unordered_map<std::string, std::string> tempLocalVariables;
            std::vector<AssetHandle> tempWaveSources;
            
            // Basic properties
            if (soundGraph["Name"])
                tempName = soundGraph["Name"].as<std::string>();
            
            if (soundGraph["Description"])
                tempDescription = soundGraph["Description"].as<std::string>();
            
            if (soundGraph["Version"])
                tempVersion = soundGraph["Version"].as<u32>();
            
            if (soundGraph["ID"])
            {
                u64 fileAssetID = soundGraph["ID"].as<u64>();
                tempAssetID = fileAssetID;
                u64 currentAssetHandle = static_cast<u64>(asset.GetHandle());
                
                // Validate the asset ID from file against the current handle
                // This helps detect potential asset ID mismatches during loading
                if (currentAssetHandle != 0 && fileAssetID != 0)
                {
                    if (currentAssetHandle != fileAssetID)
                    {
                        OLO_CORE_WARN("SoundGraphSerializer: Asset ID mismatch - file contains {}, current handle is {}. "
                                     "This could indicate the asset was loaded with a different handle than expected.",
                                     fileAssetID, currentAssetHandle);
                    }
                    else
                    {
                        OLO_CORE_TRACE("SoundGraphSerializer: Asset ID validation passed - handle {} matches file ID {}",
                                       currentAssetHandle, fileAssetID);
                    }
                }
                
                // Note: We cannot set the asset ID directly since it's managed by AssetManager
                // The ID in the file is mainly for reference and validation purposes
            }
            
            // Nodes
            if (soundGraph["Nodes"] && soundGraph["Nodes"].IsSequence())
            {
                for (const auto& nodeYaml : soundGraph["Nodes"])
                {
                    SoundGraphNodeData node;
                    
                    if (!nodeYaml["ID"] || !nodeYaml["Type"])
                    {
                        OLO_CORE_ERROR("SoundGraphSerializer: Node missing required ID or Type");
                        return false;
                    }
                    
                    node.ID = nodeYaml["ID"].as<u64>();
                    node.Type = nodeYaml["Type"].as<std::string>();
                    
                    if (nodeYaml["Name"])
                        node.Name = nodeYaml["Name"].as<std::string>();
                    
                    if (nodeYaml["PosX"])
                        node.PosX = nodeYaml["PosX"].as<float>();
                    
                    if (nodeYaml["PosY"])
                        node.PosY = nodeYaml["PosY"].as<float>();
                    
                    // Properties
                    if (nodeYaml["Properties"] && nodeYaml["Properties"].IsMap())
                    {
                        for (const auto& prop : nodeYaml["Properties"])
                        {
                            node.Properties[prop.first.as<std::string>()] = prop.second.as<std::string>();
                        }
                    }
                    
                    tempNodes.push_back(node);
                }
            }
            
            // Connections
            if (soundGraph["Connections"] && soundGraph["Connections"].IsSequence())
            {
                for (const auto& connYaml : soundGraph["Connections"])
                {
                    if (!connYaml["SourceNodeID"] || !connYaml["SourceEndpoint"] ||
                        !connYaml["TargetNodeID"] || !connYaml["TargetEndpoint"])
                    {
                        OLO_CORE_WARN("SoundGraphSerializer: Connection missing required fields, skipping");
                        continue;
                    }
                    
                    SoundGraphConnection connection;
                    connection.SourceNodeID = connYaml["SourceNodeID"].as<u64>();
                    connection.SourceEndpoint = connYaml["SourceEndpoint"].as<std::string>();
                    connection.TargetNodeID = connYaml["TargetNodeID"].as<u64>();
                    connection.TargetEndpoint = connYaml["TargetEndpoint"].as<std::string>();
                    
                    if (connYaml["IsEvent"])
                        connection.IsEvent = connYaml["IsEvent"].as<bool>();
                    
                    tempConnections.push_back(connection);
                }
            }
            
            // Graph configuration
            if (soundGraph["GraphInputs"] && soundGraph["GraphInputs"].IsMap())
            {
                for (const auto& input : soundGraph["GraphInputs"])
                {
                    tempGraphInputs[input.first.as<std::string>()] = input.second.as<std::string>();
                }
            }
            
            if (soundGraph["GraphOutputs"] && soundGraph["GraphOutputs"].IsMap())
            {
                for (const auto& output : soundGraph["GraphOutputs"])
                {
                    tempGraphOutputs[output.first.as<std::string>()] = output.second.as<std::string>();
                }
            }
            
            if (soundGraph["LocalVariables"] && soundGraph["LocalVariables"].IsMap())
            {
                for (const auto& var : soundGraph["LocalVariables"])
                {
                    tempLocalVariables[var.first.as<std::string>()] = var.second.as<std::string>();
                }
            }
            
            // Wave sources
            if (soundGraph["WaveSources"] && soundGraph["WaveSources"].IsSequence())
            {
                for (const auto& waveSourceYaml : soundGraph["WaveSources"])
                {
                    tempWaveSources.push_back(waveSourceYaml.as<u64>());
                }
            }
            
            // All parsing successful - now clear and update the asset
            asset.Clear();
            
            // Assign parsed data to asset
            asset.Name = tempName;
            asset.Description = tempDescription;
            asset.Version = tempVersion;
            
            // Add all nodes
            for (const auto& node : tempNodes)
            {
                asset.AddNode(node);
            }
            
            // Add all connections
            for (const auto& connection : tempConnections)
            {
                asset.AddConnection(connection);
            }
            
            // Set graph configuration
            asset.GraphInputs = tempGraphInputs;
            asset.GraphOutputs = tempGraphOutputs;
            asset.LocalVariables = tempLocalVariables;
            
            // Set wave sources
            asset.WaveSources = tempWaveSources;
            
            return asset.IsValid();
        }
        catch (const YAML::Exception& ex)
        {
            OLO_CORE_ERROR("SoundGraphSerializer: YAML parsing error - {}", ex.what());
            return false;
        }
        catch (const std::exception& ex)
        {
            OLO_CORE_ERROR("SoundGraphSerializer: Deserialization error - {}", ex.what());
            return false;
        }
    }

    bool SoundGraphSerializer::Serialize(const SoundGraphAsset& asset, const std::filesystem::path& filePath)
    {
        try
        {
            std::string yamlString = SerializeToString(asset);
            
            // Check if serialization failed (empty string indicates failure)
            if (yamlString.empty())
            {
                OLO_CORE_ERROR("SoundGraphSerializer: Failed to serialize SoundGraphAsset to YAML string for file: {}", filePath.string());
                return false;
            }
            
            // Ensure directory exists (only if parent path is not empty)
            if (!filePath.parent_path().empty())
            {
                std::filesystem::create_directories(filePath.parent_path());
            }
            
            std::ofstream file(filePath);
            if (!file.is_open())
            {
                OLO_CORE_ERROR("SoundGraphSerializer: Failed to open file for writing: {}", filePath.string());
                return false;
            }
            
            file << yamlString;
            
            // Flush the stream and check for write errors
            file.flush();
            if (file.fail() || file.bad())
            {
                OLO_CORE_ERROR("SoundGraphSerializer: Failed to write data to file: {}", filePath.string());
                file.close();
                return false;
            }
            
            file.close();
            
            // Check for any errors that occurred during close
            if (file.fail())
            {
                OLO_CORE_ERROR("SoundGraphSerializer: Failed to close file properly: {}", filePath.string());
                return false;
            }
            
            return true;
        }
        catch (const std::exception& ex)
        {
            OLO_CORE_ERROR("SoundGraphSerializer: Failed to serialize to file '{}': {}", filePath.string(), ex.what());
            return false;
        }
    }

    bool SoundGraphSerializer::Deserialize(SoundGraphAsset& asset, const std::filesystem::path& filePath)
    {
        try
        {
            if (!std::filesystem::exists(filePath))
            {
                OLO_CORE_ERROR("SoundGraphSerializer: File does not exist: {}", filePath.string());
                return false;
            }
            
            std::ifstream file(filePath);
            if (!file.is_open())
            {
                OLO_CORE_ERROR("SoundGraphSerializer: Failed to open file for reading: {}", filePath.string());
                return false;
            }
            
            std::string yamlString((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
            file.close();
            
            return DeserializeFromString(asset, yamlString);
        }
        catch (const std::exception& ex)
        {
            OLO_CORE_ERROR("SoundGraphSerializer: Failed to deserialize from file '{}': {}", filePath.string(), ex.what());
            return false;
        }
    }

} // namespace OloEngine::Audio::SoundGraph