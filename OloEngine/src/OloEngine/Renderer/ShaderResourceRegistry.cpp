#include "OloEnginePCH.h"
#include "ShaderResourceRegistry.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Core/Log.h"

#include <spirv_cross/spirv_cross.hpp>

namespace OloEngine
{
    ShaderResourceRegistry::ShaderResourceRegistry(const Ref<Shader>& shader)
        : m_Shader(shader)
    {
    }

    void ShaderResourceRegistry::Initialize()
    {
        if (m_Initialized)
        {
            OLO_CORE_WARN("ShaderResourceRegistry: Already initialized");
            return;
        }

        // Clear any existing bindings
        m_Bindings.clear();
        
        m_Initialized = true;
        OLO_CORE_TRACE("ShaderResourceRegistry: Initialized for shader '{0}'", 
                      m_Shader ? m_Shader->GetName() : "None");
    }

    void ShaderResourceRegistry::Shutdown()
    {
        if (!m_Initialized)
            return;

        // Clear all bindings
        m_Bindings.clear();
        
        // Reset frame manager
        m_FrameManager.reset();
        
        m_Initialized = false;
        OLO_CORE_TRACE("ShaderResourceRegistry: Shutdown complete");
    }

    void ShaderResourceRegistry::DiscoverResources([[maybe_unused]] u32 stage, const std::vector<u32>& spirvData)
    {
        try
        {
            spirv_cross::Compiler compiler(spirvData);
            spirv_cross::ShaderResources resources = compiler.get_shader_resources();

            // Discover uniform buffers
            for (const auto& resource : resources.uniform_buffers)
            {
                const auto& type = compiler.get_type(resource.type_id);
                u32 binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
                u32 bufferSize = static_cast<u32>(compiler.get_declared_struct_size(type));

                ResourceBinding resourceBinding;
                resourceBinding.Name = resource.name;
                resourceBinding.BindingPoint = binding;
                resourceBinding.Type = ShaderResourceType::UniformBuffer;
                resourceBinding.Size = bufferSize;

                m_Bindings[resource.name] = resourceBinding;
                
                OLO_CORE_TRACE("ShaderResourceRegistry: Discovered uniform buffer '{0}' at binding {1}", 
                              resource.name, binding);
            }

            // Discover textures/samplers
            for (const auto& resource : resources.sampled_images)
            {
                const auto& type = compiler.get_type(resource.type_id);
                u32 binding = compiler.get_decoration(resource.id, spv::DecorationBinding);

                ResourceBinding resourceBinding;
                
                // Try to get the name from multiple sources
                std::string name = resource.name;
                if (name.empty())
                {
                    name = compiler.get_name(resource.id);
                }
                if (name.empty())
                {
                    // Generate a fallback name based on binding point
                    name = "texture_binding_" + std::to_string(binding);
                }
                
                resourceBinding.Name = name;
                resourceBinding.BindingPoint = binding;
                
                // Determine texture type
                if (type.image.dim == spv::Dim2D)
                    resourceBinding.Type = ShaderResourceType::Texture2D;
                else if (type.image.dim == spv::DimCube)
                    resourceBinding.Type = ShaderResourceType::TextureCube;
                else
                    resourceBinding.Type = ShaderResourceType::None;

                m_Bindings[name] = resourceBinding;
                
                OLO_CORE_TRACE("ShaderResourceRegistry: Discovered texture '{0}' at binding {1}", 
                              name, binding);
            }
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("ShaderResourceRegistry: Failed to discover resources - {0}", e.what());
        }
    }

    void ShaderResourceRegistry::RegisterFromReflection(const ShaderReflection& reflection)
    {
        // Register uniform blocks
        for (const auto& block : reflection.GetUniformBlocks())
        {
            ResourceBinding binding;
            binding.Name = block.Name;
            binding.BindingPoint = block.BindingPoint;
            binding.Type = ShaderResourceType::UniformBuffer;
            binding.Size = block.Size;
            
            m_Bindings[block.Name] = binding;
        }

        // Register textures
        for (const auto& texture : reflection.GetTextures())
        {
            ResourceBinding binding;
            binding.Name = texture.Name;
            binding.BindingPoint = texture.BindingPoint;
            binding.Type = texture.Type;
            binding.Size = 0; // Textures don't have a meaningful size
            
            m_Bindings[texture.Name] = binding;
        }

        OLO_CORE_TRACE("ShaderResourceRegistry: Registered {0} uniform blocks and {1} textures from reflection",
                      reflection.GetUniformBlocks().size(), reflection.GetTextures().size());
    }

    void ShaderResourceRegistry::SetUniformBuffer(const std::string& name, Ref<UniformBuffer> buffer)
    {
        auto it = m_Bindings.find(name);
        if (it != m_Bindings.end())
        {
            it->second.Resource = buffer;
            OLO_CORE_TRACE("ShaderResourceRegistry: Set uniform buffer '{0}'", name);
        }
        else
        {
            OLO_CORE_WARN("ShaderResourceRegistry: Uniform buffer '{0}' not found in bindings", name);
        }
    }

    void ShaderResourceRegistry::SetTexture(const std::string& name, Ref<Texture2D> texture)
    {
        auto it = m_Bindings.find(name);
        if (it != m_Bindings.end())
        {
            it->second.Resource = texture;
            OLO_CORE_TRACE("ShaderResourceRegistry: Set texture2D '{0}'", name);
        }
        else
        {
            OLO_CORE_WARN("ShaderResourceRegistry: Texture '{0}' not found in bindings", name);
        }
    }

    void ShaderResourceRegistry::SetTexture(const std::string& name, Ref<TextureCubemap> texture)
    {
        auto it = m_Bindings.find(name);
        if (it != m_Bindings.end())
        {
            it->second.Resource = texture;
            OLO_CORE_TRACE("ShaderResourceRegistry: Set textureCube '{0}'", name);
        }
        else
        {
            OLO_CORE_WARN("ShaderResourceRegistry: Texture '{0}' not found in bindings", name);
        }
    }

    void ShaderResourceRegistry::SetResource(const std::string& name, const ShaderResource& resource)
    {
        auto it = m_Bindings.find(name);
        if (it != m_Bindings.end())
        {
            it->second.Resource = resource;
            OLO_CORE_TRACE("ShaderResourceRegistry: Set resource '{0}'", name);
        }
        else
        {
            OLO_CORE_WARN("ShaderResourceRegistry: Resource '{0}' not found in bindings", name);
        }
    }

    bool ShaderResourceRegistry::SetResource(const std::string& name, const ShaderResourceInput& input)
    {
        SetResource(name, input.Resource);
        return true;
    }

    Ref<UniformBuffer> ShaderResourceRegistry::GetUniformBuffer(const std::string& name) const
    {
        auto it = m_Bindings.find(name);
        if (it != m_Bindings.end())
        {
            if (std::holds_alternative<Ref<UniformBuffer>>(it->second.Resource))
            {
                return std::get<Ref<UniformBuffer>>(it->second.Resource);
            }
        }
        return nullptr;
    }

    ShaderResource ShaderResourceRegistry::GetResource(const std::string& name) const
    {
        auto it = m_Bindings.find(name);
        if (it != m_Bindings.end())
        {
            return it->second.Resource;
        }
        return ShaderResource{}; // Empty variant
    }

    void ShaderResourceRegistry::BindAll()
    {
        for (const auto& [name, binding] : m_Bindings)
        {
            if (std::holds_alternative<std::monostate>(binding.Resource))
                continue; // Skip unbound resources

            switch (binding.Type)
            {
                case ShaderResourceType::UniformBuffer:
                    BindUniformBuffer(binding);
                    break;
                case ShaderResourceType::Texture2D:
                case ShaderResourceType::TextureCube:
                    BindTexture(binding);
                    break;
                default:
                    OLO_CORE_WARN("ShaderResourceRegistry: Unsupported resource type for binding '{0}'", name);
                    break;
            }
        }
    }

    void ShaderResourceRegistry::BindResource(const std::string& name)
    {
        auto it = m_Bindings.find(name);
        if (it != m_Bindings.end())
        {
            const auto& binding = it->second;
            
            switch (binding.Type)
            {
                case ShaderResourceType::UniformBuffer:
                    BindUniformBuffer(binding);
                    break;
                case ShaderResourceType::Texture2D:
                case ShaderResourceType::TextureCube:
                    BindTexture(binding);
                    break;
                default:
                    OLO_CORE_WARN("ShaderResourceRegistry: Unsupported resource type for binding '{0}'", name);
                    break;
            }
        }
        else
        {
            OLO_CORE_WARN("ShaderResourceRegistry: Resource '{0}' not found for binding", name);
        }
    }

    bool ShaderResourceRegistry::IsResourceBound(const std::string& name) const
    {
        auto it = m_Bindings.find(name);
        return it != m_Bindings.end() && !std::holds_alternative<std::monostate>(it->second.Resource);
    }

    std::unordered_map<std::string, ShaderResource> ShaderResourceRegistry::GetBoundResources() const
    {
        std::unordered_map<std::string, ShaderResource> boundResources;
        
        for (const auto& [name, binding] : m_Bindings)
        {
            if (!std::holds_alternative<std::monostate>(binding.Resource))
            {
                boundResources[name] = binding.Resource;
            }
        }
        
        return boundResources;
    }

    const ResourceBinding* ShaderResourceRegistry::GetBindingInfo(const std::string& resourceName) const
    {
        auto it = m_Bindings.find(resourceName);
        return it != m_Bindings.end() ? &it->second : nullptr;
    }

    void ShaderResourceRegistry::SetInflightFrameManager(Ref<InflightFrameManager> manager)
    {
        m_FrameManager = manager;
    }

    void ShaderResourceRegistry::OnFrameBegin(u32 frameIndex)
    {
        m_CurrentFrame = frameIndex;
        // Future: Handle frame-in-flight specific logic here
    }

    bool ShaderResourceRegistry::Validate() const
    {
        bool allValid = true;
        
        for (const auto& [name, binding] : m_Bindings)
        {
            if (std::holds_alternative<std::monostate>(binding.Resource))
            {
                OLO_CORE_WARN("ShaderResourceRegistry: Resource '{0}' is not bound", name);
                allValid = false;
            }
        }
        
        return allValid;
    }

    const ResourceBinding* ShaderResourceRegistry::GetBinding(const std::string& name) const
    {
        auto it = m_Bindings.find(name);
        return it != m_Bindings.end() ? &it->second : nullptr;
    }

    // Helper methods
    void ShaderResourceRegistry::BindUniformBuffer(const ResourceBinding& binding)
    {
        if (std::holds_alternative<Ref<UniformBuffer>>(binding.Resource))
        {
            auto buffer = std::get<Ref<UniformBuffer>>(binding.Resource);
            if (buffer)
            {
                glBindBufferBase(GL_UNIFORM_BUFFER, binding.BindingPoint, buffer->GetRendererID());
            }
        }
    }

    void ShaderResourceRegistry::BindTexture(const ResourceBinding& binding)
    {
        if (std::holds_alternative<Ref<Texture2D>>(binding.Resource))
        {
            auto texture = std::get<Ref<Texture2D>>(binding.Resource);
            if (texture)
            {
                texture->Bind(binding.BindingPoint);
            }
        }
        else if (std::holds_alternative<Ref<TextureCubemap>>(binding.Resource))
        {
            auto texture = std::get<Ref<TextureCubemap>>(binding.Resource);
            if (texture)
            {
                texture->Bind(binding.BindingPoint);
            }
        }
    }

    // ResourceBinding helper methods
    bool ResourceBinding::IsValid() const
    {
        return !std::holds_alternative<std::monostate>(Resource);
    }

    u32 ResourceBinding::GetHandle() const
    {
        if (std::holds_alternative<Ref<UniformBuffer>>(Resource))
        {
            auto buffer = std::get<Ref<UniformBuffer>>(Resource);
            return buffer ? buffer->GetRendererID() : 0;
        }
        else if (std::holds_alternative<Ref<Texture2D>>(Resource))
        {
            auto texture = std::get<Ref<Texture2D>>(Resource);
            return texture ? texture->GetRendererID() : 0;
        }
        else if (std::holds_alternative<Ref<TextureCubemap>>(Resource))
        {
            auto texture = std::get<Ref<TextureCubemap>>(Resource);
            return texture ? texture->GetRendererID() : 0;
        }
        return 0;
    }
}
