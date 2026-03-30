#include "OloEnginePCH.h"
#include "OloEngine/Audio/AudioEvents/AudioCommandRegistry.h"

#include <yaml-cpp/yaml.h>
#include <fstream>

namespace OloEngine::Audio
{
    CommandID AudioCommandRegistry::AddTrigger(const std::string& name)
    {
        auto id = CommandID::FromString(name);
        if (!id.IsValid())
        {
            OLO_CORE_WARN("AudioCommandRegistry: Cannot add trigger with empty name");
            return {};
        }

        if (m_Triggers.contains(id.ID))
        {
            OLO_CORE_WARN("AudioCommandRegistry: Trigger '{}' already exists (CRC32 collision or duplicate)", name);
            return id;
        }

        auto& cmd = m_Triggers[id.ID];
        cmd.DebugName = name;
        cmd.ID = id;
        return id;
    }

    bool AudioCommandRegistry::RemoveTrigger(CommandID id)
    {
        return m_Triggers.erase(id.ID) > 0;
    }

    bool AudioCommandRegistry::AddAction(CommandID triggerID, const TriggerAction& action)
    {
        if (auto it = m_Triggers.find(triggerID.ID); it != m_Triggers.end())
        {
            it->second.Actions.push_back(action);
            return true;
        }
        return false;
    }

    bool AudioCommandRegistry::RemoveAction(CommandID triggerID, u32 actionIndex)
    {
        if (auto it = m_Triggers.find(triggerID.ID); it != m_Triggers.end())
        {
            auto& actions = it->second.Actions;
            if (actionIndex < actions.size())
            {
                actions.erase(actions.begin() + actionIndex);
                return true;
            }
        }
        return false;
    }

    TriggerCommand* AudioCommandRegistry::GetTrigger(CommandID id)
    {
        if (auto it = m_Triggers.find(id.ID); it != m_Triggers.end())
        {
            return &it->second;
        }
        return nullptr;
    }

    const TriggerCommand* AudioCommandRegistry::GetTrigger(CommandID id) const
    {
        if (auto it = m_Triggers.find(id.ID); it != m_Triggers.end())
        {
            return &it->second;
        }
        return nullptr;
    }

    bool AudioCommandRegistry::Contains(CommandID id) const
    {
        return m_Triggers.contains(id.ID);
    }

    void AudioCommandRegistry::Clear()
    {
        m_Triggers.clear();
    }

    // --- Serialization Helpers ---

    static const char* ActionTypeToString(ActionType type)
    {
        switch (type)
        {
            case ActionType::Play:
                return "Play";
            case ActionType::Stop:
                return "Stop";
            case ActionType::Pause:
                return "Pause";
            case ActionType::Resume:
                return "Resume";
        }
        return "Play";
    }

    static ActionType StringToActionType(const std::string& str)
    {
        if (str == "Stop")
            return ActionType::Stop;
        if (str == "Pause")
            return ActionType::Pause;
        if (str == "Resume")
            return ActionType::Resume;
        return ActionType::Play;
    }

    static const char* ActionContextToString(ActionContext ctx)
    {
        switch (ctx)
        {
            case ActionContext::GameObject:
                return "GameObject";
            case ActionContext::Global:
                return "Global";
        }
        return "GameObject";
    }

    static ActionContext StringToActionContext(const std::string& str)
    {
        if (str == "Global")
            return ActionContext::Global;
        return ActionContext::GameObject;
    }

    void AudioCommandRegistry::Serialize(const std::filesystem::path& filepath) const
    {
        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "AudioEvents" << YAML::Value;
        out << YAML::BeginMap;

        out << YAML::Key << "Triggers" << YAML::Value;
        out << YAML::BeginSeq;
        for (const auto& [id, cmd] : m_Triggers)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Name" << YAML::Value << cmd.DebugName;

            out << YAML::Key << "Actions" << YAML::Value;
            out << YAML::BeginSeq;
            for (const auto& action : cmd.Actions)
            {
                out << YAML::BeginMap;
                out << YAML::Key << "Type" << YAML::Value << ActionTypeToString(action.Type);
                out << YAML::Key << "AudioFilepath" << YAML::Value << action.AudioFilepath;
                out << YAML::Key << "Context" << YAML::Value << ActionContextToString(action.Context);
                out << YAML::Key << "VolumeMultiplier" << YAML::Value << action.VolumeMultiplier;
                out << YAML::Key << "PitchMultiplier" << YAML::Value << action.PitchMultiplier;
                out << YAML::Key << "Looping" << YAML::Value << action.Looping;
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;

            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        out << YAML::EndMap; // AudioEvents
        out << YAML::EndMap; // Root

        std::ofstream fout(filepath);
        if (!fout.is_open())
        {
            OLO_CORE_ERROR("AudioCommandRegistry: Failed to open '{}' for writing", filepath.string());
            return;
        }
        fout << out.c_str();
    }

    bool AudioCommandRegistry::Deserialize(const std::filesystem::path& filepath)
    {
        if (!std::filesystem::exists(filepath))
        {
            return false;
        }

        YAML::Node data;
        try
        {
            data = YAML::LoadFile(filepath.string());
        }
        catch (const YAML::ParserException& e)
        {
            OLO_CORE_ERROR("AudioCommandRegistry: Failed to parse '{}': {}", filepath.string(), e.what());
            return false;
        }

        const auto audioEvents = data["AudioEvents"];
        if (!audioEvents)
        {
            return false;
        }

        const auto triggers = audioEvents["Triggers"];
        if (!triggers || !triggers.IsSequence())
        {
            return false;
        }

        Clear();

        for (const auto& triggerNode : triggers)
        {
            auto name = triggerNode["Name"].as<std::string>("");
            if (name.empty())
            {
                continue;
            }

            auto id = AddTrigger(name);
            if (!id.IsValid())
            {
                continue;
            }

            if (const auto actionsNode = triggerNode["Actions"]; actionsNode && actionsNode.IsSequence())
            {
                for (const auto& actionNode : actionsNode)
                {
                    TriggerAction action;
                    action.Type = StringToActionType(actionNode["Type"].as<std::string>("Play"));
                    action.AudioFilepath = actionNode["AudioFilepath"].as<std::string>("");
                    action.Context = StringToActionContext(actionNode["Context"].as<std::string>("GameObject"));
                    action.VolumeMultiplier = actionNode["VolumeMultiplier"].as<f32>(1.0f);
                    action.PitchMultiplier = actionNode["PitchMultiplier"].as<f32>(1.0f);
                    action.Looping = actionNode["Looping"].as<bool>(false);

                    AddAction(id, action);
                }
            }
        }

        OLO_CORE_INFO("AudioCommandRegistry: Loaded {} triggers from '{}'", m_Triggers.size(), filepath.string());
        return true;
    }

} // namespace OloEngine::Audio
