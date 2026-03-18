#include "OloEnginePCH.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/AI/FSM/StateMachineAsset.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Scene.h"

#include <fstream>
#include <sstream>

#include <yaml-cpp/yaml.h>

namespace OloEngine
{
    void StateMachineSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto fsmAsset = asset.As<StateMachineAsset>();
        if (!fsmAsset)
        {
            OLO_CORE_ERROR("StateMachineSerializer::Serialize - Failed to cast asset to StateMachineAsset ({})", metadata.FilePath.string());
            return;
        }

        std::string yamlString = SerializeToYAML(fsmAsset);
        auto fullPath = Project::GetAssetDirectory() / metadata.FilePath;

        std::error_code ec;
        std::filesystem::create_directories(fullPath.parent_path(), ec);
        if (ec)
        {
            OLO_CORE_ERROR("StateMachineSerializer::Serialize - Failed to create directories for ({}): {}", fullPath.string(), ec.message());
            return;
        }

        std::ofstream fout(fullPath);
        if (!fout.is_open())
        {
            OLO_CORE_ERROR("StateMachineSerializer::Serialize - Failed to open file for writing ({})", fullPath.string());
            return;
        }
        fout << yamlString;
    }

    bool StateMachineSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto path = Project::GetAssetDirectory() / metadata.FilePath;
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_WARN("StateMachineSerializer::TryLoadData - File does not exist ({})", path.string());
            return false;
        }

        std::ifstream file(path);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("StateMachineSerializer::TryLoadData - Failed to open file ({})", path.string());
            return false;
        }

        std::stringstream ss;
        ss << file.rdbuf();

        auto fsmAsset = Ref<StateMachineAsset>::Create();
        if (!DeserializeFromYAML(ss.str(), fsmAsset))
        {
            OLO_CORE_ERROR("StateMachineSerializer::TryLoadData - Failed to deserialize ({})", path.string());
            return false;
        }

        fsmAsset->SetHandle(metadata.Handle);
        asset = fsmAsset;
        return true;
    }

    bool StateMachineSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        auto fsmAsset = AssetManager::GetAsset<StateMachineAsset>(handle);
        if (!fsmAsset)
        {
            OLO_CORE_ERROR("StateMachineSerializer::SerializeToAssetPack - Failed to get asset ({})", static_cast<u64>(handle));
            return false;
        }

        std::string yamlString = SerializeToYAML(fsmAsset);
        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yamlString);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> StateMachineSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        stream.SetStreamPosition(assetInfo.PackedOffset);
        std::string yamlString;
        stream.ReadString(yamlString);

        auto fsmAsset = Ref<StateMachineAsset>::Create();
        if (!DeserializeFromYAML(yamlString, fsmAsset))
        {
            OLO_CORE_ERROR("StateMachineSerializer::DeserializeFromAssetPack - Failed to deserialize YAML (handle: {})", static_cast<u64>(assetInfo.Handle));
            return nullptr;
        }

        fsmAsset->SetHandle(assetInfo.Handle);
        return fsmAsset;
    }

    std::string StateMachineSerializer::SerializeToYAML(const Ref<StateMachineAsset>& fsmAsset) const
    {
        OLO_PROFILE_FUNCTION();

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "StateMachine" << YAML::Value;
        out << YAML::BeginMap;

        out << YAML::Key << "InitialStateID" << YAML::Value << fsmAsset->GetInitialStateID();

        // States
        out << YAML::Key << "States" << YAML::Value << YAML::BeginSeq;
        for (const auto& state : fsmAsset->GetStates())
        {
            out << YAML::BeginMap;
            out << YAML::Key << "ID" << YAML::Value << state.ID;
            out << YAML::Key << "TypeName" << YAML::Value << state.TypeName;

            if (!state.Properties.empty())
            {
                out << YAML::Key << "Properties" << YAML::Value << YAML::BeginMap;
                for (const auto& [key, value] : state.Properties)
                {
                    out << YAML::Key << key << YAML::Value << value;
                }
                out << YAML::EndMap;
            }

            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        // Transitions
        out << YAML::Key << "Transitions" << YAML::Value << YAML::BeginSeq;
        for (const auto& trans : fsmAsset->GetTransitions())
        {
            out << YAML::BeginMap;
            out << YAML::Key << "FromState" << YAML::Value << trans.FromState;
            out << YAML::Key << "ToState" << YAML::Value << trans.ToState;
            out << YAML::Key << "ConditionExpression" << YAML::Value << trans.ConditionExpression;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        out << YAML::EndMap; // StateMachine
        out << YAML::EndMap; // Root

        return out.c_str();
    }

    bool StateMachineSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<StateMachineAsset>& fsmAsset) const
    {
        OLO_PROFILE_FUNCTION();

        YAML::Node data;
        try
        {
            data = YAML::Load(yamlString);
        }
        catch (const YAML::ParserException& e)
        {
            OLO_CORE_ERROR("StateMachineSerializer - YAML parse error: {}", e.what());
            return false;
        }

        auto fsmNode = data["StateMachine"];
        if (!fsmNode)
        {
            OLO_CORE_ERROR("StateMachineSerializer - Missing 'StateMachine' root node");
            return false;
        }

        if (auto initialState = fsmNode["InitialStateID"])
        {
            fsmAsset->SetInitialStateID(initialState.as<std::string>());
        }

        // States
        if (auto statesNode = fsmNode["States"]; statesNode && statesNode.IsSequence())
        {
            for (const auto& stateYAML : statesNode)
            {
                FSMStateData stateData;
                try
                {
                    if (!stateYAML["ID"])
                    {
                        OLO_CORE_ERROR("StateMachineSerializer - State missing required ID field");
                        return false;
                    }
                    stateData.ID = stateYAML["ID"].as<std::string>();
                }
                catch (const YAML::Exception& e)
                {
                    OLO_CORE_ERROR("StateMachineSerializer - Failed to parse state: {}", e.what());
                    return false;
                }

                stateData.TypeName = stateYAML["TypeName"].as<std::string>("");

                if (auto props = stateYAML["Properties"]; props && props.IsMap())
                {
                    for (const auto& pair : props)
                    {
                        stateData.Properties[pair.first.as<std::string>()] = pair.second.as<std::string>();
                    }
                }

                fsmAsset->AddState(std::move(stateData));
            }
        }

        // Transitions
        if (auto transNode = fsmNode["Transitions"]; transNode && transNode.IsSequence())
        {
            for (const auto& transYAML : transNode)
            {
                FSMTransitionData transData;
                try
                {
                    transData.FromState = transYAML["FromState"].as<std::string>();
                    transData.ToState = transYAML["ToState"].as<std::string>();
                    transData.ConditionExpression = transYAML["ConditionExpression"].as<std::string>("");
                }
                catch (const YAML::Exception& e)
                {
                    OLO_CORE_ERROR("StateMachineSerializer - Failed to parse transition: {}", e.what());
                    return false;
                }

                fsmAsset->AddTransition(std::move(transData));
            }
        }

        return true;
    }
} // namespace OloEngine
