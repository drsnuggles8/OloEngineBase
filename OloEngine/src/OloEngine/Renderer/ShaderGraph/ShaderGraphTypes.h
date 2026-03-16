#pragma once

#include "OloEngine/Core/Base.h"
#include <string>

namespace OloEngine
{
    /// Data types that can flow through shader graph pins
    enum class ShaderGraphPinType : u8
    {
        Float = 0,
        Vec2,
        Vec3,
        Vec4,
        Bool,
        Texture2D,
        Mat3,
        Mat4
    };

    /// Direction of a pin on a node
    enum class ShaderGraphPinDirection : u8
    {
        Input = 0,
        Output
    };

    /// Categories for organizing nodes in the creation menu
    enum class ShaderGraphNodeCategory : u8
    {
        Input = 0,
        Math,
        Texture,
        Utility,
        Custom,
        Compute,
        Output
    };

    /// Returns the GLSL type name for a given pin type
    inline const char* PinTypeToGLSL(ShaderGraphPinType type)
    {
        switch (type)
        {
            case ShaderGraphPinType::Float:
                return "float";
            case ShaderGraphPinType::Vec2:
                return "vec2";
            case ShaderGraphPinType::Vec3:
                return "vec3";
            case ShaderGraphPinType::Vec4:
                return "vec4";
            case ShaderGraphPinType::Bool:
                return "bool";
            case ShaderGraphPinType::Texture2D:
                return "sampler2D";
            case ShaderGraphPinType::Mat3:
                return "mat3";
            case ShaderGraphPinType::Mat4:
                return "mat4";
        }
        return "float";
    }

    /// Returns a human-readable name for a pin type
    inline const char* PinTypeToString(ShaderGraphPinType type)
    {
        switch (type)
        {
            case ShaderGraphPinType::Float:
                return "Float";
            case ShaderGraphPinType::Vec2:
                return "Vec2";
            case ShaderGraphPinType::Vec3:
                return "Vec3";
            case ShaderGraphPinType::Vec4:
                return "Vec4";
            case ShaderGraphPinType::Bool:
                return "Bool";
            case ShaderGraphPinType::Texture2D:
                return "Texture2D";
            case ShaderGraphPinType::Mat3:
                return "Mat3";
            case ShaderGraphPinType::Mat4:
                return "Mat4";
        }
        return "Unknown";
    }

    /// Returns a human-readable name for a node category
    inline const char* NodeCategoryToString(ShaderGraphNodeCategory category)
    {
        switch (category)
        {
            case ShaderGraphNodeCategory::Input:
                return "Input";
            case ShaderGraphNodeCategory::Math:
                return "Math";
            case ShaderGraphNodeCategory::Texture:
                return "Texture";
            case ShaderGraphNodeCategory::Utility:
                return "Utility";
            case ShaderGraphNodeCategory::Custom:
                return "Custom";
            case ShaderGraphNodeCategory::Compute:
                return "Compute";
            case ShaderGraphNodeCategory::Output:
                return "Output";
        }
        return "Unknown";
    }

    /// Check if an implicit type conversion is valid (e.g., Float→Vec3 via broadcast)
    inline bool CanConvertPinType(ShaderGraphPinType from, ShaderGraphPinType to)
    {
        if (from == to)
            return true;

        // Float can broadcast to any vector type
        if (from == ShaderGraphPinType::Float)
        {
            return to == ShaderGraphPinType::Vec2 || to == ShaderGraphPinType::Vec3 || to == ShaderGraphPinType::Vec4;
        }

        // Vec4 can truncate to Vec3 or Vec2
        if (from == ShaderGraphPinType::Vec4)
            return to == ShaderGraphPinType::Vec3 || to == ShaderGraphPinType::Vec2;

        // Vec3 can truncate to Vec2
        if (from == ShaderGraphPinType::Vec3)
            return to == ShaderGraphPinType::Vec2;

        return false;
    }

    /// Returns the GLSL expression to convert a value from one pin type to another
    inline std::string GenerateTypeConversion(const std::string& expression, ShaderGraphPinType from, ShaderGraphPinType to)
    {
        if (from == to)
            return expression;

        // Float broadcast
        if (from == ShaderGraphPinType::Float)
        {
            switch (to)
            {
                case ShaderGraphPinType::Vec2:
                    return "vec2(" + expression + ")";
                case ShaderGraphPinType::Vec3:
                    return "vec3(" + expression + ")";
                case ShaderGraphPinType::Vec4:
                    return "vec4(vec3(" + expression + "), 1.0)";
                default:
                    break;
            }
        }

        // Vec4 → Vec3/Vec2 truncation
        if (from == ShaderGraphPinType::Vec4)
        {
            if (to == ShaderGraphPinType::Vec3)
                return expression + ".xyz";
            if (to == ShaderGraphPinType::Vec2)
                return expression + ".xy";
        }

        // Vec3 → Vec2 truncation
        if (from == ShaderGraphPinType::Vec3 && to == ShaderGraphPinType::Vec2)
            return expression + ".xy";

        return expression;
    }

    /// Returns the number of components in a vector pin type
    inline u32 PinTypeComponentCount(ShaderGraphPinType type)
    {
        switch (type)
        {
            case ShaderGraphPinType::Float:
            case ShaderGraphPinType::Bool:
                return 1;
            case ShaderGraphPinType::Vec2:
                return 2;
            case ShaderGraphPinType::Vec3:
                return 3;
            case ShaderGraphPinType::Vec4:
                return 4;
            default:
                return 0;
        }
    }

} // namespace OloEngine
