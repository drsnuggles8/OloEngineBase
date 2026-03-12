#include "OloEnginePCH.h"
#include "OloEngine/Core/InputActionSerializer.h"
#include "OloEngine/Core/Log.h"

#include <yaml-cpp/yaml.h>

#include <fstream>

namespace OloEngine
{
    static const char* BindingTypeToString(InputBindingType type)
    {
        switch (type)
        {
            case InputBindingType::Keyboard: return "Keyboard";
            case InputBindingType::Mouse:    return "Mouse";
        }
        return "Unknown";
    }

    static std::optional<InputBindingType> StringToBindingType(const std::string& str)
    {
        if (str == "Keyboard") return InputBindingType::Keyboard;
        if (str == "Mouse")    return InputBindingType::Mouse;
        return std::nullopt;
    }

    bool InputActionSerializer::Serialize(const InputActionMap& map, const std::filesystem::path& filepath)
    {
        // Ensure parent directory exists
        if (auto parentPath = filepath.parent_path(); !parentPath.empty())
        {
            std::filesystem::create_directories(parentPath);
        }

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "InputActionMap" << YAML::Value;
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Name" << YAML::Value << map.Name;
            out << YAML::Key << "Actions" << YAML::Value;
            {
                out << YAML::BeginSeq;
                for (const auto& [actionName, action] : map.Actions)
                {
                    out << YAML::BeginMap;
                    out << YAML::Key << "Name" << YAML::Value << action.Name;
                    out << YAML::Key << "Bindings" << YAML::Value;
                    {
                        out << YAML::BeginSeq;
                        for (const auto& binding : action.Bindings)
                        {
                            out << YAML::BeginMap;
                            out << YAML::Key << "Type" << YAML::Value << BindingTypeToString(binding.Type);
                            out << YAML::Key << "Code" << YAML::Value << binding.Code;
                            out << YAML::EndMap;
                        }
                        out << YAML::EndSeq;
                    }
                    out << YAML::EndMap;
                }
                out << YAML::EndSeq;
            }
            out << YAML::EndMap;
        }
        out << YAML::EndMap;

        std::ofstream fout(filepath);
        if (!fout.is_open())
        {
            OLO_CORE_ERROR("InputActionSerializer: Failed to open file for writing: {}", filepath.string());
            return false;
        }
        fout << out.c_str();
        return true;
    }

    std::optional<InputActionMap> InputActionSerializer::Deserialize(const std::filesystem::path& filepath)
    {
        if (!std::filesystem::exists(filepath))
        {
            OLO_CORE_WARN("InputActionSerializer: File does not exist: {}", filepath.string());
            return std::nullopt;
        }

        YAML::Node data;
        try
        {
            data = YAML::LoadFile(filepath.string());
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("InputActionSerializer: Failed to parse YAML: {}", e.what());
            return std::nullopt;
        }

        auto rootNode = data["InputActionMap"];
        if (!rootNode)
        {
            OLO_CORE_ERROR("InputActionSerializer: Missing 'InputActionMap' root node");
            return std::nullopt;
        }

        InputActionMap map;
        if (auto nameNode = rootNode["Name"])
        {
            map.Name = nameNode.as<std::string>();
        }

        auto actionsNode = rootNode["Actions"];
        if (!actionsNode || !actionsNode.IsSequence())
        {
            OLO_CORE_WARN("InputActionSerializer: No 'Actions' sequence found, returning empty map");
            return map;
        }

        for (const auto& actionNode : actionsNode)
        {
            auto actionNameNode = actionNode["Name"];
            if (!actionNameNode)
            {
                OLO_CORE_WARN("InputActionSerializer: Skipping action with missing 'Name'");
                continue;
            }

            InputAction action;
            action.Name = actionNameNode.as<std::string>();

            if (action.Name.empty())
            {
                OLO_CORE_WARN("InputActionSerializer: Skipping action with empty name");
                continue;
            }

            auto bindingsNode = actionNode["Bindings"];
            if (bindingsNode && bindingsNode.IsSequence())
            {
                for (const auto& bindingNode : bindingsNode)
                {
                    auto typeNode = bindingNode["Type"];
                    auto codeNode = bindingNode["Code"];

                    if (!typeNode || !codeNode)
                    {
                        OLO_CORE_WARN("InputActionSerializer: Skipping binding with missing Type or Code in action '{}'", action.Name);
                        continue;
                    }

                    auto bindingType = StringToBindingType(typeNode.as<std::string>());
                    if (!bindingType)
                    {
                        OLO_CORE_WARN("InputActionSerializer: Unknown binding type '{}' in action '{}'", typeNode.as<std::string>(), action.Name);
                        continue;
                    }

                    InputBinding binding;
                    binding.Type = *bindingType;
                    binding.Code = codeNode.as<u16>();
                    action.Bindings.push_back(binding);
                }
            }

            map.AddAction(std::move(action));
        }

        return map;
    }

} // namespace OloEngine
