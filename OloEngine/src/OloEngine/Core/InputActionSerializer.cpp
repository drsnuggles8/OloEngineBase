#include "OloEnginePCH.h"
#include "OloEngine/Core/InputActionSerializer.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Instrumentor.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>
#include <utility>
#include <vector>

namespace OloEngine
{
    static const char* BindingTypeToString(InputBindingType type)
    {
        switch (type)
        {
            case InputBindingType::Keyboard:
                return "Keyboard";
            case InputBindingType::Mouse:
                return "Mouse";
            case InputBindingType::GamepadButton:
                return "GamepadButton";
            case InputBindingType::GamepadAxis:
                return "GamepadAxis";
        }
        return "Unknown";
    }

    static std::optional<InputBindingType> StringToBindingType(const std::string& str)
    {
        if (str == "Keyboard")
            return InputBindingType::Keyboard;
        if (str == "Mouse")
            return InputBindingType::Mouse;
        if (str == "GamepadButton")
            return InputBindingType::GamepadButton;
        if (str == "GamepadAxis")
            return InputBindingType::GamepadAxis;
        return std::nullopt;
    }

    // Emit the body of a single action map (a YAML map node holding Name + Actions).
    // The per-map writer used by SerializeContexts for each context; paired with
    // ParseActionMapNode, which reads back the same shape (including for legacy files).
    static void EmitActionMapNode(YAML::Emitter& out, const InputActionMap& map)
    {
        // Collect and sort action names for deterministic output
        std::vector<std::string> sortedNames;
        sortedNames.reserve(map.Actions.size());
        for (const auto& [name, _] : map.Actions)
        {
            sortedNames.push_back(name);
        }
        std::ranges::sort(sortedNames);

        out << YAML::BeginMap;
        out << YAML::Key << "Name" << YAML::Value << map.Name;
        out << YAML::Key << "Actions" << YAML::Value;
        {
            out << YAML::BeginSeq;
            for (const auto& actionName : sortedNames)
            {
                const auto& action = map.Actions.at(actionName);
                out << YAML::BeginMap;
                out << YAML::Key << "Name" << YAML::Value << action.Name;
                out << YAML::Key << "Bindings" << YAML::Value;
                {
                    out << YAML::BeginSeq;
                    for (const auto& binding : action.Bindings)
                    {
                        out << YAML::BeginMap;
                        out << YAML::Key << "Type" << YAML::Value << BindingTypeToString(binding.Type);

                        if (binding.Type == InputBindingType::Keyboard || binding.Type == InputBindingType::Mouse)
                        {
                            out << YAML::Key << "Code" << YAML::Value << binding.Code;
                        }
                        else if (binding.Type == InputBindingType::GamepadButton)
                        {
                            out << YAML::Key << "Button" << YAML::Value << GamepadButtonToString(binding.GPButton);
                        }
                        else if (binding.Type == InputBindingType::GamepadAxis)
                        {
                            out << YAML::Key << "Axis" << YAML::Value << GamepadAxisToString(binding.GPAxis);
                            out << YAML::Key << "Threshold" << YAML::Value << binding.AxisThreshold;
                            out << YAML::Key << "Positive" << YAML::Value << binding.AxisPositive;
                        }
                        else
                        {
                            // No additional handling required.
                        }

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

    // Parse a single action map from a YAML map node (the inverse of EmitActionMapNode).
    // Malformed actions/bindings are skipped with a warning rather than aborting the parse,
    // matching the robustness contract exercised by the fuzz-regression tests. The caller
    // must have verified mapNode.IsMap(). May throw on a wrong-typed node; callers wrap.
    static InputActionMap ParseActionMapNode(const YAML::Node& mapNode)
    {
        InputActionMap map;
        if (auto nameNode = mapNode["Name"]; nameNode && nameNode.IsScalar())
        {
            map.Name = nameNode.as<std::string>();
        }

        auto actionsNode = mapNode["Actions"];
        if (!actionsNode || !actionsNode.IsSequence())
        {
            OLO_CORE_WARN("InputActionSerializer: No 'Actions' sequence found, returning empty map");
            return map;
        }

        for (const auto& actionNode : actionsNode)
        {
            if (!actionNode.IsMap())
            {
                OLO_CORE_WARN("InputActionSerializer: Skipping non-map action entry");
                continue;
            }

            auto actionNameNode = actionNode["Name"];
            if (!actionNameNode || !actionNameNode.IsScalar())
            {
                OLO_CORE_WARN("InputActionSerializer: Skipping action with missing or non-scalar 'Name'");
                continue;
            }

            InputAction action;
            action.Name = actionNameNode.as<std::string>();

            if (action.Name.empty())
            {
                OLO_CORE_WARN("InputActionSerializer: Skipping action with empty name");
                continue;
            }

            if (auto bindingsNode = actionNode["Bindings"]; bindingsNode && bindingsNode.IsSequence())
            {
                for (const auto& bindingNode : bindingsNode)
                {
                    try
                    {
                        if (!bindingNode.IsMap())
                        {
                            OLO_CORE_WARN("InputActionSerializer: Skipping non-map binding in action '{}'", action.Name);
                            continue;
                        }

                        auto typeNode = bindingNode["Type"];
                        if (!typeNode || !typeNode.IsScalar())
                        {
                            OLO_CORE_WARN("InputActionSerializer: Skipping binding with missing or non-scalar Type in action '{}'", action.Name);
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

                        if (*bindingType == InputBindingType::Keyboard || *bindingType == InputBindingType::Mouse)
                        {
                            auto codeNode = bindingNode["Code"];
                            if (!codeNode || !codeNode.IsScalar())
                            {
                                OLO_CORE_WARN("InputActionSerializer: Skipping binding with missing or non-scalar Code in action '{}'", action.Name);
                                continue;
                            }
                            binding.Code = codeNode.as<u16>();
                        }
                        else if (*bindingType == InputBindingType::GamepadButton)
                        {
                            auto buttonNode = bindingNode["Button"];
                            if (!buttonNode || !buttonNode.IsScalar())
                            {
                                OLO_CORE_WARN("InputActionSerializer: Skipping gamepad button binding with missing or non-scalar Button in action '{}'", action.Name);
                                continue;
                            }
                            auto btn = StringToGamepadButton(buttonNode.as<std::string>());
                            if (!btn)
                            {
                                OLO_CORE_WARN("InputActionSerializer: Unknown gamepad button '{}' in action '{}'", buttonNode.as<std::string>(), action.Name);
                                continue;
                            }
                            binding.GPButton = *btn;
                        }
                        else if (*bindingType == InputBindingType::GamepadAxis)
                        {
                            auto axisNode = bindingNode["Axis"];
                            if (!axisNode || !axisNode.IsScalar())
                            {
                                OLO_CORE_WARN("InputActionSerializer: Skipping gamepad axis binding with missing or non-scalar Axis in action '{}'", action.Name);
                                continue;
                            }
                            auto ax = StringToGamepadAxis(axisNode.as<std::string>());
                            if (!ax)
                            {
                                OLO_CORE_WARN("InputActionSerializer: Unknown gamepad axis '{}' in action '{}'", axisNode.as<std::string>(), action.Name);
                                continue;
                            }
                            binding.GPAxis = *ax;
                            if (auto threshNode = bindingNode["Threshold"]; threshNode && threshNode.IsScalar())
                            {
                                binding.AxisThreshold = threshNode.as<f32>();
                                if (!std::isfinite(binding.AxisThreshold))
                                    binding.AxisThreshold = 0.5f;
                            }
                            if (auto posNode = bindingNode["Positive"]; posNode && posNode.IsScalar())
                            {
                                binding.AxisPositive = posNode.as<bool>();
                            }
                        }
                        else
                        {
                            // No additional handling required.
                        }

                        action.Bindings.push_back(binding);
                    }
                    catch (const YAML::Exception& e)
                    {
                        OLO_CORE_WARN("InputActionSerializer: Skipping malformed binding in action '{}': {}", action.Name, e.what());
                        continue;
                    }
                    catch (const std::exception& e)
                    {
                        OLO_CORE_WARN("InputActionSerializer: Skipping malformed binding in action '{}': {}", action.Name, e.what());
                        continue;
                    }
                }
            }

            map.AddAction(std::move(action));
        }

        return map;
    }

    static bool WriteEmitterToFile(YAML::Emitter& out, const std::filesystem::path& filepath)
    {
        if (!out.good())
        {
            OLO_CORE_ERROR("InputActionSerializer: YAML emitter error: {}", out.GetLastError());
            return false;
        }

        // Ensure parent directory exists
        if (auto parentPath = filepath.parent_path(); !parentPath.empty())
        {
            std::filesystem::create_directories(parentPath);
        }

        std::ofstream fout(filepath);
        if (!fout.is_open())
        {
            OLO_CORE_ERROR("InputActionSerializer: Failed to open file for writing: {}", filepath.string());
            return false;
        }
        fout << out.c_str();
        return true;
    }

    static std::optional<YAML::Node> LoadRootNode(const std::filesystem::path& filepath)
    {
        if (!std::filesystem::exists(filepath))
        {
            OLO_CORE_WARN("InputActionSerializer: File does not exist: {}", filepath.string());
            return std::nullopt;
        }

        try
        {
            return YAML::LoadFile(filepath.string());
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("InputActionSerializer: Failed to parse YAML: {}", e.what());
            return std::nullopt;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("InputActionSerializer: Failed to parse YAML: {}", e.what());
            return std::nullopt;
        }
    }

    bool InputActionSerializer::SerializeContexts(const ContextMaps& contexts, const std::filesystem::path& filepath)
    {
        OLO_PROFILE_FUNCTION();

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "InputActionContexts" << YAML::Value;
        {
            out << YAML::BeginSeq;
            // Iterate the compile-time enum-ordered list for deterministic output,
            // regardless of the unordered_map's internal ordering.
            for (const auto ctx : AllInputContextTypes)
            {
                auto it = contexts.find(ctx);
                if (it == contexts.end())
                {
                    continue;
                }
                out << YAML::BeginMap;
                out << YAML::Key << "Context" << YAML::Value << InputContextTypeToString(ctx);
                out << YAML::Key << "Map" << YAML::Value;
                EmitActionMapNode(out, it->second);
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
        }
        out << YAML::EndMap;

        return WriteEmitterToFile(out, filepath);
    }

    std::optional<InputActionSerializer::ContextMaps> InputActionSerializer::DeserializeContexts(const std::filesystem::path& filepath)
    {
        OLO_PROFILE_FUNCTION();

        auto data = LoadRootNode(filepath);
        if (!data)
        {
            return std::nullopt;
        }

        try
        {
            if (!data->IsMap())
            {
                OLO_CORE_ERROR("InputActionSerializer: Root YAML node is not a map");
                return std::nullopt;
            }

            // New multi-context format.
            if (auto contextsNode = (*data)["InputActionContexts"]; contextsNode && contextsNode.IsSequence())
            {
                ContextMaps result;
                for (const auto& entry : contextsNode)
                {
                    try
                    {
                        if (!entry.IsMap())
                        {
                            OLO_CORE_WARN("InputActionSerializer: Skipping non-map context entry");
                            continue;
                        }

                        auto ctxNode = entry["Context"];
                        if (!ctxNode || !ctxNode.IsScalar())
                        {
                            OLO_CORE_WARN("InputActionSerializer: Skipping context entry with missing or non-scalar 'Context'");
                            continue;
                        }

                        auto ctx = StringToInputContextType(ctxNode.as<std::string>());
                        if (!ctx)
                        {
                            OLO_CORE_WARN("InputActionSerializer: Unknown input context '{}'", ctxNode.as<std::string>());
                            continue;
                        }

                        auto mapNode = entry["Map"];
                        if (!mapNode || !mapNode.IsMap())
                        {
                            OLO_CORE_WARN("InputActionSerializer: Skipping context '{}' with missing or non-map 'Map'", ctxNode.as<std::string>());
                            continue;
                        }

                        result[*ctx] = ParseActionMapNode(mapNode);
                    }
                    catch (const YAML::Exception& e)
                    {
                        OLO_CORE_WARN("InputActionSerializer: Skipping malformed context entry: {}", e.what());
                        continue;
                    }
                    catch (const std::exception& e)
                    {
                        OLO_CORE_WARN("InputActionSerializer: Skipping malformed context entry: {}", e.what());
                        continue;
                    }
                }
                return result;
            }

            // Legacy single-map format — load it as the Gameplay context.
            if (auto legacyNode = (*data)["InputActionMap"]; legacyNode && legacyNode.IsMap())
            {
                ContextMaps result;
                result[InputContextType::Gameplay] = ParseActionMapNode(legacyNode);
                return result;
            }

            OLO_CORE_ERROR("InputActionSerializer: Missing 'InputActionContexts' sequence or legacy 'InputActionMap' root node");
            return std::nullopt;
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("InputActionSerializer: YAML exception during deserialise: {}", e.what());
            return std::nullopt;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("InputActionSerializer: Exception during deserialise: {}", e.what());
            return std::nullopt;
        }
    }

} // namespace OloEngine
