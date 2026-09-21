#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Containers/String.h"

#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <glm/glm.hpp>

namespace OloEngine
{
    // Shared YAML/editor property DTO: these alternatives are dispatched by
    // yaml-cpp conversion and the editor's std::get_if<std::string> controls.
    // Runtime variable storage uses its own FString alternative instead.
    using DialoguePropertyValue = std::variant<bool, i32, f32, std::string>;

    // Plain data-transfer structs: members intentionally use PascalCase without m_ prefix
    // to keep them lightweight POD-style types matching their YAML serialization keys.
    struct DialogueNodeData
    {
        UUID ID;
        FString Type; // "dialogue", "choice", "condition", "action"
        FString Name;
        std::unordered_map<std::string, DialoguePropertyValue> Properties;
        glm::vec2 EditorPosition{ 0.0f, 0.0f };
    };

    struct DialogueChoice
    {
        FString Text;
        UUID TargetNodeID = 0;
        FString Condition; // optional condition name (empty = always available)
    };

    // Both strings own separate heap buffers; the remaining UUID is a scalar.
    // No member points into the choice, so container growth may relocate it.
    template<>
    struct TIsTriviallyRelocatable<DialogueChoice>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(DialogueChoice::Text)>::Value &&
                                      TIsTriviallyRelocatable<decltype(DialogueChoice::TargetNodeID)>::Value &&
                                      TIsTriviallyRelocatable<decltype(DialogueChoice::Condition)>::Value;
    };

    struct DialogueConnection
    {
        UUID SourceNodeID;
        UUID TargetNodeID;
        FString SourcePort;
        FString TargetPort;
    };

    // IDs are scalars and port names own independent heap storage.
    template<>
    struct TIsTriviallyRelocatable<DialogueConnection>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(DialogueConnection::SourceNodeID)>::Value &&
                                      TIsTriviallyRelocatable<decltype(DialogueConnection::TargetNodeID)>::Value &&
                                      TIsTriviallyRelocatable<decltype(DialogueConnection::SourcePort)>::Value &&
                                      TIsTriviallyRelocatable<decltype(DialogueConnection::TargetPort)>::Value;
    };

    struct DialogueEditorSnapshot
    {
        // Editor undo DTO: nodes contain standard property maps and must be
        // copied normally, not bitwise-relocated. Runtime owns stable list nodes.
        std::vector<DialogueNodeData> Nodes;
        TArray<DialogueConnection> Connections;
        UUID RootNodeID = 0;
    };

} // namespace OloEngine
