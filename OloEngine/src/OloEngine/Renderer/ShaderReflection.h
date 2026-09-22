#pragma once

#include "OloEngine/Core/Base.h"
#include "ShaderDataTypes.h"
#include "ShaderResourceTypes.h"
#include "OloEngine/Containers/Array.h"
#include <span>
#include <unordered_map>
#include <string>

#include <spirv_cross/spirv_cross.hpp>

namespace OloEngine
{
    // @brief SPIR-V reflection system for extracting all shader resource information
    class ShaderReflection
    {
      public:
        // @brief Information about a uniform block discovered in shader
        struct UniformBlockInfo
        {
            FString Name;
            u32 BindingPoint;
            u32 Size;
            TArray<ShaderUniformDeclaration> Variables;
        };

        // @brief Information about a texture/sampler resource discovered in shader
        struct TextureInfo
        {
            FString Name;
            u32 BindingPoint;
            ShaderResourceType Type; // Texture2D, TextureCube, etc.
        };

        // @brief Generic resource information (for future expansion)
        struct ResourceInfo
        {
            FString Name;
            u32 BindingPoint;
            ShaderResourceType Type;
            u32 Size = 0; // for buffers
        };

        // @brief Reflect all shader resources from SPIR-V bytecode
        bool ReflectFromSPIRV(std::span<const u32> spirvBytecode);

        // @brief Get all discovered uniform blocks
        const TArray<UniformBlockInfo>& GetUniformBlocks() const
        {
            return m_UniformBlocks;
        }

        // @brief Get all discovered textures
        const TArray<TextureInfo>& GetTextures() const
        {
            return m_Textures;
        }

        // @brief Get all discovered resources (generic)
        const TArray<ResourceInfo>& GetResources() const
        {
            return m_Resources;
        }

        // @brief Get uniform block by name
        const UniformBlockInfo* GetUniformBlock(const std::string& name) const;

        // @brief Get uniform block size by name
        u32 GetUniformBlockSize(const std::string& blockName) const;

        // @brief Clear all reflection data
        void Clear();

      private:
        TArray<UniformBlockInfo> m_UniformBlocks;
        TArray<TextureInfo> m_Textures;
        TArray<ResourceInfo> m_Resources;
        std::unordered_map<std::string, u32> m_BlockNameToIndex;

        // @brief Parse SPIR-V and extract all resource information
        void ParseSPIRVUniforms(std::span<const u32> spirvBytecode);

        // @brief Parse SPIR-V and extract texture/sampler information
        void ParseSPIRVTextures(std::span<const u32> spirvBytecode);

        // @brief Convert SPIR-V type to ShaderDataType
        ShaderDataType ConvertSPIRVType(const spirv_cross::SPIRType& type) const;
    };

    template<>
    struct TIsTriviallyRelocatable<ShaderReflection::UniformBlockInfo>
    {
        using Info = ShaderReflection::UniformBlockInfo;
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(Info::Name)> &&
                                      TIsTriviallyRelocatable_V<decltype(Info::BindingPoint)> &&
                                      TIsTriviallyRelocatable_V<decltype(Info::Size)> &&
                                      TIsTriviallyRelocatable_V<decltype(Info::Variables)>;
    };

    template<>
    struct TIsTriviallyRelocatable<ShaderReflection::TextureInfo>
    {
        using Info = ShaderReflection::TextureInfo;
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(Info::Name)> &&
                                      TIsTriviallyRelocatable_V<decltype(Info::BindingPoint)> &&
                                      TIsTriviallyRelocatable_V<decltype(Info::Type)>;
    };

    template<>
    struct TIsTriviallyRelocatable<ShaderReflection::ResourceInfo>
    {
        using Info = ShaderReflection::ResourceInfo;
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(Info::Name)> &&
                                      TIsTriviallyRelocatable_V<decltype(Info::BindingPoint)> &&
                                      TIsTriviallyRelocatable_V<decltype(Info::Type)> &&
                                      TIsTriviallyRelocatable_V<decltype(Info::Size)>;
    };
} // namespace OloEngine
