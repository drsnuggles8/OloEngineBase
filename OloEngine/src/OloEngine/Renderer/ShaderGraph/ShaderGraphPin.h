#pragma once

#include "OloEngine/Core/UUID.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphTypes.h"

#include <glm/glm.hpp>
#include <string>
#include <variant>

namespace OloEngine
{
    /// Default value types that a pin can hold when not connected
    using ShaderGraphPinValue = std::variant<
        std::monostate, // No default / not applicable (e.g., Texture2D)
        f32,
        glm::vec2,
        glm::vec3,
        glm::vec4,
        bool>;

    /// A single input or output connection point on a shader graph node
    struct ShaderGraphPin
    {
        UUID ID;
        std::string Name;
        ShaderGraphPinType Type = ShaderGraphPinType::Float;
        ShaderGraphPinDirection Direction = ShaderGraphPinDirection::Input;
        UUID NodeID;
        ShaderGraphPinValue DefaultValue;

        ShaderGraphPin() = default;
        ShaderGraphPin(UUID id, std::string name, ShaderGraphPinType type, ShaderGraphPinDirection direction, UUID nodeID)
            : ID(id), Name(std::move(name)), Type(type), Direction(direction), NodeID(nodeID)
        {
        }

        /// Returns the default value as a GLSL literal string
        std::string GetDefaultValueGLSL() const
        {
            return std::visit([this](const auto& val) -> std::string
                              {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, std::monostate>)
                {
                    // Return type-appropriate zero
                    switch (Type)
                    {
                        case ShaderGraphPinType::Float:
                            return "0.0";
                        case ShaderGraphPinType::Vec2:
                            return "vec2(0.0)";
                        case ShaderGraphPinType::Vec3:
                            return "vec3(0.0)";
                        case ShaderGraphPinType::Vec4:
                            return "vec4(0.0)";
                        case ShaderGraphPinType::Bool:
                            return "false";
                        case ShaderGraphPinType::Texture2D:
                            return "sampler2D(0)";
                        case ShaderGraphPinType::Mat3:
                            return "mat3(0.0)";
                        case ShaderGraphPinType::Mat4:
                            return "mat4(0.0)";
                        default:
                            return "0.0";
                    }
                }
                else if constexpr (std::is_same_v<T, f32>)
                    return std::to_string(val);
                else if constexpr (std::is_same_v<T, glm::vec2>)
                    return "vec2(" + std::to_string(val.x) + ", " + std::to_string(val.y) + ")";
                else if constexpr (std::is_same_v<T, glm::vec3>)
                    return "vec3(" + std::to_string(val.x) + ", " + std::to_string(val.y) + ", " + std::to_string(val.z) + ")";
                else if constexpr (std::is_same_v<T, glm::vec4>)
                    return "vec4(" + std::to_string(val.x) + ", " + std::to_string(val.y) + ", " + std::to_string(val.z) + ", " + std::to_string(val.w) + ")";
                else if constexpr (std::is_same_v<T, bool>)
                    return val ? "true" : "false";
                else
                    return "0.0"; }, DefaultValue);
        }
    };

} // namespace OloEngine
