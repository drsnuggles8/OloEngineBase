#include "OloEnginePCH.h"
#include "OloEngine/Audio/AudioEvents/AudioCommandRegistry.h"

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>

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
        return "Unknown";
    }

    static std::optional<ActionType> StringToActionType(const std::string& str)
    {
        if (str == "Play")
            return ActionType::Play;
        if (str == "Stop")
            return ActionType::Stop;
        if (str == "Pause")
            return ActionType::Pause;
        if (str == "Resume")
            return ActionType::Resume;
        return std::nullopt;
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
        return "Unknown";
    }

    static std::optional<ActionContext> StringToActionContext(const std::string& str)
    {
        if (str == "GameObject")
            return ActionContext::GameObject;
        if (str == "Global")
            return ActionContext::Global;
        return std::nullopt;
    }

    bool AudioCommandRegistry::Serialize(const std::filesystem::path& filepath) const
    {
        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "AudioEvents" << YAML::Value;
        out << YAML::BeginMap;

        out << YAML::Key << "Triggers" << YAML::Value;
        out << YAML::BeginSeq;

        // Sort by DebugName for deterministic YAML output (m_Triggers is unordered_map)
        std::vector<const TriggerCommand*> sorted;
        sorted.reserve(m_Triggers.size());
        for (const auto& [id, cmd] : m_Triggers)
        {
            sorted.push_back(&cmd);
        }
        std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b)
                  { return a->DebugName < b->DebugName; });

        for (const auto* cmd : sorted)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Name" << YAML::Value << cmd->DebugName;

            out << YAML::Key << "Actions" << YAML::Value;
            out << YAML::BeginSeq;
            for (const auto& action : cmd->Actions)
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
            return false;
        }
        fout << out.c_str();
        fout.flush();
        if (fout.fail())
        {
            OLO_CORE_ERROR("AudioCommandRegistry: Failed to write '{}'", filepath.string());
            return false;
        }
        return true;
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
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("AudioCommandRegistry: Failed to load '{}': {}", filepath.string(), e.what());
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
                    auto typeStr = actionNode["Type"].as<std::string>("Play");
                    auto typeOpt = StringToActionType(typeStr);
                    if (!typeOpt)
                    {
                        OLO_CORE_WARN("AudioCommandRegistry: Unknown ActionType '{}' in trigger '{}', skipping action", typeStr, name);
                        continue;
                    }
                    auto ctxStr = actionNode["Context"].as<std::string>("GameObject");
                    auto ctxOpt = StringToActionContext(ctxStr);
                    if (!ctxOpt)
                    {
                        OLO_CORE_WARN("AudioCommandRegistry: Unknown ActionContext '{}' in trigger '{}', skipping action", ctxStr, name);
                        continue;
                    }

                    TriggerAction action;
                    action.Type = *typeOpt;
                    action.AudioFilepath = actionNode["AudioFilepath"].as<std::string>("");
                    action.Context = *ctxOpt;
                    action.VolumeMultiplier = actionNode["VolumeMultiplier"].as<f32>(1.0f);
                    if (!std::isfinite(action.VolumeMultiplier) || action.VolumeMultiplier < 0.0f || action.VolumeMultiplier > 10.0f)
                    {
                        OLO_CORE_WARN("AudioCommandRegistry: Invalid VolumeMultiplier {:.4f} in '{}', using 1.0", action.VolumeMultiplier, name);
                        action.VolumeMultiplier = 1.0f;
                    }
                    action.PitchMultiplier = actionNode["PitchMultiplier"].as<f32>(1.0f);
                    if (!std::isfinite(action.PitchMultiplier) || action.PitchMultiplier < 0.01f || action.PitchMultiplier > 4.0f)
                    {
                        OLO_CORE_WARN("AudioCommandRegistry: Invalid PitchMultiplier {:.4f} in '{}', using 1.0", action.PitchMultiplier, name);
                        action.PitchMultiplier = 1.0f;
                    }
                    action.Looping = actionNode["Looping"].as<bool>(false);

                    AddAction(id, action);
                }
            }
        }

        OLO_CORE_INFO("AudioCommandRegistry: Loaded {} triggers from '{}'", m_Triggers.size(), filepath.string());
        return true;
    }

} // namespace OloEngine::Audio
