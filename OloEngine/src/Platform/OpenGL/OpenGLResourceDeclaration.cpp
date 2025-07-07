#include "Platform/OpenGL/OpenGLResourceDeclaration.h"
#include "OloEngine/Core/Log.h"

#include <spirv_cross/spirv_cross.hpp>
#include <spirv_cross/spirv_glsl.hpp>
#include <algorithm>
#include <numeric>
#include <sstream>

namespace OloEngine
{
    OpenGLResourceDeclaration::OpenGLResourceDeclaration(const std::string& passName)
        : m_Declaration(passName)
    {
        // Initialize type binding counters
        m_TypeBindingCounters[ShaderResourceType::UniformBuffer] = 0;
        m_TypeBindingCounters[ShaderResourceType::StorageBuffer] = 1000;  // Offset storage buffers
        m_TypeBindingCounters[ShaderResourceType::Texture2D] = 0;
        m_TypeBindingCounters[ShaderResourceType::TextureCube] = 100;     // Offset cube textures
        m_TypeBindingCounters[ShaderResourceType::Image2D] = 2000;        // Offset images
    }

    u32 OpenGLResourceDeclaration::AddResource(const ResourceInfo& resourceInfo)
    {
        // Check for duplicate names
        if (HasResource(resourceInfo.Name))
        {
            OLO_CORE_WARN("Resource '{}' already exists, updating instead", resourceInfo.Name);
            if (UpdateResource(resourceInfo.Name, resourceInfo))
            {
                return m_Declaration.NameToIndex[resourceInfo.Name];
            }
            return UINT32_MAX;
        }

        ResourceInfo newResource = resourceInfo;

        // Auto-assign binding if not set
        if (newResource.Binding == UINT32_MAX && m_AutoAssignBindings)
        {
            if (m_TypeBindingCounters.find(newResource.Type) != m_TypeBindingCounters.end())
            {
                newResource.Binding = m_TypeBindingCounters[newResource.Type]++;
            }
            else
            {
                newResource.Binding = m_NextAutoBinding++;
            }
        }

        // Estimate memory usage if not provided
        if (newResource.EstimatedMemoryUsage == 0)
        {
            newResource.EstimatedMemoryUsage = EstimateMemoryUsage(newResource);
        }

        // Add to resources
        u32 index = static_cast<u32>(m_Declaration.Resources.size());
        m_Declaration.Resources.push_back(newResource);

        // Update indices and statistics
        UpdateIndices();
        UpdateStatistics();

        OLO_CORE_TRACE("Added resource '{}' at binding {} (index {})", 
                      newResource.Name, newResource.Binding, index);

        return index;
    }

    bool OpenGLResourceDeclaration::RemoveResource(const std::string& name)
    {
        auto it = m_Declaration.NameToIndex.find(name);
        if (it == m_Declaration.NameToIndex.end())
        {
            return false;
        }

        u32 index = it->second;
        if (index >= m_Declaration.Resources.size())
        {
            return false;
        }

        // Remove from resources vector
        m_Declaration.Resources.erase(m_Declaration.Resources.begin() + index);

        // Update indices and statistics
        UpdateIndices();
        UpdateStatistics();

        OLO_CORE_TRACE("Removed resource '{}'", name);
        return true;
    }

    const OpenGLResourceDeclaration::ResourceInfo* OpenGLResourceDeclaration::GetResource(const std::string& name) const
    {
        auto it = m_Declaration.NameToIndex.find(name);
        if (it == m_Declaration.NameToIndex.end())
        {
            return nullptr;
        }

        u32 index = it->second;
        if (index >= m_Declaration.Resources.size())
        {
            return nullptr;
        }

        return &m_Declaration.Resources[index];
    }

    const OpenGLResourceDeclaration::ResourceInfo* OpenGLResourceDeclaration::GetResource(u32 index) const
    {
        if (index >= m_Declaration.Resources.size())
        {
            return nullptr;
        }

        return &m_Declaration.Resources[index];
    }

    bool OpenGLResourceDeclaration::UpdateResource(const std::string& name, const ResourceInfo& resourceInfo)
    {
        auto it = m_Declaration.NameToIndex.find(name);
        if (it == m_Declaration.NameToIndex.end())
        {
            return false;
        }

        u32 index = it->second;
        if (index >= m_Declaration.Resources.size())
        {
            return false;
        }

        // Update resource but preserve the name mapping
        ResourceInfo updatedResource = resourceInfo;
        updatedResource.Name = name;  // Ensure name consistency

        m_Declaration.Resources[index] = updatedResource;

        UpdateStatistics();
        return true;
    }

    bool OpenGLResourceDeclaration::PopulateFromSPIRV(u32 stage, const std::vector<u32>& spirvData)
    {
        try
        {
            spirv_cross::Compiler compiler(spirvData);
            ExtractFromSPIRVCompiler(compiler, stage);
            return true;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("Failed to process SPIR-V data: {}", e.what());
            return false;
        }
    }

    void OpenGLResourceDeclaration::ExtractFromSPIRVCompiler(const spirv_cross::Compiler& compiler, u32 stage)
    {
        spirv_cross::ShaderResources resources = compiler.get_shader_resources();

        // Process different resource types
        ProcessUniformBuffers(compiler, resources);
        ProcessStorageBuffers(compiler, resources);
        ProcessTextures(compiler, resources);
        ProcessImages(compiler, resources);
        ProcessPushConstants(compiler, resources);

        // Update indices and statistics
        UpdateIndices();
        UpdateStatistics();

        OLO_CORE_INFO("Extracted {} resources from SPIR-V for stage {}", 
                     GetResourceCount(), stage);
    }

    void OpenGLResourceDeclaration::ProcessUniformBuffers(const spirv_cross::Compiler& compiler, 
                                                         const spirv_cross::ShaderResources& resources)
    {
        for (const auto& ubo : resources.uniform_buffers)
        {
            ResourceInfo resourceInfo = CreateResourceFromSPIRV(compiler, ubo, ShaderResourceType::UniformBuffer);
            
            // Get buffer size
            const auto& type = compiler.get_type(ubo.type_id);
            resourceInfo.Size = static_cast<u32>(compiler.get_declared_struct_size(type));
            
            AddResource(resourceInfo);
        }
    }

    void OpenGLResourceDeclaration::ProcessStorageBuffers(const spirv_cross::Compiler& compiler, 
                                                          const spirv_cross::ShaderResources& resources)
    {
        for (const auto& ssbo : resources.storage_buffers)
        {
            ResourceInfo resourceInfo = CreateResourceFromSPIRV(compiler, ssbo, ShaderResourceType::StorageBuffer);
            
            // Get buffer size if possible
            const auto& type = compiler.get_type(ssbo.type_id);
            if (type.basetype == spirv_cross::SPIRType::Struct)
            {
                resourceInfo.Size = static_cast<u32>(compiler.get_declared_struct_size(type));
            }
            
            AddResource(resourceInfo);
        }
    }

    void OpenGLResourceDeclaration::ProcessTextures(const spirv_cross::Compiler& compiler, 
                                                    const spirv_cross::ShaderResources& resources)
    {
        for (const auto& image : resources.sampled_images)
        {
            const auto& type = compiler.get_type(image.type_id);
            ShaderResourceType resourceType = ShaderResourceType::Texture2D;
            
            // Determine texture type from SPIR-V dimension
            switch (type.image.dim)
            {
                case spv::Dim1D:
                    resourceType = ShaderResourceType::Texture2D; // Map 1D to 2D for compatibility
                    break;
                case spv::Dim2D:
                    resourceType = ShaderResourceType::Texture2D;
                    break;
                case spv::Dim3D:
                    resourceType = ShaderResourceType::Texture2D; // Map 3D to 2D for compatibility
                    break;
                case spv::DimCube:
                    resourceType = ShaderResourceType::TextureCube;
                    break;
                default:
                    resourceType = ShaderResourceType::Texture2D;
                    break;
            }
            
            ResourceInfo resourceInfo = CreateResourceFromSPIRV(compiler, image, resourceType);
            
            // Set OpenGL-specific texture information
            resourceInfo.GLTarget = ResourceTypeToGLTarget(resourceType);
            resourceInfo.GLType = SPIRVToGLType(type.image.type, compiler);
            
            AddResource(resourceInfo);
        }
    }

    void OpenGLResourceDeclaration::ProcessImages(const spirv_cross::Compiler& compiler, 
                                                 const spirv_cross::ShaderResources& resources)
    {
        for (const auto& image : resources.storage_images)
        {
            ResourceInfo resourceInfo = CreateResourceFromSPIRV(compiler, image, ShaderResourceType::Image2D);
            
            const auto& type = compiler.get_type(image.type_id);
            resourceInfo.GLTarget = GL_TEXTURE_2D;  // Most common case
            resourceInfo.GLType = SPIRVToGLType(type.image.type, compiler);
            
            AddResource(resourceInfo);
        }
    }

    void OpenGLResourceDeclaration::ProcessPushConstants(const spirv_cross::Compiler& compiler, 
                                                         const spirv_cross::ShaderResources& resources)
    {
        // OpenGL doesn't typically use push constants (this is a Vulkan concept)
        // For now, we'll skip processing them. If needed in the future, they can be
        // mapped to uniform buffers or added as a new resource type.
        
        // Note: Push constants are typically small amounts of data that are pushed
        // directly to the shader pipeline. In OpenGL, this is usually handled through
        // uniform variables instead.
        
        if (!resources.push_constant_buffers.empty())
        {
            OLO_CORE_WARN("SPIR-V contains push constants which are not supported in OpenGL. Consider using uniform buffers instead.");
        }
    }

    OpenGLResourceDeclaration::ResourceInfo OpenGLResourceDeclaration::CreateResourceFromSPIRV(
        const spirv_cross::Compiler& compiler, 
        const spirv_cross::Resource& resource, 
        ShaderResourceType type) const
    {
        ResourceInfo resourceInfo;
        resourceInfo.Name = resource.name;
        resourceInfo.Type = type;
        resourceInfo.SPIRVTypeID = resource.type_id;
        resourceInfo.SPIRVBaseTypeID = resource.base_type_id;

        // Get binding and set information
        if (compiler.has_decoration(resource.id, spv::DecorationBinding))
        {
            resourceInfo.Binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
        }

        if (compiler.has_decoration(resource.id, spv::DecorationDescriptorSet))
        {
            resourceInfo.Set = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
        }

        if (compiler.has_decoration(resource.id, spv::DecorationLocation))
        {
            resourceInfo.Location = compiler.get_decoration(resource.id, spv::DecorationLocation);
        }

        // Check if it's an array
        const auto& spirvType = compiler.get_type(resource.type_id);
        if (!spirvType.array.empty())
        {
            resourceInfo.IsArray = true;
            resourceInfo.ArraySize = spirvType.array[0];
        }

        return resourceInfo;
    }

    bool OpenGLResourceDeclaration::Validate()
    {
        m_Declaration.ValidationErrors.clear();
        bool isValid = true;

        // Check for binding conflicts
        std::unordered_map<u32, std::string> bindingToResource;
        for (const auto& resource : m_Declaration.Resources)
        {
            if (resource.Binding != UINT32_MAX)
            {
                auto it = bindingToResource.find(resource.Binding);
                if (it != bindingToResource.end())
                {
                    std::string error = "Binding conflict: resources '" + resource.Name + 
                                      "' and '" + it->second + "' both use binding " + std::to_string(resource.Binding);
                    m_Declaration.ValidationErrors.push_back(error);
                    isValid = false;
                }
                else
                {
                    bindingToResource[resource.Binding] = resource.Name;
                }
            }
        }

        // Validate individual resources
        for (u32 i = 0; i < m_Declaration.Resources.size(); ++i)
        {
            if (!ValidateResource(m_Declaration.Resources[i], i))
            {
                isValid = false;
            }
        }

        m_Declaration.IsValid = isValid;
        return isValid;
    }

    std::vector<std::string> OpenGLResourceDeclaration::FindBindingConflicts() const
    {
        std::vector<std::string> conflicts;
        std::unordered_map<u32, std::string> bindingToResource;

        for (const auto& resource : m_Declaration.Resources)
        {
            if (resource.Binding != UINT32_MAX)
            {
                auto it = bindingToResource.find(resource.Binding);
                if (it != bindingToResource.end())
                {
                    conflicts.push_back(resource.Name);
                    conflicts.push_back(it->second);
                }
                else
                {
                    bindingToResource[resource.Binding] = resource.Name;
                }
            }
        }

        return conflicts;
    }

    bool OpenGLResourceDeclaration::OptimizeBindingLayout(bool enableAutomaticReordering)
    {
        if (!enableAutomaticReordering)
        {
            return Validate();
        }

        // Sort resources by priority and usage frequency for optimal binding assignment
        std::vector<u32> resourceIndices(m_Declaration.Resources.size());
        std::iota(resourceIndices.begin(), resourceIndices.end(), 0);

        std::sort(resourceIndices.begin(), resourceIndices.end(), 
                 [this](u32 a, u32 b) 
                 {
                     const auto& resA = m_Declaration.Resources[a];
                     const auto& resB = m_Declaration.Resources[b];
                     
                     // Sort by frequency first, then priority
                     if (resA.Frequency != resB.Frequency)
                     {
                         return static_cast<int>(resA.Frequency) > static_cast<int>(resB.Frequency);
                     }
                     return resA.Priority > resB.Priority;
                 });

        // Reassign bindings based on optimized order
        m_TypeBindingCounters.clear();
        m_TypeBindingCounters[ShaderResourceType::UniformBuffer] = 0;
        m_TypeBindingCounters[ShaderResourceType::StorageBuffer] = 1000;
        m_TypeBindingCounters[ShaderResourceType::Texture2D] = 0;
        m_TypeBindingCounters[ShaderResourceType::TextureCube] = 100;
        m_TypeBindingCounters[ShaderResourceType::Image2D] = 2000;

        for (u32 index : resourceIndices)
        {
            auto& resource = m_Declaration.Resources[index];
            if (m_TypeBindingCounters.find(resource.Type) != m_TypeBindingCounters.end())
            {
                resource.Binding = m_TypeBindingCounters[resource.Type]++;
            }
        }

        UpdateIndices();
        return Validate();
    }

    std::vector<u32> OpenGLResourceDeclaration::GetResourcesByType(ShaderResourceType type) const
    {
        std::vector<u32> indices;
        for (u32 i = 0; i < m_Declaration.Resources.size(); ++i)
        {
            if (m_Declaration.Resources[i].Type == type)
            {
                indices.push_back(i);
            }
        }
        return indices;
    }

    std::vector<u32> OpenGLResourceDeclaration::GetResourcesBySet(u32 set) const
    {
        auto it = m_Declaration.SetToResources.find(set);
        if (it != m_Declaration.SetToResources.end())
        {
            return it->second;
        }
        return {};
    }

    bool OpenGLResourceDeclaration::HasResource(const std::string& name) const
    {
        return m_Declaration.NameToIndex.find(name) != m_Declaration.NameToIndex.end();
    }

    std::string OpenGLResourceDeclaration::GenerateUsageReport() const
    {
        std::stringstream ss;
        
        ss << "=== OpenGL Resource Declaration Report ===\n";
        ss << "Pass: " << m_Declaration.PassName << "\n";
        ss << "Total Resources: " << GetResourceCount() << "\n";
        ss << "Uniform Buffers: " << m_Declaration.TotalUniformBuffers << "\n";
        ss << "Storage Buffers: " << m_Declaration.TotalStorageBuffers << "\n";
        ss << "Textures: " << m_Declaration.TotalTextures << "\n";
        ss << "Images: " << m_Declaration.TotalImages << "\n";
        ss << "Total Memory Usage: " << m_Declaration.TotalMemoryUsage << " bytes\n";
        ss << "Valid: " << (m_Declaration.IsValid ? "Yes" : "No") << "\n";
        
        if (!m_Declaration.ValidationErrors.empty())
        {
            ss << "\nValidation Errors:\n";
            for (const auto& error : m_Declaration.ValidationErrors)
            {
                ss << "  - " << error << "\n";
            }
        }
        
        ss << "\nResources by Set:\n";
        for (const auto& [set, resources] : m_Declaration.SetToResources)
        {
            ss << "  Set " << set << ": " << resources.size() << " resources\n";
        }
        
        return ss.str();
    }

    GLenum OpenGLResourceDeclaration::SPIRVToGLType(u32 spirvType, const spirv_cross::Compiler& compiler)
    {
        const auto& type = compiler.get_type(spirvType);
        
        switch (type.basetype)
        {
            case spirv_cross::SPIRType::Float:
                return GL_FLOAT;
            case spirv_cross::SPIRType::Int:
                return GL_INT;
            case spirv_cross::SPIRType::UInt:
                return GL_UNSIGNED_INT;
            case spirv_cross::SPIRType::Boolean:
                return GL_BOOL;
            default:
                return GL_FLOAT;
        }
    }

    GLenum OpenGLResourceDeclaration::ResourceTypeToGLTarget(ShaderResourceType resourceType)
    {
        switch (resourceType)
        {
            case ShaderResourceType::Texture2D:
                return GL_TEXTURE_2D;
            case ShaderResourceType::TextureCube:
                return GL_TEXTURE_CUBE_MAP;
            case ShaderResourceType::Image2D:
                return GL_TEXTURE_2D;
            default:
                return GL_TEXTURE_2D;
        }
    }

    u32 OpenGLResourceDeclaration::EstimateMemoryUsage(const ResourceInfo& resourceInfo)
    {
        switch (resourceInfo.Type)
        {
            case ShaderResourceType::UniformBuffer:
            case ShaderResourceType::StorageBuffer:
                return resourceInfo.Size * resourceInfo.ArraySize;
                
            case ShaderResourceType::Texture2D:
            case ShaderResourceType::Image2D:
                // Rough estimate for common texture sizes
                return 1024 * 1024 * 4 * resourceInfo.ArraySize; // 1MB RGBA texture
                
            default:
                return 1024; // Default small estimate
        }
    }

    void OpenGLResourceDeclaration::UpdateIndices()
    {
        m_Declaration.NameToIndex.clear();
        m_Declaration.SetToResources.clear();

        for (u32 i = 0; i < m_Declaration.Resources.size(); ++i)
        {
            const auto& resource = m_Declaration.Resources[i];
            m_Declaration.NameToIndex[resource.Name] = i;
            m_Declaration.SetToResources[resource.Set].push_back(i);
        }
    }

    void OpenGLResourceDeclaration::UpdateStatistics()
    {
        m_Declaration.TotalUniformBuffers = 0;
        m_Declaration.TotalStorageBuffers = 0;
        m_Declaration.TotalTextures = 0;
        m_Declaration.TotalImages = 0;
        m_Declaration.TotalMemoryUsage = 0;

        for (const auto& resource : m_Declaration.Resources)
        {
            switch (resource.Type)
            {
                case ShaderResourceType::UniformBuffer:
                    m_Declaration.TotalUniformBuffers++;
                    break;
                case ShaderResourceType::StorageBuffer:
                    m_Declaration.TotalStorageBuffers++;
                    break;
                case ShaderResourceType::Texture2D:
                case ShaderResourceType::TextureCube:
                    m_Declaration.TotalTextures++;
                    break;
                case ShaderResourceType::Image2D:
                    m_Declaration.TotalImages++;
                    break;
            }
            
            m_Declaration.TotalMemoryUsage += resource.EstimatedMemoryUsage;
        }
    }

    bool OpenGLResourceDeclaration::ValidateResource(const ResourceInfo& resource, u32 index) const
    {
        // Basic validation
        if (resource.Name.empty())
        {
            return false;
        }
        
        if (resource.Type == ShaderResourceType::None)
        {
            return false;
        }
        
        // Type-specific validation
        switch (resource.Type)
        {
            case ShaderResourceType::UniformBuffer:
            case ShaderResourceType::StorageBuffer:
                return resource.Size > 0;
                
            default:
                return true;
        }
    }
}
