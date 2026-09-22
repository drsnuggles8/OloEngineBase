#include <utility>

#pragma once

#include "ShaderDataTypes.h"
#include "OloEngine/Containers/Array.h"

namespace OloEngine
{
    // @brief Buffer usage patterns for optimization hints
    enum class BufferUsage
    {
        Static = 0, // Data will be modified once and used many times
        Dynamic,    // Data will be modified repeatedly and used many times
        Stream      // Data will be modified once and used at most a few times
    };

    struct BufferElement
    {
        FString name;
        ShaderDataType dataType{};
        u32 size{};
        sizet offset{};
        bool normalized{};

        BufferElement() = default;

        BufferElement(ShaderDataType const type, std::string_view elementName, const bool isNormalized = false)
            : name(elementName), dataType(type), size(ShaderUniformDeclaration::ShaderDataTypeSize(type)), offset(0), normalized(isNormalized)
        {
        }

        [[nodiscard("Store this!")]] u32 GetComponentCount() const
        {
            switch (dataType)
            {
                using enum OloEngine::ShaderDataType;
                case Float:
                    return 1;
                case Float2:
                    return 2;
                case Float3:
                    return 3;
                case Float4:
                    return 4;
                case Mat3:
                    return 3; // 3* float3
                case Mat4:
                    return 4; // 4* float4
                case Int:
                    return 1;
                case Int2:
                    return 2;
                case Int3:
                    return 3;
                case Int4:
                    return 4;
                case Bool:
                    return 1;
                case Sampler2D:
                case SamplerCube:
                    return 1;
                case None:
                    break;
            }

            OLO_CORE_ASSERT(false, "Unknown ShaderDataType!");
            return 0;
        }
    };

    template<>
    struct TIsTriviallyRelocatable<BufferElement>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(BufferElement::name)> &&
                                      TIsTriviallyRelocatable_V<decltype(BufferElement::dataType)> &&
                                      TIsTriviallyRelocatable_V<decltype(BufferElement::size)> &&
                                      TIsTriviallyRelocatable_V<decltype(BufferElement::offset)> &&
                                      TIsTriviallyRelocatable_V<decltype(BufferElement::normalized)>;
    };

    struct VertexData
    {
        const void* data;
        u32 size;
    };

    struct UniformData
    {
        const void* data;
        u32 size;
        u32 offset;
    };

    class BufferLayout
    {
      public:
        BufferLayout() = default;

        BufferLayout(const std::initializer_list<BufferElement>& elements)
            : m_Elements(elements)
        {
            CalculateOffsetsAndStride();
        }

        [[nodiscard("Store this!")]] u32 GetStride() const
        {
            return m_Stride;
        }
        [[nodiscard("Store this!")]] TArray<BufferElement> GetElements() const
        {
            return m_Elements;
        }

        [[nodiscard("Store this!")]] auto begin()
        {
            return m_Elements.begin();
        }
        [[nodiscard("Store this!")]] auto end()
        {
            return m_Elements.end();
        }
        [[nodiscard("Store this!")]] auto begin() const
        {
            return m_Elements.begin();
        }
        [[nodiscard("Store this!")]] auto end() const
        {
            return m_Elements.end();
        }

      private:
        void CalculateOffsetsAndStride()
        {
            sizet offset = 0;
            m_Stride = 0;
            for (auto& element : m_Elements)
            {
                element.offset = offset;
                offset += element.size;
                m_Stride += element.size;
            }
        }

      private:
        TArray<BufferElement> m_Elements;
        u32 m_Stride = 0;
    };
} // namespace OloEngine
