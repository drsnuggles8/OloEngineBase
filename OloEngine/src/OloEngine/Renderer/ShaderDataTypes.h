#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    /**
     * @brief Shader data types for uniform buffer layouts
     */
    enum class ShaderDataType
    {
        None = 0,
        Float, Float2, Float3, Float4,
        Mat3, Mat4,
        Int, Int2, Int3, Int4,
        Bool,
        Sampler2D, SamplerCube
    };

    /**
     * @brief Shader uniform declaration with layout information
     */
    struct ShaderUniformDeclaration
    {
        std::string Name;
        ShaderDataType Type;
        u32 Size;
        u32 Offset;
        u32 ArraySize = 1;
        
        u32 GetComponentCount() const;
        static u32 ShaderDataTypeSize(ShaderDataType type);
    };

    /**
     * @brief Get the size in bytes of a shader data type
     */
    inline u32 ShaderUniformDeclaration::ShaderDataTypeSize(ShaderDataType type)
    {
        switch (type)
        {
            case ShaderDataType::Float:    return 4;
            case ShaderDataType::Float2:   return 4 * 2;
            case ShaderDataType::Float3:   return 4 * 3;
            case ShaderDataType::Float4:   return 4 * 4;
            case ShaderDataType::Mat3:     return 4 * 3 * 3;
            case ShaderDataType::Mat4:     return 4 * 4 * 4;
            case ShaderDataType::Int:      return 4;
            case ShaderDataType::Int2:     return 4 * 2;
            case ShaderDataType::Int3:     return 4 * 3;
            case ShaderDataType::Int4:     return 4 * 4;
            case ShaderDataType::Bool:     return 1;
            default:                       return 0;
        }
    }

    inline u32 ShaderUniformDeclaration::GetComponentCount() const
    {
        switch (Type)
        {
            case ShaderDataType::Float:   return 1;
            case ShaderDataType::Float2:  return 2;
            case ShaderDataType::Float3:  return 3;
            case ShaderDataType::Float4:  return 4;
            case ShaderDataType::Mat3:    return 3; // 3* float3
            case ShaderDataType::Mat4:    return 4; // 4* float4
            case ShaderDataType::Int:     return 1;
            case ShaderDataType::Int2:    return 2;
            case ShaderDataType::Int3:    return 3;
            case ShaderDataType::Int4:    return 4;
            case ShaderDataType::Bool:    return 1;
            default:                      return 0;
        }
    }
}
