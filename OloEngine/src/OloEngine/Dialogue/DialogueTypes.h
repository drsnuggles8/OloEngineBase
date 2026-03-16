#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"

#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <glm/glm.hpp>

namespace OloEngine
{
    using DialoguePropertyValue = std::variant<bool, i32, f32, std::string>;

    // Plain data-transfer structs: members intentionally use PascalCase without m_ prefix
    // to keep them lightweight POD-style types matching their YAML serialization keys.
    struct DialogueNodeData
    {
        UUID ID;
        std::string Type; // "dialogue", "choice", "condition", "action"
        std::string Name;
        std::unordered_map<std::string, DialoguePropertyValue> Properties;
        glm::vec2 EditorPosition{ 0.0f, 0.0f };
    };

    struct DialogueChoice
    {
        std::string Text;
        UUID TargetNodeID = 0;
        std::string Condition; // optional condition name (empty = always available)
    };

    struct DialogueConnection
    {
        UUID SourceNodeID;
        UUID TargetNodeID;
        std::string SourcePort;
        std::string TargetPort;
    };

    struct DialogueEditorSnapshot
    {
        std::vector<DialogueNodeData> Nodes;
        std::vector<DialogueConnection> Connections;
        UUID RootNodeID = 0;
    };

} // namespace OloEngine
