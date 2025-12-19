#pragma once

#include "OloEngine/Core/Base.h"
#include "ShaderDataTypes.h"
#include "ShaderResourceTypes.h"
#include <vector>
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
            std::string Name;
            u32 BindingPoint;
            u32 Size;
            std::vector<ShaderUniformDeclaration> Variables;
        };

        // @brief Information about a texture/sampler resource discovered in shader
        struct TextureInfo
        {
            std::string Name;
            u32 BindingPoint;
            ShaderResourceType Type; // Texture2D, TextureCube, etc.
        };

        // @brief Generic resource information (for future expansion)
        struct ResourceInfo
        {
            std::string Name;
            u32 BindingPoint;
            ShaderResourceType Type;
            u32 Size = 0; // for buffers
        };

        // @brief Reflect all shader resources from SPIR-V bytecode
        bool ReflectFromSPIRV(const std::vector<u32>& spirvBytecode);

        // @brief Get all discovered uniform blocks
        const std::vector<UniformBlockInfo>& GetUniformBlocks() const { return m_UniformBlocks; }

        // @brief Get all discovered textures
        const std::vector<TextureInfo>& GetTextures() const { return m_Textures; }

        // @brief Get all discovered resources (generic)
        const std::vector<ResourceInfo>& GetResources() const { return m_Resources; }

        // @brief Get uniform block by name
        const UniformBlockInfo* GetUniformBlock(const std::string& name) const;

        // @brief Get uniform block size by name
        u32 GetUniformBlockSize(const std::string& blockName) const;

        // @brief Clear all reflection data
        void Clear();

    private:
        std::vector<UniformBlockInfo> m_UniformBlocks;
        std::vector<TextureInfo> m_Textures;
        std::vector<ResourceInfo> m_Resources;
        std::unordered_map<std::string, u32> m_BlockNameToIndex;

        // @brief Parse SPIR-V and extract all resource information
        void ParseSPIRVUniforms(const std::vector<u32>& spirvBytecode);
        
        // @brief Parse SPIR-V and extract texture/sampler information
        void ParseSPIRVTextures(const std::vector<u32>& spirvBytecode);
        
        // @brief Convert SPIR-V type to ShaderDataType
        ShaderDataType ConvertSPIRVType(const spirv_cross::SPIRType& type);
    };
}
