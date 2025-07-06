#include "OloEnginePCH.h"
#include "UniformBufferRegistry.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Debug/ShaderDebugger.h"
#include "OloEngine/Core/Log.h"

#include <spirv_cross/spirv_cross.hpp>
#include <glad/gl.h>
#include <imgui.h>

namespace OloEngine
{
    UniformBufferRegistry::UniformBufferRegistry(const Ref<Shader>& shader)
        : m_Shader(shader), m_Specification(UniformBufferRegistrySpecification::GetPreset(RegistryConfiguration::Development))
    {
        OLO_CORE_ASSERT(shader, "Shader cannot be null when creating UniformBufferRegistry");
        ApplySpecificationSettings();
    }

    UniformBufferRegistry::UniformBufferRegistry(const Ref<Shader>& shader, const UniformBufferRegistrySpecification& spec)
        : m_Shader(shader), m_Specification(spec),
          m_UseSetPriority(spec.UseSetPriority),
          m_AutoAssignSets(spec.AutoAssignSets),
          m_StartSet(spec.StartSet),
          m_EndSet(spec.EndSet)
    {
        OLO_CORE_ASSERT(shader, "Shader cannot be null when creating UniformBufferRegistry");
        OLO_CORE_ASSERT(spec.Validate(), "Invalid registry specification provided");
        ApplySpecificationSettings();
        
        // Phase 3.1: Initialize descriptor sets if multi-set management is enabled
        if (m_UseSetPriority)
        {
            InitializeDescriptorSets();
        }
    }

    void UniformBufferRegistry::Initialize()
    {
        if (m_Initialized)
        {
            OLO_CORE_WARN("UniformBufferRegistry already initialized");
            return;
        }

        m_ResourceBindings.clear();
        m_BoundResources.clear();
        m_DirtyBindings.clear();
        m_BindingPointUsage.clear();
        
        // Phase 1.2: Clear two-phase update data structures
        m_PendingResources.clear();
        m_InvalidatedResources.clear();

        // Phase 3.1: Initialize descriptor sets if not already done
        if (m_UseSetPriority && m_DescriptorSets.empty())
        {
            InitializeDescriptorSets();
        }

        // Phase 2.2/3.2: Initialize based on specification
        if (m_Specification.EnableDefaultResources)
        {
            InitializeDefaultResources();
        }

        if (m_Specification.UseResourceTemplates && m_Specification.AutoDetectShaderPattern)
        {
            SetupResourceTemplates();
        }

        // Phase 3.1: Auto-assign resources to descriptor sets if enabled
        if (m_AutoAssignSets && !m_ResourceBindings.empty())
        {
            AutoAssignResourceSets(true);
        }

        m_Initialized = true;

        // Clear debug resource bindings for this shader (only if shader is available)
        if (m_Shader && m_Specification.EnableDebugInterface)
        {
            ShaderDebugger::GetInstance().ClearResourceBindings(m_Shader->GetRendererID());
        }

        OLO_CORE_TRACE("UniformBufferRegistry initialized for shader: {0} (spec: {1})", 
                      m_Shader ? m_Shader->GetName() : "Template", m_Specification.Name);
    }

    void UniformBufferRegistry::Shutdown()
    {
        if (!m_Initialized)
            return;

        ClearResources();
        m_ResourceBindings.clear();
        m_BindingPointUsage.clear();
        
        // Clear debug resource bindings for this shader (only if shader is available)
        if (m_Shader)
        {
            ShaderDebugger::GetInstance().ClearResourceBindings(m_Shader->GetRendererID());
        }
        
        m_Initialized = false;

        OLO_CORE_TRACE("UniformBufferRegistry shutdown for shader: {0}", 
                      m_Shader ? m_Shader->GetName() : "Unknown");
    }

    void UniformBufferRegistry::DiscoverResources(u32 stage, const std::vector<u32>& spirvData)
    {
        if (!m_Initialized)
        {
            OLO_CORE_ERROR("UniformBufferRegistry not initialized. Call Initialize() first.");
            return;
        }

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

        m_DirtyBindings.clear();
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
        // Phase 1.1: Helper lambda to extract and store GPU handle
        auto extractAndStoreGPUHandle = [](ShaderResourceBinding& binding, u32 handle) {
            binding.SetOpenGLHandle(handle);
            OLO_CORE_TRACE("Stored GPU handle {0} for resource '{1}'", handle, binding.Name);
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
                        // Phase 1.1: Store GPU handle for tracking (if accessible)
                        // extractAndStoreGPUHandle(mutableBinding, buffer->GetRendererID()); // Uncomment if method exists
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
                        // Phase 1.1: Store GPU handle for tracking (if accessible)
                        // extractAndStoreGPUHandle(mutableBinding, buffer->GetRendererID()); // Uncomment if method exists
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
                        // Phase 1.1: Store GPU handle for tracking
                        extractAndStoreGPUHandle(mutableBinding, texture->GetRendererID());
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
                        // Phase 1.1: Store GPU handle for tracking
                        extractAndStoreGPUHandle(mutableBinding, texture->GetRendererID());
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

        // Mark binding as dirty for GPU handle tracking
        bindingIt->second.MarkDirty();

        // Add to invalidated set for two-phase updates
        m_InvalidatedResources.insert(name);

        // If resource exists in bound resources, move it to pending
        auto boundIt = m_BoundResources.find(name);
        if (boundIt != m_BoundResources.end())
        {
            m_PendingResources[name] = std::move(boundIt->second);
            m_BoundResources.erase(boundIt);
        }

        OLO_CORE_TRACE("Invalidated resource: '{0}'", name);
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
}
