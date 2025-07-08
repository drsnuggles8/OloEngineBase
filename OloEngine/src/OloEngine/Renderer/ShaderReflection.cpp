#include "OloEnginePCH.h"
#include "ShaderReflection.h"
#include "OloEngine/Core/Log.h"

#ifdef OLO_ENABLE_SPIRV_CROSS
#include <spirv_cross/spirv_cross.hpp>
#endif

namespace OloEngine
{
    bool ShaderReflection::ReflectFromSPIRV(const std::vector<u32>& spirvBytecode)
    {
#ifdef OLO_ENABLE_SPIRV_CROSS
        try
        {
            spirv_cross::Compiler compiler(spirvBytecode);
            spirv_cross::ShaderResources resources = compiler.get_shader_resources();

            // Clear existing data
            Clear();

            // Process uniform buffers
            for (const auto& resource : resources.uniform_buffers)
            {
                const auto& type = compiler.get_type(resource.type_id);
                const std::string& name = resource.name;
                
                // Get binding point
                u32 binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
                
                // Get buffer size
                u32 bufferSize = static_cast<u32>(compiler.get_declared_struct_size(type));
                
                UniformBlockInfo blockInfo;
                blockInfo.Name = name;
                blockInfo.BindingPoint = binding;
                blockInfo.Size = bufferSize;
                
                // Extract member variables
                for (u32 i = 0; i < type.member_types.size(); ++i)
                {
                    const auto& memberType = compiler.get_type(type.member_types[i]);
                    const std::string& memberName = compiler.get_member_name(resource.type_id, i);
                    
                    ShaderUniformDeclaration variable;
                    variable.Name = memberName;
                    variable.Offset = compiler.get_member_decoration(resource.type_id, i, spv::DecorationOffset);
                    
                    // Convert SPIR-V type to ShaderDataType
                    variable.Type = ConvertSPIRVType(memberType);
                    variable.Size = ShaderUniformDeclaration::ShaderDataTypeSize(variable.Type);
                    variable.ArraySize = memberType.array.empty() ? 1 : memberType.array[0];
                    
                    blockInfo.Variables.push_back(variable);
                }
                
                // Add to collections
                u32 index = static_cast<u32>(m_UniformBlocks.size());
                m_UniformBlocks.push_back(blockInfo);
                m_BlockNameToIndex[name] = index;
                
                OLO_CORE_TRACE("ShaderReflection: Found uniform block '{0}' at binding {1}, size {2} bytes", 
                              name, binding, bufferSize);
            }

            // Process textures/samplers
            for (const auto& resource : resources.sampled_images)
            {
                const auto& type = compiler.get_type(resource.type_id);
                const std::string& name = resource.name;
                
                // Get binding point
                u32 binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
                
                TextureInfo textureInfo;
                textureInfo.Name = name;
                textureInfo.BindingPoint = binding;
                
                // Determine texture type from SPIR-V image dimensions
                if (type.image.dim == spv::Dim2D)
                    textureInfo.Type = ShaderResourceType::Texture2D;
                else if (type.image.dim == spv::DimCube)
                    textureInfo.Type = ShaderResourceType::TextureCube;
                else
                    textureInfo.Type = ShaderResourceType::None;
                
                m_Textures.push_back(textureInfo);
                
                // Also add to generic resources
                ResourceInfo resourceInfo;
                resourceInfo.Name = name;
                resourceInfo.BindingPoint = binding;
                resourceInfo.Type = textureInfo.Type;
                resourceInfo.Size = 0; // Textures don't have a meaningful size in this context
                m_Resources.push_back(resourceInfo);
                
                OLO_CORE_TRACE("ShaderReflection: Found texture '{0}' at binding {1}, type {2}", 
                              name, binding, static_cast<int>(textureInfo.Type));
            }
            
            return true;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("ShaderReflection: Failed to reflect SPIR-V - {0}", e.what());
            return false;
        }
#else
        (void)spirvBytecode; // Suppress unused parameter warning
        OLO_CORE_WARN("ShaderReflection: SPIR-V reflection disabled (OLO_ENABLE_SPIRV_CROSS not defined)");
        return false;
#endif
    }

    const ShaderReflection::UniformBlockInfo* ShaderReflection::GetUniformBlock(const std::string& name) const
    {
        auto it = m_BlockNameToIndex.find(name);
        if (it != m_BlockNameToIndex.end())
        {
            return &m_UniformBlocks[it->second];
        }
        return nullptr;
    }

    u32 ShaderReflection::GetUniformBlockSize(const std::string& blockName) const
    {
        const auto* block = GetUniformBlock(blockName);
        return block ? block->Size : 0;
    }

    void ShaderReflection::Clear()
    {
        m_UniformBlocks.clear();
        m_Textures.clear();
        m_Resources.clear();
        m_BlockNameToIndex.clear();
    }

#ifdef OLO_ENABLE_SPIRV_CROSS
    ShaderDataType ShaderReflection::ConvertSPIRVType(const spirv_cross::SPIRType& type)
    {
        switch (type.basetype)
        {
            case spirv_cross::SPIRType::Float:
                if (type.columns == 1)
                {
                    switch (type.vecsize)
                    {
                        case 1: return ShaderDataType::Float;
                        case 2: return ShaderDataType::Float2;
                        case 3: return ShaderDataType::Float3;
                        case 4: return ShaderDataType::Float4;
                    }
                }
                else
                {
                    if (type.columns == 3 && type.vecsize == 3)
                        return ShaderDataType::Mat3;
                    if (type.columns == 4 && type.vecsize == 4)
                        return ShaderDataType::Mat4;
                }
                break;
                
            case spirv_cross::SPIRType::Int:
                switch (type.vecsize)
                {
                    case 1: return ShaderDataType::Int;
                    case 2: return ShaderDataType::Int2;
                    case 3: return ShaderDataType::Int3;
                    case 4: return ShaderDataType::Int4;
                }
                break;
                
            case spirv_cross::SPIRType::Boolean:
                return ShaderDataType::Bool;
                
            case spirv_cross::SPIRType::SampledImage:
                // Determine sampler type based on image dimensions
                if (type.image.dim == spv::Dim2D)
                    return ShaderDataType::Sampler2D;
                else if (type.image.dim == spv::DimCube)
                    return ShaderDataType::SamplerCube;
                break;
        }
        
        return ShaderDataType::None;
    }
#else
    ShaderDataType ShaderReflection::ConvertSPIRVType(const int type)
    {
        (void)type; // Suppress unused parameter warning
        // Fallback when SPIR-V Cross is not available
        return ShaderDataType::None;
    }
#endif
}
