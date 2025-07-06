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
        : m_Shader(shader)
    {
        OLO_CORE_ASSERT(shader, "Shader cannot be null when creating UniformBufferRegistry");
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

        m_Initialized = true;

        // Clear debug resource bindings for this shader (only if shader is available)
        if (m_Shader)
        {
            ShaderDebugger::GetInstance().ClearResourceBindings(m_Shader->GetRendererID());
        }

        OLO_CORE_TRACE("UniformBufferRegistry initialized for shader: {0}", 
                      m_Shader ? m_Shader->GetName() : "Unknown");
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

        const ShaderResourceBinding& binding = bindingIt->second;

        // Validate resource type
        if (!ValidateResourceType(binding, input))
        {
            OLO_CORE_ERROR("Resource type mismatch for '{0}'. Expected {1}, got {2}", 
                          name, static_cast<u32>(binding.Type), static_cast<u32>(input.Type));
            return false;
        }

        // Set the resource
        m_BoundResources[name] = input.Resource;
        MarkBindingDirty(name);

        OLO_CORE_TRACE("Set resource '{0}' (type={1})", name, static_cast<u32>(input.Type));
        
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
}
