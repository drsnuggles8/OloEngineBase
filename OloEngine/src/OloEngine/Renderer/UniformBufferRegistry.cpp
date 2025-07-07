#include "OloEnginePCH.h"
#include "UniformBufferRegistry.h"
#include "ResourceHandleCache.h"
#include "EnhancedResourceGetter.h"
#include "BindingStateCache.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Debug/ShaderDebugger.h"
#include "Platform/OpenGL/OpenGLDescriptorSetManager.h"
#include "Platform/OpenGL/OpenGLMultiBind.h"
#include "Platform/OpenGL/OpenGLDSABindingManager.h"

#include <spirv_cross/spirv_cross.hpp>
#include <glad/gl.h>
#include <imgui.h>
#include <sstream>

namespace OloEngine {

    UniformBufferRegistry::UniformBufferRegistry(const Ref<Shader>& shader, const UniformBufferRegistrySpecification& spec)
        : m_Shader(shader), m_Specification(spec), m_IsTemplate(false), m_IsClone(false),
          m_UseSetPriority(spec.UseSetPriority), m_AutoAssignSets(spec.AutoAssignSets),
          m_StartSet(spec.StartSet), m_EndSet(spec.EndSet),
          m_DefaultResourcesInitialized(false), m_FrameBasedBatchingEnabled(spec.EnableBatching),
          m_MaxFrameDelay(spec.FramesInFlight), m_ValidationSeverityFilter(RegistryValidationSeverity::Warning),
          m_RealtimeValidationEnabled(spec.EnableValidation),
          m_HandleCachingEnabled(spec.EnableCaching), m_Initialized(false)
    {
        // Initialize handle cache
        m_HandleCache = new ResourceHandleCache();
        m_HandleCache->SetCachingEnabled(m_HandleCachingEnabled);

        // Initialize OpenGL descriptor set manager and multi-bind
        m_DescriptorSetManager = std::make_unique<OpenGLDescriptorSetManager>();
        m_MultiBind = std::make_unique<OpenGLMultiBind>();
        m_DescriptorSetManager->SetMultiBindManager(m_MultiBind.get());

        OLO_CORE_TRACE("UniformBufferRegistry created with specification '{0}' for shader '{1}'", 
                      spec.Name, shader ? shader->GetName() : "None");
    }

    UniformBufferRegistry::~UniformBufferRegistry()
    {
        if (m_HandleCache)
        {
            delete m_HandleCache;
            m_HandleCache = nullptr;
        }
    }

    void UniformBufferRegistry::Shutdown()
    {
        if (!m_Initialized)
        {
            return;
        }

        // Clear all resources
        ClearResources();
        
        // Clear pending updates
        m_PendingResources.clear();
        m_InvalidatedResources.clear();
        
        // Reset OpenGL managers
        if (m_DescriptorSetManager)
        {
            m_DescriptorSetManager.reset();
        }
        
        if (m_MultiBind)
        {
            m_MultiBind.reset();
        }
        
        // Clear handle cache
        if (m_HandleCache)
        {
            m_HandleCache->CleanupCache(0); // Clean all cached handles
        }
        
        // Reset frame-in-flight manager
        if (m_FrameInFlightManager)
        {
            m_FrameInFlightManager.reset();
            m_FrameInFlightEnabled = false;
        }
        
        // Clear OpenGL declarations
        m_OpenGLDeclarations.clear();
        
        // Reset state
        m_ResourceBindings.clear();
        m_BindingPointUsage.clear();
        m_DescriptorSets.clear();
        m_PriorityToSetMap.clear();
        m_SetBindingOrder.clear();
        m_DefaultResources.clear();
        m_ResourceTemplates.clear();
        m_ResourceTypePriorities.clear();
        m_ResourceDeclarations.clear();
        
        m_Initialized = false;

        OLO_CORE_TRACE("UniformBufferRegistry shutdown complete for specification '{0}'", m_Specification.Name);
    }

    UniformBufferRegistry::UniformBufferRegistry(const UniformBufferRegistry& other)
        : m_Shader(nullptr), // Shader will be set separately for clones
          m_Specification(other.m_Specification),
          m_IsTemplate(false), // Copies are not templates
          m_IsClone(true),     // Mark as clone
          m_TemplateName(other.m_IsTemplate ? other.m_TemplateName : ""),
          m_SourceTemplateName(other.m_IsTemplate ? other.m_TemplateName : other.m_SourceTemplateName),
          m_UseSetPriority(other.m_UseSetPriority),
          m_AutoAssignSets(other.m_AutoAssignSets),
          m_StartSet(other.m_StartSet),
          m_EndSet(other.m_EndSet),
          m_DefaultResourcesInitialized(false), // Will be reinitialized
          m_FrameBasedBatchingEnabled(other.m_FrameBasedBatchingEnabled),
          m_MaxFrameDelay(other.m_MaxFrameDelay),
          m_ValidationSeverityFilter(other.m_ValidationSeverityFilter),
          m_RealtimeValidationEnabled(other.m_RealtimeValidationEnabled),
          m_HandleCachingEnabled(other.m_HandleCachingEnabled),
          m_Initialized(false) // Will be initialized separately
    {
        // Initialize handle cache
        m_HandleCache = new ResourceHandleCache();
        m_HandleCache->SetCachingEnabled(m_HandleCachingEnabled);

        // Initialize OpenGL descriptor set manager and multi-bind
        m_DescriptorSetManager = std::make_unique<OpenGLDescriptorSetManager>();
        m_MultiBind = std::make_unique<OpenGLMultiBind>();
        m_DescriptorSetManager->SetMultiBindManager(m_MultiBind.get());

        // Copy resource bindings (but not bound resources - they'll be set separately)
        m_ResourceBindings = other.m_ResourceBindings;
        
        // Copy descriptor sets configuration
        m_DescriptorSets = other.m_DescriptorSets;
        m_PriorityToSetMap = other.m_PriorityToSetMap;
        m_SetBindingOrder = other.m_SetBindingOrder;
        
        // Copy default resources configuration
        m_DefaultResources = other.m_DefaultResources;
        m_ResourceTemplates = other.m_ResourceTemplates;
        
        // Copy resource type priorities
        m_ResourceTypePriorities = other.m_ResourceTypePriorities;
        
        // Copy resource declarations
        m_ResourceDeclarations = other.m_ResourceDeclarations;
        
        // Reset statistics and state
        m_UpdateStats.Reset();
        m_CurrentFrame = 0;
        
        OLO_CORE_TRACE("UniformBufferRegistry copied from {0} (source template: {1})", 
                      other.m_IsTemplate ? "template" : "registry", m_SourceTemplateName);
    }

    void UniformBufferRegistry::Initialize()
    {
        if (m_Initialized)
        {
            OLO_CORE_WARN("UniformBufferRegistry already initialized");
            return;
        }

        // Phase 3.1: Auto-assign resources to descriptor sets if enabled
        if (m_AutoAssignSets && !m_ResourceBindings.empty())
        {
            AutoAssignResourceSets(true);
        }

        // Phase 4: Initialize default update priorities
        m_ResourceTypePriorities[ShaderResourceType::UniformBuffer] = UpdatePriority::High;
        m_ResourceTypePriorities[ShaderResourceType::Texture2D] = UpdatePriority::Normal;
        m_ResourceTypePriorities[ShaderResourceType::TextureCube] = UpdatePriority::Normal;
        m_ResourceTypePriorities[ShaderResourceType::StorageBuffer] = UpdatePriority::Low;
        m_ResourceTypePriorities[ShaderResourceType::Image2D] = UpdatePriority::Low;

        m_Initialized = true;

        // Clear debug resource bindings for this shader (only if shader is available)
        if (m_Shader && m_Specification.EnableDebugInterface)
        {
            ShaderDebugger::GetInstance().ClearResourceBindings(m_Shader->GetRendererID());
        }

        OLO_CORE_TRACE("UniformBufferRegistry initialized for shader: {0} (spec: {1})", 
                      m_Shader ? m_Shader->GetName() : "Template", m_Specification.Name);
    }

    bool UniformBufferRegistry::SynchronizeWithDeclaration(const OpenGLResourceDeclaration& declaration)
    {
        if (!m_Initialized)
        {
            OLO_CORE_ERROR("UniformBufferRegistry not initialized. Call Initialize() first.");
            return false;
        }

        bool success = true;
        const auto& declaredResources = declaration.GetDeclaration().Resources;

        // Check for new resources in the declaration
        for (const auto& declResource : declaredResources)
        {
            if (m_ResourceBindings.find(declResource.Name) == m_ResourceBindings.end())
            {
                // Create new binding from declaration
                ShaderResourceBinding binding(
                    declResource.Type,
                    declResource.Binding,
                    declResource.Set,
                    declResource.Name,
                    declResource.Size
                );

                if (declResource.IsArray)
                {
                    binding.IsArray = true;
                    binding.ArraySize = declResource.ArraySize;
                    binding.BaseBindingPoint = declResource.Binding;
                }

                m_ResourceBindings[declResource.Name] = binding;
                m_BindingPointUsage[declResource.Binding] = declResource.Name;

                OLO_CORE_TRACE("Added new resource from declaration: {0} (binding={1})", 
                              declResource.Name, declResource.Binding);
            }
        }

        // Validate existing bindings against declaration
        for (const auto& [name, binding] : m_ResourceBindings)
        {
            auto declIt = std::find_if(declaredResources.begin(), declaredResources.end(),
                [&name](const OpenGLResourceDeclaration::ResourceInfo& res) {
                    return res.Name == name;
                });

            if (declIt == declaredResources.end())
            {
                OLO_CORE_WARN("Resource '{0}' exists in registry but not in declaration", name);
                success = false;
            }
            else
            {
                // Check compatibility
                if (binding.Type != declIt->Type || binding.BindingPoint != declIt->Binding)
                {
                    OLO_CORE_ERROR("Resource mismatch detected for: {}", name);
                    success = false;
                }
            }
        }

        return success;
    }

    OpenGLResourceDeclaration UniformBufferRegistry::ExportToDeclaration(const std::string& passName) const
    {
        std::string actualPassName = passName.empty() ? m_DefaultPassName : passName;
        OpenGLResourceDeclaration declaration(actualPassName);

        // Export all registered resources to declaration format
        for (const auto& [name, resourceBinding] : m_ResourceBindings)
        {
            OpenGLResourceDeclaration::ResourceInfo declResource;
            declResource.Name = name;
            declResource.Type = resourceBinding.Type;
            declResource.Set = resourceBinding.Set;
            declResource.Binding = resourceBinding.BindingPoint;
            declResource.ArraySize = resourceBinding.ArraySize;
            declResource.IsArray = resourceBinding.IsArray;
            declResource.Size = static_cast<u32>(resourceBinding.Size);

            // Set access pattern based on resource type
            switch (resourceBinding.Type)
            {
                case ShaderResourceType::UniformBuffer:
                    declResource.Access = OpenGLResourceDeclaration::AccessPattern::ReadOnly;
                    declResource.Frequency = OpenGLResourceDeclaration::UsageFrequency::Normal;
                    break;
                case ShaderResourceType::StorageBuffer:
                    declResource.Access = OpenGLResourceDeclaration::AccessPattern::ReadWrite;
                    declResource.Frequency = OpenGLResourceDeclaration::UsageFrequency::Frequent;
                    break;
                case ShaderResourceType::Texture2D:
                case ShaderResourceType::TextureCube:
                    declResource.Access = OpenGLResourceDeclaration::AccessPattern::ReadOnly;
                    declResource.Frequency = OpenGLResourceDeclaration::UsageFrequency::Normal;
                    break;
                default:
                    declResource.Access = OpenGLResourceDeclaration::AccessPattern::ReadOnly;
                    declResource.Frequency = OpenGLResourceDeclaration::UsageFrequency::Normal;
                    break;
            }

            declaration.AddResource(declResource);
        }

        // Validate and optimize the exported declaration
        declaration.Validate();
        declaration.OptimizeBindingLayout(true);

        OLO_CORE_TRACE("Exported {} resources to OpenGL declaration '{}'", 
                      declaration.GetResourceCount(), actualPassName);

        return declaration;
    }

    void UniformBufferRegistry::DiscoverResources(u32 stage, const std::vector<u32>& spirvData)
    {
        // Convert stage to readable string for logging
        std::string stageName = "Unknown";
        switch (stage)
        {
            case 0x8B31: stageName = "Vertex"; break;      // GL_VERTEX_SHADER
            case 0x8B30: stageName = "Fragment"; break;    // GL_FRAGMENT_SHADER
            case 0x8DD9: stageName = "Geometry"; break;    // GL_GEOMETRY_SHADER
            case 0x8E88: stageName = "TessControl"; break; // GL_TESS_CONTROL_SHADER
            case 0x8E87: stageName = "TessEval"; break;    // GL_TESS_EVALUATION_SHADER
            case 0x91B9: stageName = "Compute"; break;     // GL_COMPUTE_SHADER
        }

        OLO_CORE_TRACE("Discovering resources for {0} shader stage", stageName);

        try
        {
            const spirv_cross::Compiler compiler(spirvData);
            const spirv_cross::ShaderResources resources = compiler.get_shader_resources();

            // Discover uniform buffers
            for (const auto& resource : resources.uniform_buffers)
            {
                const auto& bufferType = compiler.get_type(resource.base_type_id);
                sizet bufferSize = compiler.get_declared_struct_size(bufferType);
                u32 binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
                u32 set = 0; // OpenGL doesn't use descriptor sets, default to 0

                // Check if binding point is already used
                auto it = m_BindingPointUsage.find(binding);
                if (it != m_BindingPointUsage.end())
                {
                    OLO_CORE_WARN("Binding point {0} already used by resource '{1}', skipping '{2}'", 
                                  binding, it->second, resource.name);
                    continue;
                }

                ShaderResourceBinding bindingInfo(
                    ShaderResourceType::UniformBuffer,
                    binding,
                    set,
                    resource.name,
                    bufferSize
                );

                m_ResourceBindings[resource.name] = bindingInfo;
                m_BindingPointUsage[binding] = resource.name;

                OLO_CORE_TRACE("Discovered uniform buffer: {0} (binding={1}, size={2})", 
                              resource.name, binding, bufferSize);
            }

            // Discover storage buffers (SSBOs)
            for (const auto& resource : resources.storage_buffers)
            {
                const auto& bufferType = compiler.get_type(resource.base_type_id);
                sizet bufferSize = compiler.get_declared_struct_size(bufferType);
                u32 binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
                u32 set = 0; // OpenGL doesn't use descriptor sets, default to 0

                // Check if binding point is already used
                auto it = m_BindingPointUsage.find(binding);
                if (it != m_BindingPointUsage.end())
                {
                    OLO_CORE_WARN("Binding point {0} already used by resource '{1}', skipping '{2}'", 
                                  binding, it->second, resource.name);
                    continue;
                }

                ShaderResourceBinding bindingInfo(
                    ShaderResourceType::StorageBuffer,
                    binding,
                    set,
                    resource.name,
                    bufferSize
                );

                m_ResourceBindings[resource.name] = bindingInfo;
                m_BindingPointUsage[binding] = resource.name;

                OLO_CORE_TRACE("Discovered storage buffer: {0} (binding={1}, size={2})", 
                              resource.name, binding, bufferSize);
            }

            // Discover sampled images (textures)
            for (const auto& resource : resources.sampled_images)
            {
                u32 binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
                u32 set = 0; // OpenGL doesn't use descriptor sets, default to 0

                // Check if binding point is already used
                auto it = m_BindingPointUsage.find(binding);
                if (it != m_BindingPointUsage.end())
                {
                    OLO_CORE_WARN("Binding point {0} already used by resource '{1}', skipping '{2}'", 
                                  binding, it->second, resource.name);
                    continue;
                }

                // Determine texture type based on the SPIR-V type
                const auto& imageType = compiler.get_type(resource.type_id);
                ShaderResourceType resourceType = ShaderResourceType::Texture2D; // Default to 2D
                bool isArray = false;
                u32 arraySize = 0;

                // Check if this is an array texture
                if (imageType.array.size() > 0)
                {
                    isArray = true;
                    arraySize = imageType.array[0]; // Get array size
                    
                    if (imageType.image.dim == spv::DimCube)
                    {
                        resourceType = ShaderResourceType::TextureCubeArray;
                    }
                    else if (imageType.image.dim == spv::Dim2D)
                    {
                        resourceType = ShaderResourceType::Texture2DArray;
                    }
                }
                else
                {
                    if (imageType.image.dim == spv::DimCube)
                    {
                        resourceType = ShaderResourceType::TextureCube;
                    }
                    else if (imageType.image.dim == spv::Dim2D)
                    {
                        resourceType = ShaderResourceType::Texture2D;
                    }
                }

                ShaderResourceBinding bindingInfo;
                if (isArray)
                {
                    bindingInfo = ShaderResourceBinding(resourceType, binding, set, resource.name, arraySize);
                }
                else
                {
                    bindingInfo = ShaderResourceBinding(resourceType, binding, set, resource.name);
                }

                m_ResourceBindings[resource.name] = bindingInfo;
                m_BindingPointUsage[binding] = resource.name;

                if (isArray)
                {
                    OLO_CORE_TRACE("Discovered texture array: {0} (binding={1}, type={2}, size={3})", 
                                  resource.name, binding, static_cast<u32>(resourceType), arraySize);
                }
                else
                {
                    OLO_CORE_TRACE("Discovered texture: {0} (binding={1}, type={2})", 
                                  resource.name, binding, static_cast<u32>(resourceType));
                }
            }

            // Discover storage images (for future Image2D support)
            for (const auto& resource : resources.storage_images)
            {
                u32 binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
                u32 set = 0;

                auto it = m_BindingPointUsage.find(binding);
                if (it != m_BindingPointUsage.end())
                {
                    OLO_CORE_WARN("Binding point {0} already used by resource '{1}', skipping '{2}'", 
                                  binding, it->second, resource.name);
                    continue;
                }

                ShaderResourceBinding bindingInfo(
                    ShaderResourceType::Image2D,
                    binding,
                    set,
                    resource.name
                );

                m_ResourceBindings[resource.name] = bindingInfo;
                m_BindingPointUsage[binding] = resource.name;

                OLO_CORE_TRACE("Discovered storage image: {0} (binding={1})", 
                              resource.name, binding);
            }
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("Failed to reflect shader resources: {0}", e.what());
        }
    }



    bool UniformBufferRegistry::SetResource(const std::string& name, const ShaderResourceInput& input)
    {
        if (!m_Initialized)
        {
            OLO_CORE_ERROR("UniformBufferRegistry not initialized. Call Initialize() first.");
            return false;
        }

        // Find the binding information
        auto bindingIt = m_ResourceBindings.find(name);
        if (bindingIt == m_ResourceBindings.end())
        {
            OLO_CORE_WARN("Resource '{0}' not found in shader resource bindings", name);
            return false;
        }

        ShaderResourceBinding& binding = bindingIt->second;

        // Phase 1.3: Use enhanced compatibility system
        if (!IsCompatibleResource(binding, input))
        {
            OLO_CORE_ERROR("Resource compatibility check failed for '{0}'", name);
            return false;
        }

        // Phase 1.2: Two-phase update - add to pending resources
        m_PendingResources[name] = input.Resource;
        
        // Phase 1.1: Mark binding as dirty for GPU handle tracking
        binding.MarkDirty();
        MarkBindingDirty(name);

        // Phase 6.1: Invalidate cached handle when resource changes
        InvalidateCachedHandle(name);

        // Remove from invalidated set if present
        m_InvalidatedResources.erase(name);

        OLO_CORE_TRACE("Set resource '{0}' (type={1}) - added to pending updates", name, static_cast<u32>(input.Type));
        
        // Update debug information (only if shader is available)
        if (m_Shader)
        {
            ShaderDebugger::GetInstance().UpdateResourceBinding(m_Shader->GetRendererID(), name, binding.Type, binding.BindingPoint, true);
        }
        
        return true;
    }

    void UniformBufferRegistry::ApplyBindings()
    {
        if (!m_Initialized)
        {
            OLO_CORE_ERROR("UniformBufferRegistry not initialized. Call Initialize() first.");
            return;
        }

        if (m_DirtyBindings.empty())
            return;

        // Phase 3.1+: Use descriptor set system for efficient batch binding
        if (m_UseSetPriority && m_DescriptorSetManager)
        {
            // Update descriptor set resources
            for (const std::string& name : m_DirtyBindings)
            {
                auto bindingIt = m_ResourceBindings.find(name);
                auto resourceIt = m_BoundResources.find(name);

                if (bindingIt != m_ResourceBindings.end() && resourceIt != m_BoundResources.end())
                {
                    const ShaderResourceBinding& binding = bindingIt->second;
                    const ShaderResource& resource = resourceIt->second;
                    
                    // Get resource OpenGL ID
                    u32 resourceID = 0;
                    GLenum target = 0;
                    
                    if (std::holds_alternative<Ref<UniformBuffer>>(resource))
                    {
                        auto uniformBuffer = std::get<Ref<UniformBuffer>>(resource);
                        resourceID = uniformBuffer->GetRendererID();
                        target = GL_UNIFORM_BUFFER;
                    }
                    else if (std::holds_alternative<Ref<StorageBuffer>>(resource))
                    {
                        auto storageBuffer = std::get<Ref<StorageBuffer>>(resource);
                        resourceID = storageBuffer->GetRendererID();
                        target = GL_SHADER_STORAGE_BUFFER;
                    }
                    else if (std::holds_alternative<Ref<Texture2D>>(resource))
                    {
                        auto texture = std::get<Ref<Texture2D>>(resource);
                        resourceID = texture->GetRendererID();
                        target = GL_TEXTURE_2D;
                    }
                    else if (std::holds_alternative<Ref<TextureCubemap>>(resource))
                    {
                        auto textureCube = std::get<Ref<TextureCubemap>>(resource);
                        resourceID = textureCube->GetRendererID();
                        target = GL_TEXTURE_CUBE_MAP;
                    }
                    
                    if (resourceID != 0)
                    {
                        // Bind resource to descriptor set
                        u32 setIndex = binding.Set;
                        u32 localBinding = binding.BindingPoint - m_DescriptorSetManager->GetSetLayout(setIndex)->UniformBufferBaseBinding;
                        
                        // Adjust local binding based on resource type
                        if (binding.Type == ShaderResourceType::StorageBuffer)
                        {
                            localBinding = binding.BindingPoint - m_DescriptorSetManager->GetSetLayout(setIndex)->StorageBufferBaseBinding;
                        }
                        else if (binding.Type == ShaderResourceType::Texture2D || binding.Type == ShaderResourceType::TextureCube)
                        {
                            localBinding = binding.BindingPoint - m_DescriptorSetManager->GetSetLayout(setIndex)->TextureBaseBinding;
                        }
                        
                        m_DescriptorSetManager->BindResource(setIndex, name, binding.Type, localBinding, resourceID, target);
                    }
                    
                    // Mark binding as active
                    const_cast<ShaderResourceBinding&>(binding).IsActive = true;
                }
            }
            
            // Apply all descriptor sets
            m_DescriptorSetManager->BindAllSets();
            
            // Submit multi-bind operations
            if (m_MultiBind)
            {
                m_MultiBind->SubmitAll();
            }
        }
        else
        {
            // Fallback to individual binding for backward compatibility
            for (const std::string& name : m_DirtyBindings)
            {
                auto bindingIt = m_ResourceBindings.find(name);
                auto resourceIt = m_BoundResources.find(name);

                if (bindingIt != m_ResourceBindings.end() && resourceIt != m_BoundResources.end())
                {
                    ApplyResourceBinding(name, bindingIt->second, resourceIt->second);
                    
                    // Mark binding as active
                    const_cast<ShaderResourceBinding&>(bindingIt->second).IsActive = true;
                }
            }
        }

        m_DirtyBindings.clear();
    }

    void UniformBufferRegistry::ApplySetBindings(u32 setIndex)
    {
        if (!m_Initialized)
        {
            OLO_CORE_ERROR("UniformBufferRegistry not initialized. Call Initialize() first.");
            return;
        }

        if (m_UseSetPriority && m_DescriptorSetManager)
        {
            m_DescriptorSetManager->BindDescriptorSet(setIndex);
            
            // Submit multi-bind operations
            if (m_MultiBind)
            {
                m_MultiBind->SubmitAll();
            }
        }
        else
        {
            OLO_CORE_WARN("Set-based binding not available: UseSetPriority disabled or DescriptorSetManager not initialized");
        }
    }

    bool UniformBufferRegistry::Validate() const
    {
        if (!m_Initialized)
            return false;

        for (const auto& [name, binding] : m_ResourceBindings)
        {
            // Check if required resource is bound
            auto it = m_BoundResources.find(name);
            if (it == m_BoundResources.end())
            {
                OLO_CORE_WARN("Required resource '{0}' is not bound", name);
                return false;
            }

            // Check if resource is not in empty state
            if (std::holds_alternative<std::monostate>(it->second))
            {
                OLO_CORE_WARN("Resource '{0}' is in empty state", name);
                return false;
            }
        }

        return true;
    }

    bool UniformBufferRegistry::IsResourceBound(const std::string& name) const
    {
        auto it = m_BoundResources.find(name);
        return it != m_BoundResources.end() && !std::holds_alternative<std::monostate>(it->second);
    }

    const ShaderResourceBinding* UniformBufferRegistry::GetBindingInfo(const std::string& name) const
    {
        auto it = m_ResourceBindings.find(name);
        return it != m_ResourceBindings.end() ? &it->second : nullptr;
    }

    void UniformBufferRegistry::ClearResources()
    {
        m_BoundResources.clear();
        m_DirtyBindings.clear();

        // Mark all bindings as inactive
        for (auto& [name, binding] : m_ResourceBindings)
        {
            binding.IsActive = false;
        }
    }

    UniformBufferRegistry::Statistics UniformBufferRegistry::GetStatistics() const
    {
        Statistics stats;
        stats.TotalBindings = static_cast<u32>(m_ResourceBindings.size());
        stats.BoundResources = static_cast<u32>(m_BoundResources.size());
        stats.DirtyBindings = static_cast<u32>(m_DirtyBindings.size());

        for (const auto& [name, binding] : m_ResourceBindings)
        {
            switch (binding.Type)
            {
                case ShaderResourceType::UniformBuffer:
                case ShaderResourceType::StorageBuffer:
                    stats.UniformBuffers++;
                    break;
                case ShaderResourceType::Texture2D:
                case ShaderResourceType::TextureCube:
                case ShaderResourceType::Image2D:
                    stats.Textures++;
                    break;
                default:
                    break;
            }
        }

        return stats;
    }

    std::vector<std::string> UniformBufferRegistry::GetMissingResources() const
    {
        std::vector<std::string> missing;

        for (const auto& [name, binding] : m_ResourceBindings)
        {
            if (!IsResourceBound(name))
            {
                missing.push_back(name);
            }
        }

        return missing;
    }

    OpenGLResourceDeclaration& UniformBufferRegistry::GetOpenGLDeclaration(const std::string& passName)
    {
        std::string actualPassName = passName.empty() ? m_DefaultPassName : passName;
        
        auto it = m_OpenGLDeclarations.find(actualPassName);
        if (it == m_OpenGLDeclarations.end())
        {
            // Create new declaration if it doesn't exist
            auto declaration = std::make_unique<OpenGLResourceDeclaration>(actualPassName);
            
            // Populate with current registry state
            for (const auto& [name, binding] : m_ResourceBindings)
            {
                OpenGLResourceDeclaration::ResourceInfo resourceInfo;
                resourceInfo.Name = name;
                resourceInfo.Type = binding.Type;
                resourceInfo.Set = binding.Set;
                resourceInfo.Binding = binding.BindingPoint;
                resourceInfo.Size = static_cast<u32>(binding.Size);
                resourceInfo.IsArray = binding.IsArray;
                resourceInfo.ArraySize = binding.ArraySize;
                
                declaration->AddResource(resourceInfo);
            }
            
            auto& ref = *declaration;
            m_OpenGLDeclarations[actualPassName] = std::move(declaration);
            return ref;
        }
        
        return *it->second;
    }

    const OpenGLResourceDeclaration* UniformBufferRegistry::GetOpenGLDeclaration(const std::string& passName) const
    {
        std::string actualPassName = passName.empty() ? m_DefaultPassName : passName;
        
        auto it = m_OpenGLDeclarations.find(actualPassName);
        if (it == m_OpenGLDeclarations.end())
        {
            return nullptr;
        }
        
        return it->second.get();
    }

    void UniformBufferRegistry::RenderDebugInterface()
    {
        if (!m_Initialized)
        {
            ImGui::Text("Registry not initialized");
            return;
        }

        const Statistics stats = GetStatistics();

        ImGui::Text("Registry Statistics:");
        ImGui::Indent();
        ImGui::Text("Total Bindings: %u", stats.TotalBindings);
        ImGui::Text("Bound Resources: %u", stats.BoundResources);
        ImGui::Text("Uniform Buffers: %u", stats.UniformBuffers);
        ImGui::Text("Textures: %u", stats.Textures);
        ImGui::Text("Dirty Bindings: %u", stats.DirtyBindings);
        ImGui::Unindent();

        ImGui::Separator();

        if (ImGui::CollapsingHeader("Resource Bindings"))
        {
            ImGui::Columns(5, "ResourceBindings");
            ImGui::Text("Name");
            ImGui::NextColumn();
            ImGui::Text("Type");
            ImGui::NextColumn();
            ImGui::Text("Binding");
            ImGui::NextColumn();
            ImGui::Text("Size");
            ImGui::NextColumn();
            ImGui::Text("Status");
            ImGui::NextColumn();
            ImGui::Separator();

            for (const auto& [name, binding] : m_ResourceBindings)
            {
                ImGui::Text("%s", name.c_str());
                ImGui::NextColumn();

                const char* typeName = "Unknown";
                switch (binding.Type)
                {
                    case ShaderResourceType::UniformBuffer:
                        typeName = "UniformBuffer";
                        break;
                    case ShaderResourceType::StorageBuffer:
                        typeName = "StorageBuffer";
                        break;
                    case ShaderResourceType::Texture2D:
                        typeName = "Texture2D";
                        break;
                    case ShaderResourceType::TextureCube:
                        typeName = "TextureCube";
                        break;
                    case ShaderResourceType::Image2D:
                        typeName = "Image2D";
                        break;
                    default:
                        break;
                }
                ImGui::Text("%s", typeName);
                ImGui::NextColumn();

                ImGui::Text("%u", binding.BindingPoint);
                ImGui::NextColumn();

                ImGui::Text("%zu", binding.Size);
                ImGui::NextColumn();

                if (IsResourceBound(name))
                {
                    if (binding.IsActive)
                    {
                        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Active");
                    }
                    else
                    {
                        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Bound");
                    }
                }
                else
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Missing");
                }
                ImGui::NextColumn();
            }

            ImGui::Columns(1);
        }

        // Show missing resources
        const std::vector<std::string> missing = GetMissingResources();
        if (!missing.empty())
        {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Missing Resources:");
            for (const std::string& name : missing)
            {
                ImGui::BulletText("%s", name.c_str());
            }
        }
    }

    bool UniformBufferRegistry::ValidateResourceType(const ShaderResourceBinding& binding, 
                                                   const ShaderResourceInput& input) const
    {
        return binding.Type == input.Type;
    }

    void UniformBufferRegistry::ApplyResourceBinding(const std::string& name, 
                                                   const ShaderResourceBinding& binding, 
                                                   const ShaderResource& resource)
    {
        // Phase 1.1 + 6.1: Helper lambda to extract and store GPU handle + cache it
        auto extractAndStoreGPUHandle = [this](ShaderResourceBinding& binding, u32 handle, 
                                              ShaderResourceType type, u32 memorySize = 0) {
            binding.SetOpenGLHandle(handle);
            
            // Phase 6.1: Cache the handle for fast access
            if (m_HandleCache && m_HandleCachingEnabled)
            {
                m_HandleCache->CacheHandle(binding.Name, handle, type, memorySize);
            }
            
            OLO_CORE_TRACE("Stored and cached GPU handle {0} for resource '{1}'", handle, binding.Name);
        };
        
        // Get mutable reference to binding for GPU handle tracking
        auto bindingIt = m_ResourceBindings.find(name);
        if (bindingIt == m_ResourceBindings.end())
        {
            OLO_CORE_ERROR("Cannot apply binding for unknown resource: '{0}'", name);
            return;
        }
        ShaderResourceBinding& mutableBinding = bindingIt->second;

        switch (binding.Type)
        {
            case ShaderResourceType::UniformBuffer:
            {
                if (std::holds_alternative<Ref<UniformBuffer>>(resource))
                {
                    auto buffer = std::get<Ref<UniformBuffer>>(resource);
                    if (buffer)
                    {
                        // Note: UniformBuffers are automatically bound to their binding point when created
                        // No explicit bind call is needed here
                        // Phase 1.1 + 6.1: Store GPU handle and cache it
                        extractAndStoreGPUHandle(mutableBinding, buffer->GetRendererID(), 
                                               ShaderResourceType::UniformBuffer, buffer->GetSize());
                        OLO_CORE_TRACE("Applied uniform buffer '{0}' to binding point {1}", name, binding.BindingPoint);
                    }
                }
                break;
            }
            case ShaderResourceType::StorageBuffer:
            {
                if (std::holds_alternative<Ref<StorageBuffer>>(resource))
                {
                    auto buffer = std::get<Ref<StorageBuffer>>(resource);
                    if (buffer)
                    {
                        buffer->Bind(binding.BindingPoint);
                        // Phase 1.1 + 6.1: Store GPU handle and cache it
                        extractAndStoreGPUHandle(mutableBinding, buffer->GetRendererID(), 
                                               ShaderResourceType::StorageBuffer, buffer->GetSize());
                        OLO_CORE_TRACE("Applied storage buffer '{0}' to binding point {1}", name, binding.BindingPoint);
                    }
                }
                break;
            }
            case ShaderResourceType::Texture2D:
            {
                if (std::holds_alternative<Ref<Texture2D>>(resource))
                {
                    auto texture = std::get<Ref<Texture2D>>(resource);
                    if (texture)
                    {
                        texture->Bind(binding.BindingPoint);
                        // Phase 1.1 + 6.1: Store GPU handle and cache it
                        extractAndStoreGPUHandle(mutableBinding, texture->GetRendererID(), 
                                               ShaderResourceType::Texture2D);
                        OLO_CORE_TRACE("Applied texture2D '{0}' to binding point {1}", name, binding.BindingPoint);
                    }
                }
                break;
            }
            case ShaderResourceType::TextureCube:
            {
                if (std::holds_alternative<Ref<TextureCubemap>>(resource))
                {
                    auto texture = std::get<Ref<TextureCubemap>>(resource);
                    if (texture)
                    {
                        texture->Bind(binding.BindingPoint);
                        // Phase 1.1 + 6.1: Store GPU handle and cache it
                        extractAndStoreGPUHandle(mutableBinding, texture->GetRendererID(), 
                                               ShaderResourceType::TextureCube);
                        OLO_CORE_TRACE("Applied textureCube '{0}' to binding point {1}", name, binding.BindingPoint);
                    }
                }
                break;
            }
            // Array resource types (Phase 1.2)
            case ShaderResourceType::UniformBufferArray:
            {
                if (std::holds_alternative<Ref<UniformBufferArray>>(resource))
                {
                    auto bufferArray = std::get<Ref<UniformBufferArray>>(resource);
                    if (bufferArray)
                    {
                        bufferArray->BindArray();
                        OLO_CORE_TRACE("Applied uniform buffer array '{0}' starting at binding point {1}", name, binding.BindingPoint);
                    }
                }
                break;
            }
            case ShaderResourceType::StorageBufferArray:
            {
                if (std::holds_alternative<Ref<StorageBufferArray>>(resource))
                {
                    auto bufferArray = std::get<Ref<StorageBufferArray>>(resource);
                    if (bufferArray)
                    {
                        bufferArray->BindArray();
                        OLO_CORE_TRACE("Applied storage buffer array '{0}' starting at binding point {1}", name, binding.BindingPoint);
                    }
                }
                break;
            }
            case ShaderResourceType::Texture2DArray:
            {
                if (std::holds_alternative<Ref<Texture2DArray>>(resource))
                {
                    auto textureArray = std::get<Ref<Texture2DArray>>(resource);
                    if (textureArray)
                    {
                        textureArray->BindArray();
                        OLO_CORE_TRACE("Applied texture2D array '{0}' starting at binding point {1}", name, binding.BindingPoint);
                    }
                }
                break;
            }
            case ShaderResourceType::TextureCubeArray:
            {
                if (std::holds_alternative<Ref<TextureCubemapArray>>(resource))
                {
                    auto textureArray = std::get<Ref<TextureCubemapArray>>(resource);
                    if (textureArray)
                    {
                        textureArray->BindArray();
                        OLO_CORE_TRACE("Applied textureCube array '{0}' starting at binding point {1}", name, binding.BindingPoint);
                    }
                }
                break;
            }
            default:
                OLO_CORE_WARN("Unsupported resource type {0} for resource '{1}'", 
                             static_cast<u32>(binding.Type), name);
                break;
        }
    }

    void UniformBufferRegistry::MarkBindingDirty(const std::string& name)
    {
        m_DirtyBindings.insert(name);
    }

    // Frame-in-flight implementation (Phase 1.3)

    void UniformBufferRegistry::EnableFrameInFlight(u32 framesInFlight)
    {
        if (m_FrameInFlightEnabled)
        {
            OLO_CORE_WARN("Frame-in-flight already enabled for UniformBufferRegistry");
            return;
        }

        m_FrameInFlightManager = std::make_unique<FrameInFlightManager>(framesInFlight);
        m_FrameInFlightEnabled = true;

        OLO_CORE_INFO("Frame-in-flight enabled for UniformBufferRegistry with {0} frames", framesInFlight);
    }

    void UniformBufferRegistry::DisableFrameInFlight()
    {
        if (!m_FrameInFlightEnabled)
        {
            OLO_CORE_WARN("Frame-in-flight not enabled for UniformBufferRegistry");
            return;
        }

        m_FrameInFlightManager.reset();
        m_FrameInFlightEnabled = false;

        OLO_CORE_INFO("Frame-in-flight disabled for UniformBufferRegistry");
    }

    void UniformBufferRegistry::RegisterFrameInFlightResource(const std::string& name, ShaderResourceType type, u32 size, 
                                                            BufferUsage usage, u32 arraySize, u32 baseBindingPoint)
    {
        if (!m_FrameInFlightEnabled || !m_FrameInFlightManager)
        {
            OLO_CORE_ERROR("Frame-in-flight not enabled. Call EnableFrameInFlight() first.");
            return;
        }

        switch (type)
        {
            case ShaderResourceType::UniformBuffer:
                m_FrameInFlightManager->RegisterUniformBuffer(name, size, baseBindingPoint, usage);
                break;

            case ShaderResourceType::StorageBuffer:
                m_FrameInFlightManager->RegisterStorageBuffer(name, size, usage);
                break;

            case ShaderResourceType::UniformBufferArray:
                m_FrameInFlightManager->RegisterUniformBufferArray(name, baseBindingPoint, arraySize, size, usage);
                break;

            case ShaderResourceType::StorageBufferArray:
                m_FrameInFlightManager->RegisterStorageBufferArray(name, baseBindingPoint, arraySize, size, usage);
                break;

            default:
                OLO_CORE_WARN("Unsupported resource type {0} for frame-in-flight: '{1}'", 
                             static_cast<u32>(type), name);
                break;
        }

        OLO_CORE_TRACE("Registered frame-in-flight resource: '{0}' (type: {1})", name, static_cast<u32>(type));
    }

    void UniformBufferRegistry::NextFrame()
    {
        if (m_FrameInFlightEnabled && m_FrameInFlightManager)
        {
            m_FrameInFlightManager->NextFrame();
        }
    }

    FrameInFlightManager::Statistics UniformBufferRegistry::GetFrameInFlightStatistics() const
    {
        if (m_FrameInFlightEnabled && m_FrameInFlightManager)
        {
            return m_FrameInFlightManager->GetStatistics();
        }

        return {}; // Return empty statistics if not enabled
    }

    // ================================================================================================
    // Phase 1.2: Two-Phase Resource Updates Implementation
    // ================================================================================================

    void UniformBufferRegistry::InvalidateResource(const std::string& name)
    {
        // Delegate to the enhanced invalidation method with default reason
        InvalidateResourceWithReason(name, InvalidationReason::UserRequested, false);
    }

    void UniformBufferRegistry::CommitPendingUpdates()
    {
        if (m_PendingResources.empty())
        {
            return; // No pending updates
        }

        u32 committedCount = 0;
        u32 currentFrame = static_cast<u32>(std::chrono::steady_clock::now().time_since_epoch().count());

        // Batch commit all pending resources
        for (auto& [name, resource] : m_PendingResources)
        {
            auto bindingIt = m_ResourceBindings.find(name);
            if (bindingIt != m_ResourceBindings.end())
            {
                // Apply the resource binding
                ApplyResourceBinding(name, bindingIt->second, resource);

                // Update frame tracking and dirty state
                bindingIt->second.UpdateBindFrame(currentFrame);

                // Move from pending to bound
                m_BoundResources[name] = std::move(resource);
                
                // Remove from invalidated set
                m_InvalidatedResources.erase(name);

                committedCount++;
            }
        }

        // Clear pending resources
        m_PendingResources.clear();

        if (committedCount > 0)
        {
            OLO_CORE_TRACE("Committed {0} pending resource updates", committedCount);
        }
    }

    bool UniformBufferRegistry::IsResourceInvalidated(const std::string& name) const
    {
        return m_InvalidatedResources.contains(name);
    }

    // ================================================================================================
    // Phase 1.3: Enhanced Resource Compatibility System Implementation
    // ================================================================================================

    bool UniformBufferRegistry::IsCompatibleResource(const ShaderResourceBinding& binding, const ShaderResourceInput& input) const
    {
        // First check if types match exactly
        if (binding.Type != input.Type)
        {
            OLO_CORE_WARN("Resource type mismatch for '{0}': expected {1}, got {2}", 
                         binding.Name, static_cast<u32>(binding.Type), static_cast<u32>(input.Type));
            return false;
        }

        // Check for null resources
        if (std::holds_alternative<std::monostate>(input.Resource))
        {
            OLO_CORE_WARN("Cannot bind null resource to '{0}'", binding.Name);
            return false;
        }

        // Array-specific compatibility checks
        if (binding.IsArray)
        {
            switch (binding.Type)
            {
                case ShaderResourceType::UniformBufferArray:
                    return std::holds_alternative<Ref<UniformBufferArray>>(input.Resource);
                case ShaderResourceType::StorageBufferArray:
                    return std::holds_alternative<Ref<StorageBufferArray>>(input.Resource);
                case ShaderResourceType::Texture2DArray:
                    return std::holds_alternative<Ref<Texture2DArray>>(input.Resource);
                case ShaderResourceType::TextureCubeArray:
                    return std::holds_alternative<Ref<TextureCubemapArray>>(input.Resource);
                default:
                    OLO_CORE_WARN("Unknown array resource type for '{0}'", binding.Name);
                    return false;
            }
        }

        // Non-array resource compatibility checks
        switch (binding.Type)
        {
            case ShaderResourceType::UniformBuffer:
                return std::holds_alternative<Ref<UniformBuffer>>(input.Resource);
            case ShaderResourceType::StorageBuffer:
                return std::holds_alternative<Ref<StorageBuffer>>(input.Resource);
            case ShaderResourceType::Texture2D:
                return std::holds_alternative<Ref<Texture2D>>(input.Resource);
            case ShaderResourceType::TextureCube:
                return std::holds_alternative<Ref<TextureCubemap>>(input.Resource);
            default:
                OLO_CORE_WARN("Unknown resource type {0} for compatibility check", static_cast<u32>(binding.Type));
                return false;
        }
    }

    GLenum UniformBufferRegistry::MapToOpenGLResourceType(ShaderResourceType type) const
    {
        switch (type)
        {
            case ShaderResourceType::UniformBuffer:
            case ShaderResourceType::UniformBufferArray:
                return GL_UNIFORM_BUFFER;

            case ShaderResourceType::StorageBuffer:
            case ShaderResourceType::StorageBufferArray:
                return GL_SHADER_STORAGE_BUFFER;

            case ShaderResourceType::Texture2D:
            case ShaderResourceType::Texture2DArray:
                return GL_TEXTURE_2D;

            case ShaderResourceType::TextureCube:
            case ShaderResourceType::TextureCubeArray:
                return GL_TEXTURE_CUBE_MAP;

            case ShaderResourceType::Image2D:
                return GL_TEXTURE_2D; // Images use texture storage

            case ShaderResourceType::None:
            default:
                OLO_CORE_WARN("Cannot map unknown resource type {0} to OpenGL", static_cast<u32>(type));
                return GL_NONE;
        }
    }

    // ================================================================================================
    // Phase 2.1: Template and Clone Support Implementation
    // ================================================================================================

    Scope<UniformBufferRegistry> UniformBufferRegistry::CreateTemplate(const UniformBufferRegistry& templateRegistry, 
                                                                      const std::string& templateName)
    {
        if (!templateRegistry.m_Specification.AllowTemplateCreation)
        {
            OLO_CORE_ERROR("Template creation not allowed for registry '{0}'", templateRegistry.m_Specification.Name);
            return nullptr;
        }

        // Create template specification
        auto templateSpec = templateRegistry.m_Specification;
        templateSpec.Name = templateName.empty() ? templateRegistry.m_Specification.Name + "_Template" : templateName;
        templateSpec.TemplateSource = templateRegistry.m_Specification.Name;

        // Create template registry without shader (templates are shader-agnostic)
        auto templateReg = CreateScope<UniformBufferRegistry>(nullptr, templateSpec);
        templateReg->m_IsTemplate = true;
        templateReg->m_TemplateName = templateSpec.Name;
        
        // Copy bindings but not bound resources
        templateReg->CopyBindingsFrom(templateRegistry, false);
        
        OLO_CORE_INFO("Created template registry '{0}' from source '{1}'", 
                     templateSpec.Name, templateRegistry.m_Specification.Name);
        
        return templateReg;
    }

    Scope<UniformBufferRegistry> UniformBufferRegistry::Clone(const Ref<Shader>& targetShader, 
                                                            const std::string& cloneName) const
    {
        if (!m_Specification.AllowCloning)
        {
            OLO_CORE_ERROR("Cloning not allowed for registry '{0}'", m_Specification.Name);
            return nullptr;
        }

        if (!targetShader)
        {
            OLO_CORE_ERROR("Target shader cannot be null for cloning");
            return nullptr;
        }

        // Create clone specification
        auto cloneSpec = m_Specification;
        cloneSpec.Name = cloneName.empty() ? m_Specification.Name + "_Clone" : cloneName;
        cloneSpec.TemplateSource = m_IsTemplate ? m_TemplateName : m_Specification.Name;

        // Create cloned registry
        auto clonedReg = CreateScope<UniformBufferRegistry>(targetShader, cloneSpec);
        clonedReg->m_IsClone = true;
        clonedReg->m_SourceTemplateName = cloneSpec.TemplateSource;
        
        // Copy bindings and validate compatibility
        clonedReg->CopyBindingsFrom(*this, true);
        
        if (!clonedReg->ValidateCloneCompatibility(targetShader))
        {
            OLO_CORE_ERROR("Clone validation failed for target shader '{0}'", targetShader->GetName());
            return nullptr;
        }
        
        OLO_CORE_INFO("Successfully cloned registry '{0}' to '{1}' for shader '{2}'", 
                     m_Specification.Name, cloneSpec.Name, targetShader->GetName());
        
        return clonedReg;
    }

    Scope<UniformBufferRegistry> UniformBufferRegistry::CreateFromTemplate(const UniformBufferRegistry& templateRegistry,
                                                                          const Ref<Shader>& targetShader,
                                                                          const std::string& instanceName)
    {
        if (!templateRegistry.m_IsTemplate)
        {
            OLO_CORE_ERROR("Source registry '{0}' is not a template", templateRegistry.m_Specification.Name);
            return nullptr;
        }

        return templateRegistry.Clone(targetShader, instanceName);
    }

    bool UniformBufferRegistry::ValidateTemplateCompatibility(const Ref<Shader>& targetShader) const
    {
        if (!targetShader)
        {
            OLO_CORE_ERROR("Target shader cannot be null for compatibility validation");
            return false;
        }

        // For now, this is a placeholder - would need to implement SPIR-V reflection comparison
        // In a real implementation, this would:
        // 1. Compare resource names and types between template and target shader
        // 2. Validate binding points compatibility
        // 3. Check resource size requirements
        // 4. Ensure all template resources exist in target shader

        OLO_CORE_TRACE("Template compatibility validation for shader '{0}' - placeholder implementation", 
                      targetShader->GetName());
        
        return true; // Placeholder - assume compatible for now
    }

    void UniformBufferRegistry::UpdateSpecification(const UniformBufferRegistrySpecification& newSpec, bool reinitialize)
    {
        if (!newSpec.Validate())
        {
            OLO_CORE_ERROR("Invalid specification provided for registry '{0}'", m_Specification.Name);
            return;
        }

        auto oldSpec = m_Specification;
        m_Specification = newSpec;
        
        // Apply new settings
        ApplySpecificationSettings();
        
        if (reinitialize && m_Initialized)
        {
            OLO_CORE_INFO("Reinitializing registry '{0}' with new specification", m_Specification.Name);
            Shutdown();
            Initialize();
        }
        
        OLO_CORE_TRACE("Updated specification for registry '{0}'", m_Specification.Name);
    }

    void UniformBufferRegistry::CopyBindingsFrom(const UniformBufferRegistry& source, bool includeResources)
    {
        // Copy resource bindings
        m_ResourceBindings = source.m_ResourceBindings;
        
        // Copy bound resources if requested
        if (includeResources)
        {
            m_BoundResources = source.m_BoundResources;
        }
        
        // Copy binding point usage
        m_BindingPointUsage = source.m_BindingPointUsage;
        
        // Note: We don't copy dirty bindings, pending resources, or invalidated resources
        // The cloned registry starts with a clean state
        
        OLO_CORE_TRACE("Copied bindings from source registry (includeResources: {0})", includeResources);
    }

    bool UniformBufferRegistry::ValidateCloneCompatibility(const Ref<Shader>& targetShader) const
    {
        if (!targetShader)
        {
            OLO_CORE_ERROR("Target shader cannot be null for clone compatibility validation");
            return false;
        }

        // Basic validation - in a real implementation, this would use shader reflection
        // to ensure the target shader has all the resources that the template expects
        
        if (m_ResourceBindings.empty())
        {
            OLO_CORE_WARN("No resource bindings to validate for clone compatibility");
            return true;
        }

        // Placeholder validation - assume compatible
        // Real implementation would:
        // 1. Use SPIR-V reflection on target shader
        // 2. Compare resource names, types, and binding points
        // 3. Validate resource sizes and requirements
        // 4. Check for binding conflicts

        OLO_CORE_TRACE("Clone compatibility validation passed for shader '{0}' (placeholder)", 
                      targetShader->GetName());
        
        return true;
    }

    void UniformBufferRegistry::ApplySpecificationSettings()
    {
        // Apply frame-in-flight settings
        if (m_Specification.EnableFrameInFlight && !m_FrameInFlightEnabled)
        {
            EnableFrameInFlight(m_Specification.FramesInFlight);
        }
        else if (!m_Specification.EnableFrameInFlight && m_FrameInFlightEnabled)
        {
            DisableFrameInFlight();
        }
        
        OLO_CORE_TRACE("Applied specification settings for registry '{0}'", m_Specification.Name);
    }

    void UniformBufferRegistry::SetupResourceTemplates()
    {
        if (!m_Specification.UseResourceTemplates)
            return;

        // Placeholder for resource template setup
        // Real implementation would:
        // 1. Analyze shader reflection data to detect common patterns
        // 2. Set up appropriate resource templates
        // 3. Configure automatic resource population

        OLO_CORE_TRACE("Set up resource templates for registry '{0}' (placeholder)", m_Specification.Name);
    }

    // ==========================================
    // Phase 3.1: Multi-Set Management
    // ==========================================

    void UniformBufferRegistry::ConfigureDescriptorSet(DescriptorSetPriority priority, u32 setIndex, const std::string& name)
    {
        std::string setName = name.empty() ? 
            ("Set" + std::to_string(setIndex) + "_" + std::to_string(static_cast<u32>(priority))) : name;

        DescriptorSetInfo setInfo(setIndex, priority, setName);
        m_DescriptorSets[setIndex] = std::move(setInfo);
        m_PriorityToSetMap[priority] = setIndex;

        UpdateSetBindingOrder();

        OLO_CORE_TRACE("Configured descriptor set {0} for priority {1} with name '{2}'", 
                      setIndex, static_cast<u32>(priority), setName);
    }

    bool UniformBufferRegistry::AssignResourceToSet(const std::string& resourceName, u32 setIndex)
    {
        auto bindingIt = m_ResourceBindings.find(resourceName);
        if (bindingIt == m_ResourceBindings.end())
        {
            OLO_CORE_ERROR("Cannot assign unknown resource '{0}' to set {1}", resourceName, setIndex);
            return false;
        }

        auto setIt = m_DescriptorSets.find(setIndex);
        if (setIt == m_DescriptorSets.end())
        {
            OLO_CORE_WARN("Set {0} not configured, creating default configuration", setIndex);
            // Create default set configuration
            DescriptorSetPriority priority = static_cast<DescriptorSetPriority>(
                std::min(setIndex, static_cast<u32>(DescriptorSetPriority::Custom)));
            ConfigureDescriptorSet(priority, setIndex);
            setIt = m_DescriptorSets.find(setIndex);
        }

        // Add resource to set
        setIt->second.ResourceNames.push_back(resourceName);
        
        // Update resource binding to include set information
        bindingIt->second.Set = setIndex;

        OLO_CORE_TRACE("Assigned resource '{0}' to descriptor set {1}", resourceName, setIndex);
        return true;
    }

    void UniformBufferRegistry::AutoAssignResourceSets(bool useHeuristics)
    {
        if (!m_AutoAssignSets)
        {
            OLO_CORE_TRACE("Auto-assignment disabled, skipping resource set assignment");
            return;
        }

        // Initialize default descriptor sets if not configured
        if (m_DescriptorSets.empty())
        {
            InitializeDescriptorSets();
        }

        u32 assignedCount = 0;
        for (auto& [resourceName, binding] : m_ResourceBindings)
        {
            if (binding.Set == UINT32_MAX) // Not assigned yet
            {
                DescriptorSetPriority priority = useHeuristics ? 
                    DetermineResourceSetPriority(resourceName, binding) : 
                    DescriptorSetPriority::Material; // Default fallback

                auto priorityIt = m_PriorityToSetMap.find(priority);
                if (priorityIt != m_PriorityToSetMap.end())
                {
                    AssignResourceToSet(resourceName, priorityIt->second);
                    assignedCount++;
                }
            }
        }

        OLO_CORE_TRACE("Auto-assigned {0} resources to descriptor sets (useHeuristics: {1})", 
                      assignedCount, useHeuristics);
    }

    const DescriptorSetInfo* UniformBufferRegistry::GetDescriptorSetInfo(u32 setIndex) const
    {
        auto it = m_DescriptorSets.find(setIndex);
        return (it != m_DescriptorSets.end()) ? &it->second : nullptr;
    }

    u32 UniformBufferRegistry::GetResourceSetIndex(const std::string& resourceName) const
    {
        auto it = m_ResourceBindings.find(resourceName);
        return (it != m_ResourceBindings.end()) ? it->second.Set : UINT32_MAX;
    }

    void UniformBufferRegistry::BindDescriptorSet(u32 setIndex)
    {
        auto setInfo = GetDescriptorSetInfo(setIndex);
        if (!setInfo || !setInfo->IsActive)
        {
            OLO_CORE_WARN("Cannot bind inactive or non-existent descriptor set {0}", setIndex);
            return;
        }

        u32 boundCount = 0;
        for (const std::string& resourceName : setInfo->ResourceNames)
        {
            auto resourceIt = m_BoundResources.find(resourceName);
            if (resourceIt != m_BoundResources.end())
            {
                // Apply the binding using the existing binding mechanism
                // TODO: Actual OpenGL binding calls would go here
                // This would be similar to ApplyBinding but for specific set resources
                // const auto& resource = resourceIt->second;
                // const auto& binding = m_ResourceBindings.at(resourceName);
                
                boundCount++;
            }
        }

        // Update frequency tracking for optimization
        const_cast<DescriptorSetInfo*>(setInfo)->BindFrequency++;

        OLO_CORE_TRACE("Bound descriptor set {0} '{1}' ({2} resources)", 
                      setIndex, setInfo->Name, boundCount);
    }

    void UniformBufferRegistry::BindAllSets()
    {
        if (!m_UseSetPriority)
        {
            // Fall back to traditional binding
            ApplyBindings();
            return;
        }

        for (u32 setIndex : m_SetBindingOrder)
        {
            BindDescriptorSet(setIndex);
        }

        OLO_CORE_TRACE("Bound all descriptor sets in priority order ({0} sets)", m_SetBindingOrder.size());
    }

    // ==========================================
    // Phase 3.2: Default Resource System
    // ==========================================

    void UniformBufferRegistry::InitializeDefaultResources(bool forceReinitialize)
    {
        if (m_DefaultResourcesInitialized && !forceReinitialize)
        {
            OLO_CORE_TRACE("Default resources already initialized, skipping");
            return;
        }

        if (!m_Specification.EnableDefaultResources)
        {
            OLO_CORE_TRACE("Default resources disabled in specification");
            return;
        }

        // Initialize built-in templates first
        InitializeBuiltinTemplates();

        // Create system defaults if enabled
        if (m_Specification.CreateSystemDefaults)
        {
            CreateSystemDefaults();
        }

        // Auto-detect shader pattern and create appropriate defaults
        if (m_Specification.AutoDetectShaderPattern)
        {
            std::string detectedPattern = DetectShaderPattern();
            if (!detectedPattern.empty())
            {
                ApplyResourceTemplate(detectedPattern);
                OLO_CORE_TRACE("Applied detected shader pattern: {0}", detectedPattern);
            }
        }

        m_DefaultResourcesInitialized = true;
        OLO_CORE_TRACE("Initialized default resources for registry '{0}'", m_Specification.Name);
    }

    void UniformBufferRegistry::AddDefaultResource(const std::string& resourceName, const ShaderResourceInfo& resourceInfo)
    {
        m_DefaultResources[resourceName] = resourceInfo;
        OLO_CORE_TRACE("Added default resource template: {0}", resourceName);
    }

    void UniformBufferRegistry::CreateSystemDefaults()
    {
        CreateDefaultSystemBuffer();
        CreateDefaultLightingBuffer();
        
        OLO_CORE_TRACE("Created system default resources");
    }

    void UniformBufferRegistry::CreateMaterialDefaults()
    {
        CreateDefaultMaterialBuffer();
        SetupDefaultTextures();
        
        OLO_CORE_TRACE("Created material default resources");
    }

    bool UniformBufferRegistry::ApplyResourceTemplate(const std::string& templateName)
    {
        auto templateIt = m_ResourceTemplates.find(templateName);
        if (templateIt == m_ResourceTemplates.end())
        {
            OLO_CORE_WARN("Resource template '{0}' not found", templateName);
            return false;
        }

        // Apply template specification settings
        const auto& templateSpec = templateIt->second;
        
        // Merge relevant settings (don't override core configuration)
        if (templateSpec.EnableDefaultResources && !m_DefaultResourcesInitialized)
        {
            InitializeDefaultResources(true);
        }

        OLO_CORE_TRACE("Applied resource template '{0}'", templateName);
        return true;
    }

    std::string UniformBufferRegistry::DetectShaderPattern() const
    {
        return AnalyzeShaderPattern();
    }

    // ==========================================
    // Phase 3 Private Implementation Methods
    // ==========================================

    void UniformBufferRegistry::InitializeDescriptorSets()
    {
        // Create default descriptor set configuration
        ConfigureDescriptorSet(DescriptorSetPriority::System, 0, "SystemResources");
        ConfigureDescriptorSet(DescriptorSetPriority::Global, 1, "GlobalResources");
        ConfigureDescriptorSet(DescriptorSetPriority::Material, 2, "MaterialResources");
        ConfigureDescriptorSet(DescriptorSetPriority::Instance, 3, "InstanceResources");
        
        if (m_Specification.EndSet > 3)
        {
            ConfigureDescriptorSet(DescriptorSetPriority::Custom, 4, "CustomResources");
        }

        // Initialize OpenGL descriptor set layouts
        if (m_DescriptorSetManager)
        {
            // Configure automatic binding ranges based on specification
            u32 totalUniformBuffers = (m_EndSet - m_StartSet + 1) * 8;  // 8 UBOs per set
            u32 totalStorageBuffers = (m_EndSet - m_StartSet + 1) * 4;  // 4 SSBOs per set
            u32 totalTextures = (m_EndSet - m_StartSet + 1) * 16;       // 16 textures per set
            u32 totalImages = (m_EndSet - m_StartSet + 1) * 4;          // 4 images per set
            u32 setCount = m_EndSet - m_StartSet + 1;
            
            m_DescriptorSetManager->ConfigureAutomaticBindingRanges(
                totalUniformBuffers, totalStorageBuffers, totalTextures, totalImages, setCount);

            // Create OpenGL descriptor set layouts for each configured set
            for (const auto& [setIndex, setInfo] : m_DescriptorSets)
            {
                OpenGLDescriptorSetManager::DescriptorSetLayout layout(setIndex, setInfo.Name);
                
                // Configure layout based on priority
                switch (setInfo.Priority)
                {
                    case DescriptorSetPriority::System:
                        layout.MaxUniformBuffers = 4;
                        layout.MaxTextures = 4;
                        layout.Priority = 4.0f; // Highest priority
                        break;
                        
                    case DescriptorSetPriority::Global:
                        layout.MaxUniformBuffers = 8;
                        layout.MaxTextures = 16;
                        layout.Priority = 3.0f;
                        break;
                        
                    case DescriptorSetPriority::Material:
                        layout.MaxUniformBuffers = 4;
                        layout.MaxTextures = 16;
                        layout.Priority = 2.0f;
                        break;
                        
                    case DescriptorSetPriority::Instance:
                        layout.MaxUniformBuffers = 2;
                        layout.MaxStorageBuffers = 4;
                        layout.MaxTextures = 4;
                        layout.Priority = 1.0f; // Lowest priority
                        break;
                        
                    case DescriptorSetPriority::Custom:
                        layout.MaxUniformBuffers = 8;
                        layout.MaxStorageBuffers = 8;
                        layout.MaxTextures = 16;
                        layout.MaxImages = 8;
                        layout.Priority = 0.5f;
                        break;
                }
                
                m_DescriptorSetManager->CreateSetLayout(setIndex, setInfo.Name, layout);
            }
        }

        OLO_CORE_TRACE("Initialized default descriptor sets (System={0}, Global={1}, Material={2}, Instance={3})",
                      0, 1, 2, 3);
    }

    void UniformBufferRegistry::UpdateSetBindingOrder()
    {
        m_SetBindingOrder.clear();
        
        // Create ordered list based on priority (System first, Custom last)
        std::vector<std::pair<DescriptorSetPriority, u32>> prioritizedSets;
        for (const auto& [setIndex, setInfo] : m_DescriptorSets)
        {
            if (setInfo.IsActive)
            {
                prioritizedSets.emplace_back(setInfo.Priority, setIndex);
            }
        }

        // Sort by priority (lower values = higher priority)
        std::sort(prioritizedSets.begin(), prioritizedSets.end(),
                 [](const auto& a, const auto& b) { return a.first < b.first; });

        for (const auto& [priority, setIndex] : prioritizedSets)
        {
            m_SetBindingOrder.push_back(setIndex);
        }

        OLO_CORE_TRACE("Updated set binding order: {0} active sets", m_SetBindingOrder.size());
    }

    DescriptorSetPriority UniformBufferRegistry::DetermineResourceSetPriority(const std::string& resourceName, 
                                                                            const ShaderResourceBinding& resourceInfo) const
    {
        // Smart heuristics to determine appropriate descriptor set based on resource name and type
        
        // Use resource type information for better classification
        if (resourceInfo.Type == ShaderResourceType::UniformBuffer)
        {
            // Large uniform buffers are often system or global level
            if (resourceInfo.Size > 1024)  // Larger buffers tend to be system/global
            {
                if (resourceName.find("System") != std::string::npos ||
                    resourceName.find("Global") != std::string::npos)
                {
                    return DescriptorSetPriority::System;
                }
            }
        }
        
        // System-level resources (view/projection matrices, time, etc.)
        if (resourceName.find("View") != std::string::npos ||
            resourceName.find("Projection") != std::string::npos ||
            resourceName.find("Camera") != std::string::npos ||
            resourceName.find("Time") != std::string::npos ||
            resourceName.find("Delta") != std::string::npos ||
            resourceName.find("System") != std::string::npos)
        {
            return DescriptorSetPriority::System;
        }

        // Global scene resources (lighting, environment, etc.)
        if (resourceName.find("Light") != std::string::npos ||
            resourceName.find("Environment") != std::string::npos ||
            resourceName.find("Shadow") != std::string::npos ||
            resourceName.find("Global") != std::string::npos ||
            resourceName.find("Scene") != std::string::npos)
        {
            return DescriptorSetPriority::Global;
        }

        // Textures are typically material-level
        if (resourceInfo.Type == ShaderResourceType::Texture2D ||
            resourceInfo.Type == ShaderResourceType::TextureCube)
        {
            return DescriptorSetPriority::Material;
        }

        // Instance-level resources (model matrices, instance data)
        if (resourceName.find("Model") != std::string::npos ||
            resourceName.find("World") != std::string::npos ||
            resourceName.find("Instance") != std::string::npos ||
            resourceName.find("Transform") != std::string::npos)
        {
            return DescriptorSetPriority::Instance;
        }

        // Default to material-level for textures and material properties
        return DescriptorSetPriority::Material;
    }

    bool UniformBufferRegistry::ValidateSetAssignments() const
    {
        if (!m_Specification.EnableSetValidation)
            return true;

        // Check for orphaned resources (assigned to non-existent sets)
        for (const auto& [resourceName, binding] : m_ResourceBindings)
        {
            if (binding.Set != UINT32_MAX && m_DescriptorSets.find(binding.Set) == m_DescriptorSets.end())
            {
                OLO_CORE_ERROR("Resource '{0}' assigned to non-existent set {1}", resourceName, binding.Set);
                return false;
            }
        }

        // Check for set range violations
        for (const auto& [setIndex, setInfo] : m_DescriptorSets)
        {
            if (setIndex < m_StartSet || setIndex > m_EndSet)
            {
                OLO_CORE_ERROR("Descriptor set {0} outside allowed range [{1}, {2}]", 
                              setIndex, m_StartSet, m_EndSet);
                return false;
            }
        }

        return true;
    }

    void UniformBufferRegistry::InitializeBuiltinTemplates()
    {
        // Standard PBR material template
        {
            UniformBufferRegistrySpecification pbrTemplate;
            pbrTemplate.Name = "StandardPBR";
            pbrTemplate.Configuration = RegistryConfiguration::Performance;
            pbrTemplate.EnableDefaultResources = true;
            pbrTemplate.CreateSystemDefaults = true;
            m_ResourceTemplates["StandardPBR"] = pbrTemplate;
        }

        // Basic unlit template
        {
            UniformBufferRegistrySpecification unlitTemplate;
            unlitTemplate.Name = "BasicUnlit";
            unlitTemplate.Configuration = RegistryConfiguration::Performance;
            unlitTemplate.EnableDefaultResources = true;
            unlitTemplate.CreateSystemDefaults = false;
            m_ResourceTemplates["BasicUnlit"] = unlitTemplate;
        }

        // Debug wireframe template
        {
            UniformBufferRegistrySpecification debugTemplate;
            debugTemplate.Name = "DebugWireframe";
            debugTemplate.Configuration = RegistryConfiguration::Debug;
            debugTemplate.EnableDefaultResources = true;
            debugTemplate.CreateSystemDefaults = true;
            m_ResourceTemplates["DebugWireframe"] = debugTemplate;
        }

        OLO_CORE_TRACE("Initialized {0} built-in resource templates", m_ResourceTemplates.size());
    }

    void UniformBufferRegistry::CreateDefaultSystemBuffer()
    {
        // Create a default system uniform buffer with common matrices and time
        ShaderResourceInfo systemInfo;
        systemInfo.Name = "SystemUniforms";
        systemInfo.Type = ShaderResourceType::UniformBuffer;
        systemInfo.Size = 256; // Enough for view/proj matrices + time + padding
        systemInfo.Binding = 0;
        systemInfo.Set = 0; // System set
        
        AddDefaultResource("SystemUniforms", systemInfo);
        
        // Auto-assign to system set if multi-set is enabled
        if (m_UseSetPriority)
        {
            AssignResourceToSet("SystemUniforms", 0);
        }

        OLO_CORE_TRACE("Created default system uniform buffer");
    }

    void UniformBufferRegistry::CreateDefaultMaterialBuffer()
    {
        // Create a default material uniform buffer
        ShaderResourceInfo materialInfo;
        materialInfo.Name = "MaterialUniforms";
        materialInfo.Type = ShaderResourceType::UniformBuffer;
        materialInfo.Size = 128; // Material properties
        materialInfo.Binding = 1;
        materialInfo.Set = 2; // Material set
        
        AddDefaultResource("MaterialUniforms", materialInfo);
        
        if (m_UseSetPriority)
        {
            AssignResourceToSet("MaterialUniforms", 2);
        }

        OLO_CORE_TRACE("Created default material uniform buffer");
    }

    void UniformBufferRegistry::CreateDefaultLightingBuffer()
    {
        // Create a default lighting uniform buffer
        ShaderResourceInfo lightingInfo;
        lightingInfo.Name = "LightingUniforms";
        lightingInfo.Type = ShaderResourceType::UniformBuffer;
        lightingInfo.Size = 512; // Multiple lights + ambient
        lightingInfo.Binding = 2;
        lightingInfo.Set = 1; // Global set
        
        AddDefaultResource("LightingUniforms", lightingInfo);
        
        if (m_UseSetPriority)
        {
            AssignResourceToSet("LightingUniforms", 1);
        }

        OLO_CORE_TRACE("Created default lighting uniform buffer");
    }

    std::string UniformBufferRegistry::AnalyzeShaderPattern() const
    {
        if (!m_Shader)
            return "";

        // Analyze shader name and uniforms to detect common patterns
        std::string shaderName = m_Shader->GetName();
        std::transform(shaderName.begin(), shaderName.end(), shaderName.begin(), ::tolower);

        if (shaderName.find("pbr") != std::string::npos ||
            shaderName.find("standard") != std::string::npos)
        {
            return "StandardPBR";
        }
        
        if (shaderName.find("unlit") != std::string::npos ||
            shaderName.find("basic") != std::string::npos)
        {
            return "BasicUnlit";
        }
        
        if (shaderName.find("debug") != std::string::npos ||
            shaderName.find("wireframe") != std::string::npos)
        {
            return "DebugWireframe";
        }

        // Could add more sophisticated analysis based on uniform names, types, etc.
        
        return ""; // No pattern detected
    }

    void UniformBufferRegistry::SetupDefaultTextures()
    {
        // Set up default texture bindings for common material textures
        const std::vector<std::pair<std::string, u32>> defaultTextures = {
            {"DiffuseTexture", 0},
            {"NormalTexture", 1},
            {"MetallicRoughnessTexture", 2},
            {"EmissiveTexture", 3},
            {"AOTexture", 4}
        };

        for (const auto& [textureName, binding] : defaultTextures)
        {
            ShaderResourceInfo textureInfo;
            textureInfo.Name = textureName;
            textureInfo.Type = ShaderResourceType::Texture2D;
            textureInfo.Binding = binding;
            textureInfo.Set = 2; // Material set
            
            AddDefaultResource(textureName, textureInfo);
            
            if (m_UseSetPriority)
            {
                AssignResourceToSet(textureName, 2);
            }
        }

        OLO_CORE_TRACE("Set up default texture bindings for material resources");
    }

    // ==========================================
    // Phase 4: Advanced Invalidation System Implementation
    // ==========================================

    // Phase 4.1: Granular Invalidation Tracking Implementation

    void UniformBufferRegistry::InvalidateResourceWithReason(const std::string& name, InvalidationReason reason, bool propagateToDependents)
    {
        if (!m_Initialized)
        {
            OLO_CORE_WARN("Cannot invalidate resource '{0}' - registry not initialized", name);
            return;
        }

        auto bindingIt = m_ResourceBindings.find(name);
        if (bindingIt == m_ResourceBindings.end())
        {
            OLO_CORE_WARN("Cannot invalidate unknown resource: '{0}'", name);
            return;
        }

        // Create detailed invalidation info
        InvalidationInfo info(name, reason, bindingIt->second.BindingPoint);
        info.FrameInvalidated = m_CurrentFrame;

        // Get existing dependencies if any
        auto depIt = m_ResourceDependencies.find(name);
        if (depIt != m_ResourceDependencies.end())
        {
            info.Dependencies = depIt->second;
        }

        auto dependentIt = m_ResourceDependents.find(name);
        if (dependentIt != m_ResourceDependents.end())
        {
            info.Dependents = dependentIt->second;
        }

        // Store invalidation details
        m_InvalidationDetails[name] = std::move(info);

        // Update binding point invalidation tracking
        UpdateBindingPointInvalidation(bindingIt->second.BindingPoint, name, true);

        // Call original invalidation method for compatibility
        InvalidateResource(name);

        // Propagate to dependents if requested
        if (propagateToDependents && dependentIt != m_ResourceDependents.end())
        {
            PropagateInvalidationToDependents(name, reason);
        }

        OLO_CORE_TRACE("Invalidated resource '{0}' with reason {1} (propagate: {2})", 
                      name, static_cast<u32>(reason), propagateToDependents);
    }

    bool UniformBufferRegistry::IsBindingPointInvalidated(u32 bindingPoint) const
    {
        auto it = m_BindingPointInvalidations.find(bindingPoint);
        return it != m_BindingPointInvalidations.end() && !it->second.empty();
    }

    const InvalidationInfo* UniformBufferRegistry::GetInvalidationInfo(const std::string& name) const
    {
        auto it = m_InvalidationDetails.find(name);
        return (it != m_InvalidationDetails.end()) ? &it->second : nullptr;
    }

    void UniformBufferRegistry::AddResourceDependency(const std::string& dependentResource, const std::string& dependencyResource)
    {
        // Validate both resources exist
        if (m_ResourceBindings.find(dependentResource) == m_ResourceBindings.end())
        {
            OLO_CORE_WARN("Cannot add dependency for unknown dependent resource: '{0}'", dependentResource);
            return;
        }

        if (m_ResourceBindings.find(dependencyResource) == m_ResourceBindings.end())
        {
            OLO_CORE_WARN("Cannot add dependency for unknown dependency resource: '{0}'", dependencyResource);
            return;
        }

        // Add to dependencies map
        auto& dependencies = m_ResourceDependencies[dependentResource];
        if (std::find(dependencies.begin(), dependencies.end(), dependencyResource) == dependencies.end())
        {
            dependencies.push_back(dependencyResource);
        }

        // Add to dependents map
        auto& dependents = m_ResourceDependents[dependencyResource];
        if (std::find(dependents.begin(), dependents.end(), dependentResource) == dependents.end())
        {
            dependents.push_back(dependentResource);
        }

        // Validate for cycles
        if (!ValidateDependencyGraph())
        {
            OLO_CORE_ERROR("Dependency cycle detected after adding dependency '{0}' -> '{1}'", 
                          dependentResource, dependencyResource);
            // Remove the problematic dependency
            RemoveResourceDependency(dependentResource, dependencyResource);
            return;
        }

        OLO_CORE_TRACE("Added dependency: '{0}' depends on '{1}'", dependentResource, dependencyResource);
    }

    void UniformBufferRegistry::RemoveResourceDependency(const std::string& dependentResource, const std::string& dependencyResource)
    {
        // Remove from dependencies map
        auto depIt = m_ResourceDependencies.find(dependentResource);
        if (depIt != m_ResourceDependencies.end())
        {
            auto& dependencies = depIt->second;
            dependencies.erase(std::remove(dependencies.begin(), dependencies.end(), dependencyResource), dependencies.end());
            
            if (dependencies.empty())
            {
                m_ResourceDependencies.erase(depIt);
            }
        }

        // Remove from dependents map
        auto dependentIt = m_ResourceDependents.find(dependencyResource);
        if (dependentIt != m_ResourceDependents.end())
        {
            auto& dependents = dependentIt->second;
            dependents.erase(std::remove(dependents.begin(), dependents.end(), dependentResource), dependents.end());
            
            if (dependents.empty())
            {
                m_ResourceDependents.erase(dependentIt);
            }
        }

        OLO_CORE_TRACE("Removed dependency: '{0}' no longer depends on '{1}'", dependentResource, dependencyResource);
    }

    std::vector<std::string> UniformBufferRegistry::GetResourceDependents(const std::string& name) const
    {
        auto it = m_ResourceDependents.find(name);
        return (it != m_ResourceDependents.end()) ? it->second : std::vector<std::string>{};
    }

    std::vector<std::string> UniformBufferRegistry::GetResourceDependencies(const std::string& name) const
    {
        auto it = m_ResourceDependencies.find(name);
        return (it != m_ResourceDependencies.end()) ? it->second : std::vector<std::string>{};
    }

    // Phase 4.2: Batch Update Optimization Implementation

    void UniformBufferRegistry::ScheduleBatchUpdates(u32 maxBatchSize, UpdatePriority priorityThreshold)
    {
        if (m_InvalidatedResources.empty())
        {
            return; // Nothing to schedule
        }

        // Clear existing batches
        m_UpdateBatches.clear();

        // Create batches from invalidated resources
        CreateUpdateBatches();

        // Apply size limits if specified
        if (maxBatchSize > 0)
        {
            std::vector<UpdateBatch> resizedBatches;
            
            for (auto& batch : m_UpdateBatches)
            {
                if (batch.ResourceNames.size() <= maxBatchSize)
                {
                    resizedBatches.push_back(std::move(batch));
                }
                else
                {
                    // Split large batches
                    for (size_t i = 0; i < batch.ResourceNames.size(); i += maxBatchSize)
                    {
                        UpdateBatch subBatch(batch.ResourceType, batch.Priority);
                        size_t endIdx = std::min(i + maxBatchSize, batch.ResourceNames.size());
                        
                        subBatch.ResourceNames.assign(
                            batch.ResourceNames.begin() + i,
                            batch.ResourceNames.begin() + endIdx);
                        
                        subBatch.FrameScheduled = batch.FrameScheduled;
                        subBatch.EstimatedCost = CalculateBatchCost(subBatch);
                        
                        resizedBatches.push_back(std::move(subBatch));
                    }
                }
            }
            
            m_UpdateBatches = std::move(resizedBatches);
        }

        // Filter by priority threshold
        m_UpdateBatches.erase(
            std::remove_if(m_UpdateBatches.begin(), m_UpdateBatches.end(),
                [priorityThreshold](const UpdateBatch& batch) {
                    return batch.Priority > priorityThreshold;
                }),
            m_UpdateBatches.end());

        // Sort batches for optimal processing
        SortUpdateBatches();

        OLO_CORE_TRACE("Scheduled {0} update batches (maxSize: {1}, priorityThreshold: {2})", 
                      m_UpdateBatches.size(), maxBatchSize, static_cast<u32>(priorityThreshold));
    }

    u32 UniformBufferRegistry::ProcessUpdateBatches(u32 frameNumber)
    {
        m_CurrentFrame = frameNumber;
        u32 processedCount = 0;

        auto startTime = std::chrono::high_resolution_clock::now();

        for (auto& batch : m_UpdateBatches)
        {
            if (!batch.IsProcessed && ShouldProcessBatch(batch, frameNumber))
            {
                if (ProcessUpdateBatch(batch))
                {
                    batch.IsProcessed = true;
                    processedCount++;
                    m_UpdateStats.BatchedUpdates += static_cast<u32>(batch.ResourceNames.size());
                }
            }
        }

        // Remove processed batches
        m_UpdateBatches.erase(
            std::remove_if(m_UpdateBatches.begin(), m_UpdateBatches.end(),
                [](const UpdateBatch& batch) { return batch.IsProcessed; }),
            m_UpdateBatches.end());

        // Update statistics
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
        
        m_UpdateStats.TotalUpdates += processedCount;
        if (processedCount > 0)
        {
            f32 currentUpdateTime = duration.count() / 1000.0f; // Convert to milliseconds
            m_UpdateStats.AverageUpdateTime = (m_UpdateStats.AverageUpdateTime + currentUpdateTime) / 2.0f;
        }

        m_LastBatchFrame = frameNumber;

        OLO_CORE_TRACE("Processed {0} update batches in frame {1} ({2:.2f}ms)", 
                      processedCount, frameNumber, duration.count() / 1000.0f);

        return processedCount;
    }

    void UniformBufferRegistry::SetResourceTypePriority(ShaderResourceType resourceType, UpdatePriority priority)
    {
        m_ResourceTypePriorities[resourceType] = priority;
        OLO_CORE_TRACE("Set priority {0} for resource type {1}", 
                      static_cast<u32>(priority), static_cast<u32>(resourceType));
    }

    void UniformBufferRegistry::SetResourcePriority(const std::string& name, UpdatePriority priority)
    {
        if (m_ResourceBindings.find(name) == m_ResourceBindings.end())
        {
            OLO_CORE_WARN("Cannot set priority for unknown resource: '{0}'", name);
            return;
        }

        m_ResourcePriorities[name] = priority;
        OLO_CORE_TRACE("Set priority {0} for resource '{1}'", static_cast<u32>(priority), name);
    }

    void UniformBufferRegistry::EnableFrameBasedBatching(bool enabled, u32 maxFrameDelay)
    {
        m_FrameBasedBatchingEnabled = enabled;
        m_MaxFrameDelay = maxFrameDelay;

        OLO_CORE_TRACE("Frame-based batching {0} (maxDelay: {1} frames)", 
                      enabled ? "enabled" : "disabled", maxFrameDelay);
    }

    void UniformBufferRegistry::FlushAllUpdates()
    {
        auto startTime = std::chrono::high_resolution_clock::now();
        
        // Process all pending updates immediately
        u32 immediateUpdates = 0;
        
        // Process any existing batches
        for (auto& batch : m_UpdateBatches)
        {
            if (!batch.IsProcessed)
            {
                ProcessUpdateBatch(batch);
                batch.IsProcessed = true;
                immediateUpdates += static_cast<u32>(batch.ResourceNames.size());
            }
        }

        // Clear all batches
        m_UpdateBatches.clear();

        // Commit any remaining pending updates
        CommitPendingUpdates();

        // Update statistics
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
        
        m_UpdateStats.ImmediateUpdates += immediateUpdates;
        m_UpdateStats.TotalUpdates += immediateUpdates;

        OLO_CORE_TRACE("Flushed all updates ({0} resources, {1:.2f}ms)", 
                      immediateUpdates, duration.count() / 1000.0f);
    }

    // ==========================================
    // Phase 4 Private Implementation Methods
    // ==========================================

    void UniformBufferRegistry::PropagateInvalidationToDependents(const std::string& resourceName, InvalidationReason reason)
    {
        auto dependentIt = m_ResourceDependents.find(resourceName);
        if (dependentIt == m_ResourceDependents.end())
        {
            return; // No dependents
        }

        // Create derived reason for dependent invalidations
        InvalidationReason dependentReason = InvalidationReason::DependencyChanged;

        for (const std::string& dependent : dependentIt->second)
        {
            // Avoid infinite recursion by checking if already invalidated
            if (m_InvalidatedResources.find(dependent) == m_InvalidatedResources.end())
            {
                InvalidateResourceWithReason(dependent, dependentReason, false); // Don't propagate further to avoid cycles
            }
        }

        OLO_CORE_TRACE("Propagated invalidation from '{0}' (reason: {1}) to {2} dependents", 
                      resourceName, static_cast<u32>(reason), dependentIt->second.size());
    }

    void UniformBufferRegistry::UpdateBindingPointInvalidation(u32 bindingPoint, const std::string& resourceName, bool add)
    {
        auto& invalidatedResources = m_BindingPointInvalidations[bindingPoint];
        
        if (add)
        {
            if (std::find(invalidatedResources.begin(), invalidatedResources.end(), resourceName) == invalidatedResources.end())
            {
                invalidatedResources.push_back(resourceName);
            }
        }
        else
        {
            invalidatedResources.erase(
                std::remove(invalidatedResources.begin(), invalidatedResources.end(), resourceName),
                invalidatedResources.end());
            
            if (invalidatedResources.empty())
            {
                m_BindingPointInvalidations.erase(bindingPoint);
            }
        }
    }

    bool UniformBufferRegistry::ValidateDependencyGraph() const
    {
        // Simple cycle detection using DFS
        std::unordered_set<std::string> visiting;
        std::unordered_set<std::string> visited;
        
        std::function<bool(const std::string&)> hasCycle = [&](const std::string& resource) -> bool {
            if (visiting.find(resource) != visiting.end())
            {
                return true; // Cycle detected
            }
            
            if (visited.find(resource) != visited.end())
            {
                return false; // Already processed
            }
            
            visiting.insert(resource);
            
            auto dependenciesIt = m_ResourceDependencies.find(resource);
            if (dependenciesIt != m_ResourceDependencies.end())
            {
                for (const std::string& dependency : dependenciesIt->second)
                {
                    if (hasCycle(dependency))
                    {
                        return true;
                    }
                }
            }
            
            visiting.erase(resource);
            visited.insert(resource);
            return false;
        };
        
        // Check all resources for cycles
        for (const auto& [resourceName, _] : m_ResourceBindings)
        {
            if (visited.find(resourceName) == visited.end())
            {
                if (hasCycle(resourceName))
                {
                    return false;
                }
            }
        }
        
        return true;
    }

    void UniformBufferRegistry::CreateUpdateBatches()
    {
        // Group invalidated resources by type
        std::unordered_map<ShaderResourceType, std::vector<std::string>> resourcesByType;
        
        for (const std::string& resourceName : m_InvalidatedResources)
        {
            auto bindingIt = m_ResourceBindings.find(resourceName);
            if (bindingIt != m_ResourceBindings.end())
            {
                resourcesByType[bindingIt->second.Type].push_back(resourceName);
            }
        }

        // Create batches for each resource type
        for (auto& [resourceType, resources] : resourcesByType)
        {
            if (resources.empty()) continue;

            UpdatePriority priority = DetermineUpdatePriority(resources[0], resourceType);
            UpdateBatch batch(resourceType, priority);
            batch.ResourceNames = std::move(resources);
            batch.FrameScheduled = m_CurrentFrame;
            batch.EstimatedCost = CalculateBatchCost(batch);

            m_UpdateBatches.push_back(std::move(batch));
        }
    }

    UpdatePriority UniformBufferRegistry::DetermineUpdatePriority(const std::string& resourceName, ShaderResourceType resourceType) const
    {
        // Check for resource-specific priority first
        auto resourcePriorityIt = m_ResourcePriorities.find(resourceName);
        if (resourcePriorityIt != m_ResourcePriorities.end())
        {
            return resourcePriorityIt->second;
        }

        // Check for resource type priority
        auto typePriorityIt = m_ResourceTypePriorities.find(resourceType);
        if (typePriorityIt != m_ResourceTypePriorities.end())
        {
            return typePriorityIt->second;
        }

        // Default priority based on resource type characteristics
        switch (resourceType)
        {
            case ShaderResourceType::UniformBuffer:
                return UpdatePriority::High;    // Uniforms are usually small and frequently updated
            case ShaderResourceType::Texture2D:
            case ShaderResourceType::TextureCube:
                return UpdatePriority::Normal;  // Textures are moderate cost
            case ShaderResourceType::StorageBuffer:
                return UpdatePriority::Low;     // Storage buffers can be large
            case ShaderResourceType::Image2D:
                return UpdatePriority::Low;     // Images have higher binding cost
            default:
                return UpdatePriority::Normal;
        }
    }

    u32 UniformBufferRegistry::CalculateBatchCost(const UpdateBatch& batch) const
    {
        u32 cost = 0;
        
        // Base cost per resource type
        u32 baseCost = 1;
        switch (batch.ResourceType)
        {
            case ShaderResourceType::UniformBuffer:
                baseCost = 1;
                break;
            case ShaderResourceType::StorageBuffer:
                baseCost = 2;
                break;
            case ShaderResourceType::Texture2D:
            case ShaderResourceType::TextureCube:
                baseCost = 3;
                break;
            case ShaderResourceType::Image2D:
                baseCost = 4;
                break;
            default:
                baseCost = 2;
                break;
        }
        
        // Cost scales with number of resources but with diminishing returns
        cost = baseCost * static_cast<u32>(batch.ResourceNames.size());
        
        // Priority affects cost (higher priority = lower perceived cost)
        switch (batch.Priority)
        {
            case UpdatePriority::Immediate:
                cost = cost / 4;
                break;
            case UpdatePriority::High:
                cost = cost / 2;
                break;
            case UpdatePriority::Normal:
                // No change
                break;
            case UpdatePriority::Low:
                cost = cost * 2;
                break;
            case UpdatePriority::Background:
                cost = cost * 4;
                break;
        }
        
        return cost;
    }

    void UniformBufferRegistry::SortUpdateBatches()
    {
        std::sort(m_UpdateBatches.begin(), m_UpdateBatches.end(),
            [](const UpdateBatch& a, const UpdateBatch& b) {
                // Sort by priority first (lower value = higher priority)
                if (a.Priority != b.Priority)
                {
                    return a.Priority < b.Priority;
                }
                
                // Then by estimated cost (lower cost first)
                return a.EstimatedCost < b.EstimatedCost;
            });
    }

    bool UniformBufferRegistry::ProcessUpdateBatch(UpdateBatch& batch)
    {
        auto startTime = std::chrono::high_resolution_clock::now();
        
        for (const std::string& resourceName : batch.ResourceNames)
        {
            // Find the resource in pending updates
            auto pendingIt = m_PendingResources.find(resourceName);
            if (pendingIt != m_PendingResources.end())
            {
                auto bindingIt = m_ResourceBindings.find(resourceName);
                if (bindingIt != m_ResourceBindings.end())
                {
                    // Apply the resource binding
                    ApplyResourceBinding(resourceName, bindingIt->second, pendingIt->second);

                    // Update frame tracking
                    bindingIt->second.UpdateBindFrame(m_CurrentFrame);

                    // Move from pending to bound
                    m_BoundResources[resourceName] = std::move(pendingIt->second);
                    m_PendingResources.erase(pendingIt);
                    
                    // Remove from invalidated set
                    m_InvalidatedResources.erase(resourceName);

                    // Remove from invalidation details
                    auto invalidationIt = m_InvalidationDetails.find(resourceName);
                    if (invalidationIt != m_InvalidationDetails.end())
                    {
                        UpdateBindingPointInvalidation(invalidationIt->second.BindingPoint, resourceName, false);
                        m_InvalidationDetails.erase(invalidationIt);
                    }
                }
            }
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
        
        // Update batch processing statistics
        f32 batchTime = duration.count() / 1000.0f; // Convert to milliseconds
        m_UpdateStats.AverageBatchSize = (m_UpdateStats.AverageBatchSize + static_cast<f32>(batch.ResourceNames.size())) / 2.0f;
        
        // Estimate state change savings (batching similar resources together)
        if (batch.ResourceNames.size() > 1)
        {
            m_UpdateStats.StateChangeSavings += static_cast<u32>(batch.ResourceNames.size() - 1);
        }

        OLO_CORE_TRACE("Processed update batch with {0} resources of type {1} ({2:.2f}ms)", 
                      batch.ResourceNames.size(), static_cast<u32>(batch.ResourceType), batchTime);

        return true;
    }

    bool UniformBufferRegistry::ShouldProcessBatch(const UpdateBatch& batch, u32 currentFrame) const
    {
        // Always process immediate priority batches
        if (batch.Priority == UpdatePriority::Immediate)
        {
            return true;
        }

        // If frame-based batching is disabled, process all batches
        if (!m_FrameBasedBatchingEnabled)
        {
            return true;
        }

        // Check if enough frames have passed based on priority
        u32 framesSinceScheduled = currentFrame - batch.FrameScheduled;
        
        switch (batch.Priority)
        {
            case UpdatePriority::High:
                return framesSinceScheduled >= 1; // Process next frame
            case UpdatePriority::Normal:
                return framesSinceScheduled >= 2; // Process after 2 frames
            case UpdatePriority::Low:
                return framesSinceScheduled >= m_MaxFrameDelay; // Process after max delay
            case UpdatePriority::Background:
                return framesSinceScheduled >= m_MaxFrameDelay * 2; // Process after extended delay
            default:
                return true;
        }
    }

    // ==========================================
    // Phase 5: Enhanced Debug and Validation System Implementation
    // ==========================================

    // Phase 5.1: Resource Declaration System
    const ResourceDeclaration* UniformBufferRegistry::GetResourceDeclaration(const std::string& name) const
    {
        auto it = m_ResourceDeclarations.find(name);
        return (it != m_ResourceDeclarations.end()) ? &it->second : nullptr;
    }

    const std::unordered_map<std::string, ResourceDeclaration>& UniformBufferRegistry::GetResourceDeclarations() const
    {
        return m_ResourceDeclarations;
    }

    void UniformBufferRegistry::UpdateResourceUsageStatistics(const std::string& name, bool wasRead, bool wasWritten, u64 bytesTransferred)
    {
        auto it = m_ResourceDeclarations.find(name);
        if (it == m_ResourceDeclarations.end())
        {
            // Initialize resource declaration if it doesn't exist
            auto resourceIt = m_ResourceBindings.find(name);
            if (resourceIt != m_ResourceBindings.end())
            {
                InitializeResourceDeclaration(name, resourceIt->second.Type);
                it = m_ResourceDeclarations.find(name);
            }
            else
            {
                return; // Resource doesn't exist
            }
        }

        auto& stats = it->second.Statistics;
        auto now = std::chrono::steady_clock::now();

        if (wasRead)
        {
            stats.ReadCount++;
            stats.LastAccessed = now;
        }

        if (wasWritten)
        {
            stats.WriteCount++;
            stats.LastAccessed = now;
        }

        stats.TotalSizeTransferred += bytesTransferred;
        
        // Set first access time if this is the first access
        if (stats.ReadCount == 1 && stats.WriteCount == 0)
        {
            stats.FirstAccessed = now;
        }
        else if (stats.WriteCount == 1 && stats.ReadCount == 0)
        {
            stats.FirstAccessed = now;
        }

        // Detect usage pattern based on access frequency
        auto timeSinceFirstAccess = std::chrono::duration_cast<std::chrono::milliseconds>(now - stats.FirstAccessed).count();
        if (timeSinceFirstAccess > 1000) // Only analyze after 1 second
        {
            f32 writeFrequency = stats.WriteCount / (timeSinceFirstAccess / 1000.0f);
            
            if (writeFrequency > 60.0f) // More than 60 writes per second
            {
                it->second.UsagePattern = ResourceUsagePattern::Streaming;
            }
            else if (writeFrequency > 10.0f) // More than 10 writes per second
            {
                it->second.UsagePattern = ResourceUsagePattern::DynamicWrite;
            }
            else if (stats.WriteCount > 0)
            {
                it->second.UsagePattern = ResourceUsagePattern::StaticWrite;
            }
            else
            {
                it->second.UsagePattern = ResourceUsagePattern::ReadOnly;
            }
        }

        // Real-time validation if enabled
        if (m_RealtimeValidationEnabled)
        {
            auto issues = ValidateResourceSizeAlignment(name);
            m_ValidationIssues.insert(m_ValidationIssues.end(), issues.begin(), issues.end());
        }
    }

    void UniformBufferRegistry::SetResourceUsagePattern(const std::string& name, ResourceUsagePattern pattern)
    {
        auto it = m_ResourceDeclarations.find(name);
        if (it == m_ResourceDeclarations.end())
        {
            auto resourceIt = m_ResourceBindings.find(name);
            if (resourceIt != m_ResourceBindings.end())
            {
                InitializeResourceDeclaration(name, resourceIt->second.Type);
                it = m_ResourceDeclarations.find(name);
            }
            else
            {
                if (m_RealtimeValidationEnabled)
                {                m_ValidationIssues.push_back(CreateValidationIssue(
                    RegistryValidationSeverity::Warning,
                    "ResourceDeclaration",
                    "Attempted to set usage pattern for non-existent resource: " + name,
                    name
                ));
                }
                return;
            }
        }

        // Check for pattern consistency
        if (it->second.UsagePattern != ResourceUsagePattern::Unknown && 
            it->second.UsagePattern != pattern)
        {
            if (m_RealtimeValidationEnabled)
            {
                m_ValidationIssues.push_back(CreateValidationIssue(
                    RegistryValidationSeverity::Info,
                    "UsagePattern",
                    "Usage pattern changed for resource '" + name + "' from " + 
                    std::to_string(static_cast<i32>(it->second.UsagePattern)) + " to " + 
                    std::to_string(static_cast<i32>(pattern)),
                    name
                ));
            }
        }

        it->second.UsagePattern = pattern;
    }

    const ResourceDeclaration::UsageStatistics* UniformBufferRegistry::GetResourceUsageStatistics(const std::string& name) const
    {
        auto it = m_ResourceDeclarations.find(name);
        return (it != m_ResourceDeclarations.end()) ? &it->second.Statistics : nullptr;
    }

    std::vector<RegistryValidationIssue> UniformBufferRegistry::DetectBindingConflicts() const
    {
        std::vector<RegistryValidationIssue> issues;
        std::unordered_map<u32, std::vector<std::string>> bindingPointUsage;

        // Collect all binding point usages
        for (const auto& [name, resource] : m_ResourceBindings)
        {
            bindingPointUsage[resource.BindingPoint].push_back(name);
        }

        // Check for conflicts
        for (const auto& [bindingPoint, resources] : bindingPointUsage)
        {
            if (resources.size() > 1)
            {
                std::string resourceList;
                for (sizet i = 0; i < resources.size(); ++i)
                {
                    if (i > 0) resourceList += ", ";
                    resourceList += resources[i];
                }

                issues.push_back(CreateValidationIssue(
                    RegistryValidationSeverity::Error,
                    "BindingConflict",
                    "Binding point " + std::to_string(bindingPoint) + " is used by multiple resources: " + resourceList
                ));
            }
        }

        return issues;
    }

    // Phase 5.2: Advanced Validation
    std::vector<RegistryValidationIssue> UniformBufferRegistry::ValidateResources(bool enableLifecycleValidation, 
                                                                                  bool enableSizeValidation, 
                                                                                  bool enableConflictDetection) const
    {
        std::vector<RegistryValidationIssue> allIssues;

        if (enableConflictDetection)
        {
            auto conflictIssues = DetectBindingConflicts();
            allIssues.insert(allIssues.end(), conflictIssues.begin(), conflictIssues.end());
        }

        for (const auto& [name, resource] : m_ResourceBindings)
        {
            if (enableLifecycleValidation)
            {
                auto lifecycleIssues = ValidateResourceLifecycle(name);
                allIssues.insert(allIssues.end(), lifecycleIssues.begin(), lifecycleIssues.end());
            }

            if (enableSizeValidation)
            {
                auto sizeIssues = ValidateResourceSizeAlignment(name);
                allIssues.insert(allIssues.end(), sizeIssues.begin(), sizeIssues.end());
            }
        }

        // Filter by severity
        std::vector<RegistryValidationIssue> filteredIssues;
        for (const auto& issue : allIssues)
        {
            if (ShouldReportValidationIssue(issue))
            {
                filteredIssues.push_back(issue);
            }
        }

        return filteredIssues;
    }

    std::vector<RegistryValidationIssue> UniformBufferRegistry::ValidateResourceLifecycle(const std::string& name) const
    {
        std::vector<RegistryValidationIssue> issues;
        
        auto lifecycleIt = m_ResourceLifecycleInfo.find(name);
        if (lifecycleIt == m_ResourceLifecycleInfo.end())
        {
            issues.push_back(CreateValidationIssue(
                RegistryValidationSeverity::Warning,
                "LifecycleTracking",
                "Resource '" + name + "' has no lifecycle information",
                name
            ));
            return issues;
        }

        const auto& lifecycle = lifecycleIt->second;
        auto now = std::chrono::steady_clock::now();

        // Check for stale resources
        auto timeSinceAccess = std::chrono::duration_cast<std::chrono::seconds>(now - lifecycle.LastBound).count();
        if (timeSinceAccess > 300) // 5 minutes
        {
            issues.push_back(CreateValidationIssue(
                RegistryValidationSeverity::Info,
                "ResourceStale",
                "Resource '" + name + "' has not been accessed for " + std::to_string(timeSinceAccess) + " seconds",
                name
            ));
        }

        // Check for error states
        if (lifecycle.State == ResourceLifecycleState::Destroyed)
        {
            issues.push_back(CreateValidationIssue(
                RegistryValidationSeverity::Error,
                "ResourceError",
                "Resource '" + name + "' is in destroyed state: " + lifecycle.LastError,
                name
            ));
        }

        return issues;
    }

    std::vector<RegistryValidationIssue> UniformBufferRegistry::ValidateResourceSizeAlignment(const std::string& name) const
    {
        std::vector<RegistryValidationIssue> issues;
        
        auto resourceIt = m_ResourceBindings.find(name);
        if (resourceIt == m_ResourceBindings.end())
        {
            return issues;
        }

        const auto& resource = resourceIt->second;

        // Check uniform buffer alignment (std140 requires 16-byte alignment)
        if (resource.Type == ShaderResourceType::UniformBuffer)
        {
            if (resource.Size % 16 != 0)
            {
                issues.push_back(CreateValidationIssue(
                    RegistryValidationSeverity::Warning,
                    "Alignment",
                    "Uniform buffer '" + name + "' size (" + std::to_string(resource.Size) + 
                    " bytes) is not aligned to 16-byte boundary (std140 requirement)",
                    name
                ));
            }

            // Check for extremely large buffers
            if (resource.Size > 65536) // 64KB
            {
                issues.push_back(CreateValidationIssue(
                    RegistryValidationSeverity::Warning,
                    "Performance",
                    "Uniform buffer '" + name + "' is very large (" + std::to_string(resource.Size) + 
                    " bytes). Consider using storage buffer for better performance",
                    name
                ));
            }
        }

        // Check storage buffer alignment (typically requires 4-byte alignment)
        if (resource.Type == ShaderResourceType::StorageBuffer)
        {
            if (resource.Size % 4 != 0)
            {
                issues.push_back(CreateValidationIssue(
                    RegistryValidationSeverity::Error,
                    "Alignment",
                    "Storage buffer '" + name + "' size (" + std::to_string(resource.Size) + 
                    " bytes) is not aligned to 4-byte boundary",
                    name
                ));
            }
        }

        return issues;
    }

    const ResourceLifecycleInfo* UniformBufferRegistry::GetResourceLifecycleInfo(const std::string& name) const
    {
        auto it = m_ResourceLifecycleInfo.find(name);
        return (it != m_ResourceLifecycleInfo.end()) ? &it->second : nullptr;
    }

    void UniformBufferRegistry::UpdateResourceLifecycleState(const std::string& name, ResourceLifecycleState newState, 
                                                            const std::string& errorMessage)
    {
        auto it = m_ResourceLifecycleInfo.find(name);
        if (it == m_ResourceLifecycleInfo.end())
        {        // Initialize lifecycle info
        ResourceLifecycleInfo info{};
        info.StateChanged = std::chrono::steady_clock::now();
        info.Created = info.StateChanged;
        info.State = newState;
        info.LastError = errorMessage;
        m_ResourceLifecycleInfo[name] = info;
            return;
        }

        auto& lifecycle = it->second;
        auto oldState = lifecycle.State;

        // Validate transition
        if (m_RealtimeValidationEnabled)
        {
            auto transitionIssues = ValidateLifecycleTransition(name, oldState, newState);
            m_ValidationIssues.insert(m_ValidationIssues.end(), transitionIssues.begin(), transitionIssues.end());
        }

        lifecycle.State = newState;
        lifecycle.StateChanged = std::chrono::steady_clock::now();
        lifecycle.LastError = errorMessage;

        // Log state change if it's significant
        if (oldState != newState && (newState == ResourceLifecycleState::Destroyed))
        {
            OLO_CORE_WARN("Resource '{}' lifecycle state changed from {} to {}", 
                         name, static_cast<i32>(oldState), static_cast<i32>(newState));
        }
    }

    void UniformBufferRegistry::SetValidationSeverityFilter(RegistryValidationSeverity minSeverity)
    {
        m_ValidationSeverityFilter = minSeverity;
    }

    std::vector<RegistryValidationIssue> UniformBufferRegistry::GetValidationIssues(RegistryValidationSeverity severityFilter) const
    {
        std::vector<RegistryValidationIssue> filteredIssues;
        
        for (const auto& issue : m_ValidationIssues)
        {
            if (static_cast<i32>(issue.Severity) >= static_cast<i32>(severityFilter))
            {
                filteredIssues.push_back(issue);
            }
        }

        return filteredIssues;
    }

    void UniformBufferRegistry::ClearValidationIssues()
    {
        m_ValidationIssues.clear();
    }

    void UniformBufferRegistry::EnableRealtimeValidation(bool enabled)
    {
        m_RealtimeValidationEnabled = enabled;
        
        if (enabled)
        {
            OLO_CORE_INFO("Real-time validation enabled for UniformBufferRegistry");
        }
        else
        {
            OLO_CORE_INFO("Real-time validation disabled for UniformBufferRegistry");
        }
    }

    std::string UniformBufferRegistry::GenerateUsageReport() const
    {
        std::ostringstream report;
        report << "=== UniformBufferRegistry Usage Report ===\n";
        report << "Generated at: " << std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count() << "\n\n";

        report << "Resource Count: " << m_ResourceBindings.size() << "\n";
        report << "Validation Issues: " << m_ValidationIssues.size() << "\n";
        report << "Real-time Validation: " << (m_RealtimeValidationEnabled ? "Enabled" : "Disabled") << "\n\n";

        // Resource usage statistics
        report << "=== Resource Usage Statistics ===\n";
        for (const auto& [name, declaration] : m_ResourceDeclarations)
        {
            const auto& stats = declaration.Statistics;
            report << "Resource: " << name << "\n";
            report << "  Type: " << static_cast<i32>(declaration.Type) << "\n";
            report << "  Usage Pattern: " << static_cast<i32>(declaration.UsagePattern) << "\n";
            report << "  Read Count: " << stats.ReadCount << "\n";
            report << "  Write Count: " << stats.WriteCount << "\n";
            report << "  Bytes Transferred: " << stats.TotalSizeTransferred << "\n";
            
            auto now = std::chrono::steady_clock::now();
            auto timeSinceAccess = std::chrono::duration_cast<std::chrono::seconds>(now - stats.LastAccessed).count();
            report << "  Last Access: " << timeSinceAccess << " seconds ago\n";
            report << "\n";
        }

        // Validation issues summary
        report << "=== Validation Issues ===\n";
        std::unordered_map<RegistryValidationSeverity, u32> severityCounts;
        
        for (const auto& issue : m_ValidationIssues)
        {
            severityCounts[issue.Severity]++;
        }

        for (const auto& [severity, count] : severityCounts)
        {
            report << "  " << static_cast<i32>(severity) << ": " << count << " issues\n";
        }

        return report.str();
    }

    // Phase 5 Private Methods
    void UniformBufferRegistry::InitializeResourceDeclaration(const std::string& name, ShaderResourceType resourceType)
    {
        ResourceDeclaration declaration{};
        declaration.Name = name;
        declaration.Type = resourceType;
        declaration.UsagePattern = ResourceUsagePattern::Unknown;
        
        // Initialize usage statistics
        auto now = std::chrono::steady_clock::now();
        declaration.Statistics.FirstAccessed = now;
        declaration.Statistics.LastAccessed = now;
        declaration.Statistics.ReadCount = 0;
        declaration.Statistics.WriteCount = 0;
        declaration.Statistics.TotalSizeTransferred = 0;

        // Extract SPIR-V metadata if available
        declaration.SPIRVInfo = ExtractSPIRVMetadata(name);

        m_ResourceDeclarations[name] = declaration;

        // Initialize lifecycle info
        ResourceLifecycleInfo lifecycle{};
        lifecycle.Created = now;
        lifecycle.StateChanged = now;
        lifecycle.State = ResourceLifecycleState::Declared;
        m_ResourceLifecycleInfo[name] = lifecycle;
    }

    ResourceDeclaration::SPIRVMetadata UniformBufferRegistry::ExtractSPIRVMetadata(const std::string& name) const
    {
        ResourceDeclaration::SPIRVMetadata metadata{};
        
        // For now, return empty metadata
        // In a full implementation, this would analyze SPIR-V bytecode to extract:
        // - Decorations (layout qualifiers)
        // - Member offsets and types
        // - Array sizes and strides
        // - Usage patterns from instructions
        
        metadata.HasDecorations = false;
        
        return metadata;
    }

    std::vector<RegistryValidationIssue> UniformBufferRegistry::ValidateUsagePatternConsistency(const std::string& name, 
                                                                                              ResourceUsagePattern actualPattern,
                                                                                              ResourceUsagePattern declaredPattern) const
    {
        std::vector<RegistryValidationIssue> issues;

        if (declaredPattern == ResourceUsagePattern::Unknown)
        {
            return issues; // No declared pattern to compare against
        }

        if (actualPattern != declaredPattern)
        {
            RegistryValidationSeverity severity = RegistryValidationSeverity::Warning;
            
            // Determine severity based on pattern mismatch type
            if ((declaredPattern == ResourceUsagePattern::ReadOnly && actualPattern == ResourceUsagePattern::StaticWrite) ||
                (declaredPattern == ResourceUsagePattern::StaticWrite && actualPattern == ResourceUsagePattern::DynamicWrite))
            {
                severity = RegistryValidationSeverity::Error; // These mismatches can cause performance issues
            }

            issues.push_back(CreateValidationIssue(
                severity,
                "UsagePatternMismatch",
                "Resource '" + name + "' usage pattern mismatch. Declared: " + 
                std::to_string(static_cast<i32>(declaredPattern)) + ", Actual: " + 
                std::to_string(static_cast<i32>(actualPattern)),
                name
            ));
        }

        return issues;
    }

    RegistryValidationIssue UniformBufferRegistry::CreateValidationIssue(RegistryValidationSeverity severity, 
                                                                      const std::string& category,
                                                                      const std::string& message,
                                                                      const std::string& resourceName) const
    {
        RegistryValidationIssue issue{};
        issue.Severity = severity;
        issue.Category = category;
        issue.Message = message;
        issue.ResourceName = resourceName;
        issue.Timestamp = std::chrono::steady_clock::now();
        
        return issue;
    }

    bool UniformBufferRegistry::ShouldReportValidationIssue(const RegistryValidationIssue& issue) const
    {
        return static_cast<i32>(issue.Severity) >= static_cast<i32>(m_ValidationSeverityFilter);
    }

    std::vector<RegistryValidationIssue> UniformBufferRegistry::ValidateLifecycleTransition(const std::string& name,
                                                                                           ResourceLifecycleState fromState,
                                                                                           ResourceLifecycleState toState) const
    {
        std::vector<RegistryValidationIssue> issues;

        // Define valid transitions
        bool validTransition = false;

        switch (fromState)
        {
            case ResourceLifecycleState::Declared:
                validTransition = (toState == ResourceLifecycleState::Allocated || 
                                 toState == ResourceLifecycleState::Destroyed);
                break;
            case ResourceLifecycleState::Allocated:
                validTransition = (toState == ResourceLifecycleState::Bound ||
                                 toState == ResourceLifecycleState::Deallocated ||
                                 toState == ResourceLifecycleState::Destroyed);
                break;
            case ResourceLifecycleState::Bound:
                validTransition = (toState == ResourceLifecycleState::Active ||
                                 toState == ResourceLifecycleState::Unbound ||
                                 toState == ResourceLifecycleState::Destroyed);
                break;
            case ResourceLifecycleState::Active:
                validTransition = (toState == ResourceLifecycleState::Bound ||
                                 toState == ResourceLifecycleState::Stale ||
                                 toState == ResourceLifecycleState::Unbound ||
                                 toState == ResourceLifecycleState::Destroyed);
                break;
            case ResourceLifecycleState::Stale:
                validTransition = (toState == ResourceLifecycleState::Active ||
                                 toState == ResourceLifecycleState::Unbound ||
                                 toState == ResourceLifecycleState::Destroyed);
                break;
            case ResourceLifecycleState::Unbound:
                validTransition = (toState == ResourceLifecycleState::Bound ||
                                 toState == ResourceLifecycleState::Deallocated ||
                                 toState == ResourceLifecycleState::Destroyed);
                break;
            case ResourceLifecycleState::Deallocated:
                validTransition = (toState == ResourceLifecycleState::Destroyed);
                break;
            case ResourceLifecycleState::Destroyed:
                validTransition = false; // Cannot transition from destroyed state
                break;
            default:
                validTransition = true; // Allow unknown transitions for flexibility
                break;
        }

        if (!validTransition)
        {
            issues.push_back(CreateValidationIssue(
                RegistryValidationSeverity::Error,
                "InvalidLifecycleTransition",
                "Invalid lifecycle transition for resource '" + name + "' from " + 
                std::to_string(static_cast<i32>(fromState)) + " to " + 
                std::to_string(static_cast<i32>(toState)),
                name
            ));
        }

        return issues;
    }

    // ================================================================================================
    // Phase 6: Performance and Usability Implementation
    // ================================================================================================

    // Phase 6.1: Resource Handle Caching Implementation

    u32 UniformBufferRegistry::GetCachedHandle(const std::string& name) const
    {
        if (!m_HandleCache || !m_HandleCachingEnabled)
            return 0;

        auto* cachedHandle = m_HandleCache->GetCachedHandle(name);
        if (cachedHandle && cachedHandle->IsValid)
        {
            return cachedHandle->Handle;
        }

        // Try to cache handle from bound resource
        auto it = m_BoundResources.find(name);
        if (it != m_BoundResources.end())
        {
            u32 handle = 0;
            ShaderResourceType type = ShaderResourceType::None;
            sizet memorySize = 0;

            // Extract handle based on resource type
            if (std::holds_alternative<Ref<UniformBuffer>>(it->second))
            {
                auto buffer = std::get<Ref<UniformBuffer>>(it->second);
                if (buffer)
                {
                    handle = buffer->GetRendererID();
                    type = ShaderResourceType::UniformBuffer;
                    memorySize = buffer->GetSize();
                }
            }
            else if (std::holds_alternative<Ref<StorageBuffer>>(it->second))
            {
                auto buffer = std::get<Ref<StorageBuffer>>(it->second);
                if (buffer)
                {
                    handle = buffer->GetRendererID();
                    type = ShaderResourceType::StorageBuffer;
                    memorySize = buffer->GetSize();
                }
            }
            else if (std::holds_alternative<Ref<Texture2D>>(it->second))
            {
                auto texture = std::get<Ref<Texture2D>>(it->second);
                if (texture)
                {
                    handle = texture->GetRendererID();
                    type = ShaderResourceType::Texture2D;
                    // Texture memory size calculation would require additional API
                }
            }
            else if (std::holds_alternative<Ref<TextureCubemap>>(it->second))
            {
                auto texture = std::get<Ref<TextureCubemap>>(it->second);
                if (texture)
                {
                    handle = texture->GetRendererID();
                    type = ShaderResourceType::TextureCube;
                }
            }

            if (handle > 0)
            {
                // Cache the handle for future access
                auto* newCachedHandle = m_HandleCache->CacheHandle(name, handle, type, memorySize);
                return newCachedHandle ? newCachedHandle->Handle : handle;
            }
        }

        return 0;
    }

    void UniformBufferRegistry::InvalidateCachedHandle(const std::string& name)
    {
        if (m_HandleCache)
        {
            m_HandleCache->InvalidateHandle(name);
        }
    }

    void UniformBufferRegistry::SetHandleCachingEnabled(bool enabled)
    {
        m_HandleCachingEnabled = enabled;
        if (m_HandleCache)
        {
            m_HandleCache->SetCachingEnabled(enabled);
        }
    }

    const ShaderResourceBinding* UniformBufferRegistry::GetResourceBinding(const std::string& name) const
    {
        auto it = m_ResourceBindings.find(name);
        return (it != m_ResourceBindings.end()) ? &it->second : nullptr;
    }

    // Phase 6.2: Enhanced Template Getter Implementation (Template methods defined in header)

    // Template specializations for GetResourceEnhanced - Enhanced with OpenGL Declaration Support
    template<>
    ResourceAccessResult<UniformBuffer> UniformBufferRegistry::GetResourceEnhanced<UniformBuffer>(const std::string& name) const
    {
        return EnhancedResourceGetter::GetResource<UniformBuffer>(*this, name, m_DefaultPassName);
    }

    template<>
    ResourceAccessResult<StorageBuffer> UniformBufferRegistry::GetResourceEnhanced<StorageBuffer>(const std::string& name) const
    {
        return EnhancedResourceGetter::GetResource<StorageBuffer>(*this, name, m_DefaultPassName);
    }

    template<>
    ResourceAccessResult<Texture2D> UniformBufferRegistry::GetResourceEnhanced<Texture2D>(const std::string& name) const
    {
        return EnhancedResourceGetter::GetResource<Texture2D>(*this, name, m_DefaultPassName);
    }

    template<>
    ResourceAccessResult<TextureCubemap> UniformBufferRegistry::GetResourceEnhanced<TextureCubemap>(const std::string& name) const
    {
        return EnhancedResourceGetter::GetResource<TextureCubemap>(*this, name, m_DefaultPassName);
    }

    template<>
    ResourceAccessResult<UniformBufferArray> UniformBufferRegistry::GetResourceEnhanced<UniformBufferArray>(const std::string& name) const
    {
        return EnhancedResourceGetter::GetResource<UniformBufferArray>(*this, name, m_DefaultPassName);
    }

    template<>
    ResourceAccessResult<StorageBufferArray> UniformBufferRegistry::GetResourceEnhanced<StorageBufferArray>(const std::string& name) const
    {
        return EnhancedResourceGetter::GetResource<StorageBufferArray>(*this, name, m_DefaultPassName);
    }

    template<>
    ResourceAccessResult<Texture2DArray> UniformBufferRegistry::GetResourceEnhanced<Texture2DArray>(const std::string& name) const
    {
        return EnhancedResourceGetter::GetResource<Texture2DArray>(*this, name, m_DefaultPassName);
    }

    template<>
    ResourceAccessResult<TextureCubemapArray> UniformBufferRegistry::GetResourceEnhanced<TextureCubemapArray>(const std::string& name) const
    {
        return EnhancedResourceGetter::GetResource<TextureCubemapArray>(*this, name, m_DefaultPassName);
    }

    // Template specializations for GetResourceOrFallback - Enhanced with OpenGL Declaration Support
    template<>
    Ref<UniformBuffer> UniformBufferRegistry::GetResourceOrFallback<UniformBuffer>(const std::string& name, Ref<UniformBuffer> fallback) const
    {
        return EnhancedResourceGetter::GetResourceOrFallback<UniformBuffer>(*this, name, fallback, m_DefaultPassName);
    }

    template<>
    Ref<StorageBuffer> UniformBufferRegistry::GetResourceOrFallback<StorageBuffer>(const std::string& name, Ref<StorageBuffer> fallback) const
    {
        return EnhancedResourceGetter::GetResourceOrFallback<StorageBuffer>(*this, name, fallback, m_DefaultPassName);
    }

    template<>
    Ref<Texture2D> UniformBufferRegistry::GetResourceOrFallback<Texture2D>(const std::string& name, Ref<Texture2D> fallback) const
    {
        return EnhancedResourceGetter::GetResourceOrFallback<Texture2D>(*this, name, fallback, m_DefaultPassName);
    }

    template<>
    Ref<TextureCubemap> UniformBufferRegistry::GetResourceOrFallback<TextureCubemap>(const std::string& name, Ref<TextureCubemap> fallback) const
    {
        return EnhancedResourceGetter::GetResourceOrFallback<TextureCubemap>(*this, name, fallback, m_DefaultPassName);
    }

    // Template specializations for GetOrCreateResource - Enhanced with OpenGL Declaration Support
    template<>
    Ref<UniformBuffer> UniformBufferRegistry::GetOrCreateResource<UniformBuffer>(const std::string& name, std::function<Ref<UniformBuffer>()> factory)
    {
        return EnhancedResourceGetter::GetOrCreateResource<UniformBuffer>(*this, name, factory, m_DefaultPassName);
    }

    template<>
    Ref<StorageBuffer> UniformBufferRegistry::GetOrCreateResource<StorageBuffer>(const std::string& name, std::function<Ref<StorageBuffer>()> factory)
    {
        return EnhancedResourceGetter::GetOrCreateResource<StorageBuffer>(*this, name, factory, m_DefaultPassName);
    }

    template<>
    Ref<Texture2D> UniformBufferRegistry::GetOrCreateResource<Texture2D>(const std::string& name, std::function<Ref<Texture2D>()> factory)
    {
        return EnhancedResourceGetter::GetOrCreateResource<Texture2D>(*this, name, factory, m_DefaultPassName);
    }

    template<>
    Ref<TextureCubemap> UniformBufferRegistry::GetOrCreateResource<TextureCubemap>(const std::string& name, std::function<Ref<TextureCubemap>()> factory)
    {
        return EnhancedResourceGetter::GetOrCreateResource<TextureCubemap>(*this, name, factory, m_DefaultPassName);
    }

    // Template specializations for IsResourceReady
    template<>
    bool UniformBufferRegistry::IsResourceReady<UniformBuffer>(const std::string& name) const
    {
        return EnhancedResourceGetter::IsResourceReady<UniformBuffer>(*this, name);
    }

    template<>
    bool UniformBufferRegistry::IsResourceReady<StorageBuffer>(const std::string& name) const
    {
        return EnhancedResourceGetter::IsResourceReady<StorageBuffer>(*this, name);
    }

    template<>
    bool UniformBufferRegistry::IsResourceReady<Texture2D>(const std::string& name) const
    {
        return EnhancedResourceGetter::IsResourceReady<Texture2D>(*this, name);
    }

    template<>
    bool UniformBufferRegistry::IsResourceReady<TextureCubemap>(const std::string& name) const
    {
        return EnhancedResourceGetter::IsResourceReady<TextureCubemap>(*this, name);
    }

    // Handle pool template specializations
    template<>
    void* UniformBufferRegistry::GetHandlePool<UniformBuffer>()
    {
        return m_HandleCache ? m_HandleCache->GetHandlePool<UniformBuffer>() : nullptr;
    }

    template<>
    void* UniformBufferRegistry::GetHandlePool<StorageBuffer>()
    {
        return m_HandleCache ? m_HandleCache->GetHandlePool<StorageBuffer>() : nullptr;
    }

    template<>
    void* UniformBufferRegistry::GetHandlePool<Texture2D>()
    {
        return m_HandleCache ? m_HandleCache->GetHandlePool<Texture2D>() : nullptr;
    }

    template<>
    void* UniformBufferRegistry::GetHandlePool<TextureCubemap>()
    {
        return m_HandleCache ? m_HandleCache->GetHandlePool<TextureCubemap>() : nullptr;
    }

    template<>
    void UniformBufferRegistry::CreateHandlePool<UniformBuffer>(u32 maxSize, std::function<Ref<UniformBuffer>()> factory)
    {
        if (m_HandleCache)
        {
            m_HandleCache->CreateHandlePool<UniformBuffer>(maxSize, std::move(factory));
        }
    }

    template<>
    void UniformBufferRegistry::CreateHandlePool<StorageBuffer>(u32 maxSize, std::function<Ref<StorageBuffer>()> factory)
    {
        if (m_HandleCache)
        {
            m_HandleCache->CreateHandlePool<StorageBuffer>(maxSize, std::move(factory));
        }
    }

    template<>
    void UniformBufferRegistry::CreateHandlePool<Texture2D>(u32 maxSize, std::function<Ref<Texture2D>()> factory)
    {
        if (m_HandleCache)
        {
            m_HandleCache->CreateHandlePool<Texture2D>(maxSize, std::move(factory));
        }
    }

    template<>
    void UniformBufferRegistry::CreateHandlePool<TextureCubemap>(u32 maxSize, std::function<Ref<TextureCubemap>()> factory)
    {
        if (m_HandleCache)
        {
            m_HandleCache->CreateHandlePool<TextureCubemap>(maxSize, std::move(factory));
        }
    }

    // ==========================================
    // Step 11: DSA Integration Implementation
    // ==========================================

    bool UniformBufferRegistry::IsDSAEnabled() const
    {
        return m_DSAEnabled && m_DSABindingManager && m_DSABindingManager->IsFeatureSupported(DSABindingManager::DSAFeature::NamedBufferStorage);
    }

    u32 UniformBufferRegistry::ApplyBindingsWithDSA(bool enableBatching)
    {
        if (!IsDSAEnabled())
        {
            // Fallback to standard binding
            ApplyBindings();
            return static_cast<u32>(m_ResourceBindings.size());
        }

        // Update current frame for DSA tracking
        m_DSABindingManager->SetCurrentFrame(m_CurrentFrame);

        // Use DSA for efficient binding
        return m_DSABindingManager->ApplyRegistryBindings(*this, enableBatching);
    }

    u32 UniformBufferRegistry::ApplySetBindingsWithDSA(u32 setIndex, bool enableBatching)
    {
        if (!IsDSAEnabled())
        {
            // Fallback to standard binding
            ApplySetBindings(setIndex);
            return 0; // Standard method doesn't return count
        }

        // Get descriptor set info
        const auto* setInfo = GetDescriptorSetInfo(setIndex);
        if (!setInfo)
        {
            OLO_CORE_WARN("DSA: Descriptor set {} not found", setIndex);
            return 0;
        }

        // Update current frame for DSA tracking
        m_DSABindingManager->SetCurrentFrame(m_CurrentFrame);

        // Apply bindings for resources in this set
        u32 boundCount = 0;
        for (const auto& resourceName : setInfo->ResourceNames)
        {
            const auto* binding = GetResourceBinding(resourceName);
            if (!binding || !IsResourceBound(resourceName))
                continue;

            u32 handle = binding->GetOpenGLHandle();
            if (handle == 0)
                continue;

            bool success = false;
            switch (binding->Type)
            {
                case ShaderResourceType::UniformBuffer:
                    success = m_DSABindingManager->BindUniformBuffer(binding->BindingPoint, handle);
                    break;
                case ShaderResourceType::StorageBuffer:
                    success = m_DSABindingManager->BindStorageBuffer(binding->BindingPoint, handle);
                    break;
                case ShaderResourceType::Texture2D:
                case ShaderResourceType::TextureCube:
                    success = m_DSABindingManager->BindTexture(binding->BindingPoint, handle);
                    break;
                default:
                    break;
            }

            if (success)
                boundCount++;
        }

        return boundCount;
    }

    void UniformBufferRegistry::SetDSAEnabled(bool enabled)
    {
        m_DSAEnabled = enabled;
        if (enabled && (!m_DSABindingManager || !m_DSABindingManager->IsFeatureSupported(DSABindingManager::DSAFeature::NamedBufferStorage)))
        {
            OLO_CORE_WARN("DSA requested but not available, keeping disabled");
            m_DSAEnabled = false;
        }
    }

    DSAStatistics UniformBufferRegistry::GetDSAStatistics() const
    {
        if (m_DSABindingManager)
        {
            return m_DSABindingManager->GetStatistics();
        }
        return {};
    }

    void UniformBufferRegistry::ResetDSAStatistics()
    {
        if (m_DSABindingManager)
        {
            m_DSABindingManager->ResetStatistics();
        }
    }

    void UniformBufferRegistry::InvalidateDSABindings()
    {
        if (m_DSABindingManager)
        {
            m_DSABindingManager->InvalidateAllBindings();
        }
    }

    // ==========================================
    // Step 12: Binding State Caching Implementation
    // ==========================================

    bool UniformBufferRegistry::SetCacheEnabled(bool enabled, 
                                               BindingStateCache::CachePolicy policy,
                                               BindingStateCache::InvalidationStrategy strategy)
    {
        m_CacheEnabled = enabled;
        
        if (enabled && !m_CacheInitialized)
        {
            try
            {
                auto& cache = GetBindingStateCache();
                cache.SetCachePolicy(policy);
                cache.SetInvalidationStrategy(strategy);
                m_CacheInitialized = cache.Initialize(policy, strategy);
                
                if (m_CacheInitialized)
                {
                    OLO_CORE_INFO("Binding state cache enabled with policy: {}, strategy: {}", 
                                  static_cast<int>(policy), static_cast<int>(strategy));
                }
                else
                {
                    OLO_CORE_ERROR("Failed to initialize binding state cache");
                    m_CacheEnabled = false;
                }
            }
            catch (const std::exception& e)
            {
                OLO_CORE_ERROR("Exception initializing binding state cache: {}", e.what());
                m_CacheEnabled = false;
                m_CacheInitialized = false;
            }
        }
        else if (!enabled && m_CacheInitialized)
        {
            // Optionally shut down cache if disabling
            m_CacheInitialized = false;
        }
        
        return m_CacheEnabled && m_CacheInitialized;
    }

    u32 UniformBufferRegistry::ApplyBindingsWithCache(bool forceRebind)
    {
        if (!IsCacheEnabled())
        {
            // Fallback to standard binding without cache
            ApplyBindings();
            return 0; // ApplyBindings() doesn't return a count
        }

        try
        {
            auto& cache = GetBindingStateCache();
            return cache.ApplyRegistryBindings(*this, forceRebind);
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("Exception during cached binding application: {}", e.what());
            // Fallback to standard binding
            ApplyBindings();
            return 0; // ApplyBindings() doesn't return a count
        }
    }

    u32 UniformBufferRegistry::ApplySetBindingsWithCache(u32 setIndex, bool forceRebind)
    {
        if (!IsCacheEnabled())
        {
            // Fallback to standard set binding
            ApplySetBindings(setIndex);
            return 1; // Return 1 to indicate the set was applied
        }

        u32 appliedCount = 0;
        auto& cache = GetBindingStateCache();
        
        // Get bindings for the specific set
        for (const auto& [resourceName, binding] : m_ResourceBindings)
        {
            if (binding.Set != setIndex || !IsResourceBound(resourceName))
            {
                continue;
            }

            u32 handle = binding.GetOpenGLHandle();
            if (handle == 0)
            {
                continue;
            }

            GLenum target = GetOpenGLTargetFromType(binding.Type);
            bool shouldBind = forceRebind || 
                            !cache.IsBindingRedundant(target, binding.BindingPoint, handle, 
                                                    0, binding.Size);

            if (shouldBind)
            {
                // Apply the binding
                switch (binding.Type)
                {
                    case ShaderResourceType::UniformBuffer:
                        glBindBufferRange(GL_UNIFORM_BUFFER, binding.BindingPoint, 
                                        handle, 0, binding.Size);
                        break;
                    case ShaderResourceType::StorageBuffer:
                        glBindBufferRange(GL_SHADER_STORAGE_BUFFER, binding.BindingPoint, 
                                        handle, 0, binding.Size);
                        break;
                    case ShaderResourceType::Texture2D:
                        glActiveTexture(GL_TEXTURE0 + binding.BindingPoint);
                        glBindTexture(GL_TEXTURE_2D, handle);
                        break;
                    default:
                        continue;
                }

                // Record in cache
                cache.RecordBinding(target, binding.BindingPoint, handle, 
                                  binding.Type, 0, 
                                  binding.Size, m_CurrentFrame);
                
                appliedCount++;
            }
        }

        return appliedCount;
    }

    u32 UniformBufferRegistry::ApplyBindingsOptimal(bool enableBatching, bool forceRebind)
    {
        // Try to use both DSA and caching for maximum efficiency
        if (IsDSAEnabled() && IsCacheEnabled())
        {
            // Use DSA with cache validation
            auto& cache = GetBindingStateCache();
            u32 appliedCount = 0;
            
            // Validate cache against current state
            if (!forceRebind && !cache.ValidateCache(false))
            {
                OLO_CORE_WARN("Cache validation failed, forcing rebind");
                forceRebind = true;
            }
            
            if (enableBatching)
            {
                // Use regular DSA binding (batch methods not yet implemented)
                appliedCount = ApplyBindingsWithDSA(enableBatching);
            }
            else
            {
                appliedCount = ApplyBindingsWithDSA(enableBatching);
            }
            
            return appliedCount;
        }
        else if (IsDSAEnabled())
        {
            // Use DSA without cache
            return ApplyBindingsWithDSA(enableBatching);
        }
        else if (IsCacheEnabled())
        {
            // Use cache without DSA
            return ApplyBindingsWithCache(forceRebind);
        }
        else
        {
            // Standard binding
            ApplyBindings();
            return 0; // ApplyBindings() doesn't return a count
        }
    }

    void UniformBufferRegistry::InvalidateCache(ShaderResourceType resourceType)
    {
        if (!IsCacheEnabled())
        {
            return;
        }

        auto& cache = GetBindingStateCache();
        
        if (resourceType == ShaderResourceType::None)
        {
            cache.InvalidateAllBindings();
            OLO_CORE_TRACE("Invalidated all cached bindings for registry");
        }
        else
        {
            GLenum target = GetOpenGLTargetFromType(resourceType);
            cache.InvalidateBindingsOfType(target);
            OLO_CORE_TRACE("Invalidated cached bindings for type: {}", static_cast<int>(resourceType));
        }
    }

    void UniformBufferRegistry::SetCurrentFrame(u32 frameNumber)
    {
        m_CurrentFrame = frameNumber;
        
        if (IsCacheEnabled())
        {
            auto& cache = GetBindingStateCache();
            cache.SetCurrentFrame(frameNumber);
        }
    }

    BindingCacheStatistics UniformBufferRegistry::GetCacheStatistics() const
    {
        if (IsCacheEnabled())
        {
            auto& cache = GetBindingStateCache();
            return cache.GetStatistics();
        }
        return {};
    }

    BindingStateCache::CacheInfo UniformBufferRegistry::GetCacheInfo() const
    {
        if (IsCacheEnabled())
        {
            auto& cache = GetBindingStateCache();
            return cache.GetCacheInfo();
        }
        return {};
    }

    u32 UniformBufferRegistry::CleanupStaleCache(std::chrono::milliseconds maxAge)
    {
        if (!IsCacheEnabled())
        {
            return 0;
        }

        auto& cache = GetBindingStateCache();
        return cache.CleanupStaleBindings(maxAge);
    }

    bool UniformBufferRegistry::ValidateCache(bool fullValidation)
    {
        if (!IsCacheEnabled())
        {
            return true; // No cache to validate
        }

        auto& cache = GetBindingStateCache();
        return cache.ValidateCache(fullValidation);
    }

    void UniformBufferRegistry::SynchronizeCache()
    {
        if (IsCacheEnabled())
        {
            auto& cache = GetBindingStateCache();
            cache.SynchronizeWithOpenGL();
            OLO_CORE_INFO("Synchronized binding cache with OpenGL state");
        }
    }

    // Helper method to get OpenGL target from resource type
    GLenum UniformBufferRegistry::GetOpenGLTargetFromType(ShaderResourceType type) const
    {
        switch (type)
        {
            case ShaderResourceType::UniformBuffer:
                return GL_UNIFORM_BUFFER;
            case ShaderResourceType::StorageBuffer:
                return GL_SHADER_STORAGE_BUFFER;
            case ShaderResourceType::Texture2D:
                return GL_TEXTURE_2D;
            case ShaderResourceType::TextureCube:
                return GL_TEXTURE_CUBE_MAP;
            default:
                return GL_NONE;
        }
    }

}
