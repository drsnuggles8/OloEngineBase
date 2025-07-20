#include "OloEnginePCH.h"
#include "ShaderResourceRegistry.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Core/Log.h"

#include <spirv_cross/spirv_cross.hpp>
#include <regex>
#include <fstream>

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

    void ShaderResourceRegistry::DiscoverResources(u32 stage, const std::vector<u32>& spirvData, const std::string& filePath)
    {
        OLO_CORE_WARN("ShaderResourceRegistry: DiscoverResources called for stage {}", stage);
        
        try
        {
            spirv_cross::Compiler compiler(spirvData);
            spirv_cross::ShaderResources resources = compiler.get_shader_resources();

            for (const auto& resource : resources.uniform_buffers)
            {
                const auto& type = compiler.get_type(resource.type_id);
                u32 binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
                u32 bufferSize = static_cast<u32>(compiler.get_declared_struct_size(type));

                ResourceBinding resourceBinding;
                
                std::string name = resource.name;
                if (name.empty())
                {
                    name = compiler.get_name(resource.id);
                }
                
                if (name.empty() || (name.find("_") == 0 && name.length() > 1 && std::isdigit(name[1])))
                {
                    std::string glslName = ParseUBONameFromGLSL(binding, filePath);
                    if (!glslName.empty())
                    {
                        name = glslName;
                    }
                }
                
                if (name.empty())
                {
                    name = "ubo_binding_" + std::to_string(binding);
                    OLO_CORE_WARN("ShaderResourceRegistry: No name found for UBO at binding {}, using fallback '{}'", binding, name);
                }
                
                resourceBinding.Name = name;
                resourceBinding.BindingPoint = binding;
                resourceBinding.Type = ShaderResourceType::UniformBuffer;
                resourceBinding.Size = bufferSize;
                m_Bindings[name] = resourceBinding;
            }

            for (const auto& resource : resources.sampled_images)
            {
                const auto& type = compiler.get_type(resource.type_id);
                u32 binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
                ResourceBinding resourceBinding;
                
                std::string name = resource.name;
                if (name.empty())
                {
                    name = compiler.get_name(resource.id);
                }
                
                if (name.empty() || (name.find("_") == 0 && name.length() > 1 && std::isdigit(name[1])))
                {
                    std::string glslName = ParseTextureNameFromGLSL(binding, filePath);
                    if (!glslName.empty())
                    {
                        name = glslName;
                    }
                }
                
                if (name.empty())
                {
                    name = "texture_binding_" + std::to_string(binding);
                    OLO_CORE_WARN("ShaderResourceRegistry: No name found for texture at binding {}, using fallback '{}'", binding, name);
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
            }
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("ShaderResourceRegistry: Failed to discover resources - {0}", e.what());
        }
        
        if (!ValidateStandardBindings())
        {
            OLO_CORE_WARN("ShaderResourceRegistry: Shader has non-standard binding layout");
        }
    }

    void ShaderResourceRegistry::RegisterFromReflection(const ShaderReflection& reflection)
    {
        for (const auto& block : reflection.GetUniformBlocks())
        {
            ResourceBinding binding;
            binding.Name = block.Name;
            binding.BindingPoint = block.BindingPoint;
            binding.Type = ShaderResourceType::UniformBuffer;
            binding.Size = block.Size;
            
            m_Bindings[block.Name] = binding;
        }

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
        auto it = m_Bindings.find(name);
        if (it != m_Bindings.end())
        {
            it->second.Resource = input.Resource;
            return true;
        }
        else
        {
            OLO_CORE_WARN("ShaderResourceRegistry: Resource '{0}' not found in bindings", name);
            return false;
        }
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

    // Standardized Binding Layout Validation Implementation
    bool ShaderResourceRegistry::ValidateStandardBindings() const
    {
        bool isValid = true;
        
        for (const auto& [name, binding] : m_Bindings)
        {
            if (binding.Type == ShaderResourceType::UniformBuffer)
            {
                if (!IsStandardUBOBinding(binding.BindingPoint, name))
                {
                    OLO_CORE_WARN("Non-standard UBO binding: '{}' at binding {}", 
                                  name, binding.BindingPoint);
                    isValid = false;
                }
            }
            else if (binding.Type == ShaderResourceType::Texture2D || 
                     binding.Type == ShaderResourceType::TextureCube)
            {
                if (!IsStandardTextureBinding(binding.BindingPoint, name))
                {
                    OLO_CORE_WARN("Non-standard texture binding: '{}' at binding {}", 
                                  name, binding.BindingPoint);
                    isValid = false;
                }
            }
        }
        
        return isValid;
    }

    bool ShaderResourceRegistry::IsStandardUBOBinding(u32 binding, const std::string& name) const
    {
        // If the name starts with underscore and numbers, it's likely a SPIR-V generated name
        // In this case, we only check the binding point
        if (name.find("_") == 0 && name.length() > 1 && std::isdigit(name[1]))
        {
            // SPIR-V generated name - validate only by binding point
            return binding <= ShaderBindingLayout::UBO_USER_2; // Valid binding range
        }
        
        // Check if this is one of our standardized names
        switch (binding)
        {
            case ShaderBindingLayout::UBO_CAMERA:
                return name == "CameraMatrices" ||
                       name.find("Camera") != std::string::npos || 
                       name.find("camera") != std::string::npos;
                       
            case ShaderBindingLayout::UBO_LIGHTS:
                return name == "LightProperties" ||
                       name.find("Light") != std::string::npos || 
                       name.find("light") != std::string::npos;
                       
            case ShaderBindingLayout::UBO_MATERIAL:
                return name == "MaterialProperties" ||
                       name.find("Material") != std::string::npos || 
                       name.find("material") != std::string::npos;
                       
            case ShaderBindingLayout::UBO_MODEL:
                return name == "ModelMatrices" ||
                       name.find("Model") != std::string::npos || 
                       name.find("model") != std::string::npos;
                       
            case ShaderBindingLayout::UBO_ANIMATION:
                return name == "AnimationMatrices" ||
                       name.find("Animation") != std::string::npos || 
                       name.find("animation") != std::string::npos ||
                       name.find("Bone") != std::string::npos ||
                       name.find("bone") != std::string::npos;
                       
            default:
                // User-defined bindings (5-7) are always valid
                return binding >= ShaderBindingLayout::UBO_USER_0;
        }
    }

    bool ShaderResourceRegistry::IsStandardTextureBinding(u32 binding, const std::string& name) const
    {
        // If the name starts with "texture_binding_", it's our fallback name - validate only by binding
        if (name.find("texture_binding_") == 0)
        {
            return binding <= ShaderBindingLayout::TEX_USER_3; // Valid binding range
        }
        
        // Special case for 2D renderer texture arrays
        if (binding == ShaderBindingLayout::TEX_DIFFUSE && 
            (name == "u_Textures" || name.find("Textures") != std::string::npos))
        {
            return true;
        }
        
        // Check if this is one of our standardized names first
        switch (binding)
        {
            case ShaderBindingLayout::TEX_DIFFUSE:
                return name == "u_DiffuseMap" ||
                       name.find("diffuse") != std::string::npos || 
                       name.find("Diffuse") != std::string::npos ||
                       name.find("albedo") != std::string::npos ||
                       name.find("Albedo") != std::string::npos ||
                       name == "u_Texture";  // Common generic texture name
                       
            case ShaderBindingLayout::TEX_SPECULAR:
                return name == "u_SpecularMap" ||
                       name.find("specular") != std::string::npos || 
                       name.find("Specular") != std::string::npos;
                       
            case ShaderBindingLayout::TEX_NORMAL:
                return name == "u_NormalMap" ||
                       name.find("normal") != std::string::npos || 
                       name.find("Normal") != std::string::npos;
                       
            case ShaderBindingLayout::TEX_ENVIRONMENT:
                return name == "u_EnvironmentMap" ||
                       name.find("Skybox") != std::string::npos ||
                       name.find("skybox") != std::string::npos ||
                       name.find("Environment") != std::string::npos ||
                       name.find("environment") != std::string::npos ||
                       name.find("Cubemap") != std::string::npos ||
                       name == "u_Skybox";
                       
            case ShaderBindingLayout::TEX_SHADOW:
                return name == "u_ShadowMap" ||
                       name.find("Shadow") != std::string::npos ||
                       name.find("shadow") != std::string::npos ||
                       name.find("FontAtlas") != std::string::npos ||
                       name.find("font") != std::string::npos;
                       
            default:
                // User-defined texture bindings (10+) are always valid
                return binding >= ShaderBindingLayout::TEX_USER_0;
        }
    }

    // GLSL source parsing fallbacks
    std::string ShaderResourceRegistry::ParseUBONameFromGLSL(u32 binding) const
    {
        if (!m_Shader)
        {
            OLO_CORE_TRACE("ShaderResourceRegistry: No shader available for UBO binding {}", binding);
            return "";
        }

        // Try to get the original GLSL source from the shader
        // This would need to be implemented in the Shader class to expose the source
        // For now, we'll try to read from the file path if available
        std::string shaderPath = m_Shader->GetFilePath();
        if (shaderPath.empty())
        {
            OLO_CORE_TRACE("ShaderResourceRegistry: ParseUBONameFromGLSL - No shader path available");
            return "";
        }

        OLO_CORE_TRACE("ShaderResourceRegistry: ParseUBONameFromGLSL - Trying to read from path: '{}'", shaderPath);

        try
        {
            std::ifstream file(shaderPath);
            if (!file.is_open())
            {
                OLO_CORE_TRACE("ShaderResourceRegistry: ParseUBONameFromGLSL - Failed to open file: '{}'", shaderPath);
                return "";
            }

            std::string line;
            std::regex uboRegex(R"(layout\s*\(\s*std140\s*,\s*binding\s*=\s*)" + std::to_string(binding) + R"(\s*\)\s*uniform\s+(\w+))");
            std::smatch match;

            OLO_CORE_TRACE("ShaderResourceRegistry: ParseUBONameFromGLSL - Looking for binding {} with regex", binding);

            while (std::getline(file, line))
            {
                if (std::regex_search(line, match, uboRegex) && match.size() > 1)
                {
                    OLO_CORE_TRACE("ShaderResourceRegistry: ParseUBONameFromGLSL - Found match: '{}'", match[1].str());
                    return match[1].str(); // Return the UBO name
                }
            }
            
            OLO_CORE_TRACE("ShaderResourceRegistry: ParseUBONameFromGLSL - No matching UBO found for binding {}", binding);
        }
        catch (const std::exception& e)
        {
            OLO_CORE_WARN("ShaderResourceRegistry: Failed to parse GLSL source for UBO at binding {}: {}", binding, e.what());
        }

        return "";
    }

    std::string ShaderResourceRegistry::ParseTextureNameFromGLSL(u32 binding) const
    {
        if (!m_Shader)
            return "";

        std::string shaderPath = m_Shader->GetFilePath();
        if (shaderPath.empty())
            return "";

        try
        {
            std::ifstream file(shaderPath);
            if (!file.is_open())
                return "";

            std::string line;
            std::regex textureRegex(R"(layout\s*\(\s*binding\s*=\s*)" + std::to_string(binding) + R"(\s*\)\s*uniform\s+sampler\w+\s+(\w+))");
            std::smatch match;

            while (std::getline(file, line))
            {
                if (std::regex_search(line, match, textureRegex) && match.size() > 1)
                {
                    return match[1].str(); // Return the texture name
                }
            }
        }
        catch (const std::exception& e)
        {
            OLO_CORE_WARN("ShaderResourceRegistry: Failed to parse GLSL source for texture at binding {}: {}", binding, e.what());
        }

        return "";
    }

    std::string ShaderResourceRegistry::ParseUBONameFromGLSL(u32 binding, const std::string& filePath) const
    {
        if (filePath.empty())
        {
            OLO_CORE_TRACE("ShaderResourceRegistry: No file path provided for UBO binding {}", binding);
            return "";
        }

        OLO_CORE_TRACE("ShaderResourceRegistry: Parsing GLSL file for UBO at binding {}: '{}'", binding, filePath);

        try
        {
            std::ifstream file(filePath);
            if (!file.is_open())
            {
                OLO_CORE_TRACE("ShaderResourceRegistry: Failed to open file: '{}'", filePath);
                return "";
            }

            std::string line;
            std::regex uboRegex(R"(layout\s*\(\s*std140\s*,\s*binding\s*=\s*)" + std::to_string(binding) + R"(\s*\)\s*uniform\s+(\w+))");
            std::smatch match;

            OLO_CORE_TRACE("ShaderResourceRegistry: Looking for UBO at binding {}", binding);

            while (std::getline(file, line))
            {
                if (std::regex_search(line, match, uboRegex) && match.size() > 1)
                {
                    OLO_CORE_TRACE("ShaderResourceRegistry: Found UBO name '{}' at binding {}", match[1].str(), binding);
                    return match[1].str(); // Return the UBO name
                }
            }
            
            OLO_CORE_TRACE("ShaderResourceRegistry: No matching UBO found for binding {}", binding);
        }
        catch (const std::exception& e)
        {
            OLO_CORE_WARN("ShaderResourceRegistry: Failed to parse GLSL source for UBO at binding {}: {}", binding, e.what());
        }

        return "";
    }

    std::string ShaderResourceRegistry::ParseTextureNameFromGLSL(u32 binding, const std::string& filePath) const
    {
        if (filePath.empty())
        {
            OLO_CORE_TRACE("ShaderResourceRegistry: No file path provided for texture binding {}", binding);
            return "";
        }

        try
        {
            std::ifstream file(filePath);
            if (!file.is_open())
            {
                OLO_CORE_TRACE("ShaderResourceRegistry: Failed to open file: '{}'", filePath);
                return "";
            }

            std::string line;
            std::regex textureRegex(R"(layout\s*\(\s*binding\s*=\s*)" + std::to_string(binding) + R"(\s*\)\s*uniform\s+sampler\w+\s+(\w+)\s*;)");
            std::smatch match;

            while (std::getline(file, line))
            {
                if (std::regex_search(line, match, textureRegex) && match.size() > 1)
                {
                    OLO_CORE_TRACE("ShaderResourceRegistry: Found texture name '{}' at binding {}", match[1].str(), binding);
                    return match[1].str();
                }
            }
            
            OLO_CORE_TRACE("ShaderResourceRegistry: No matching texture found for binding {}", binding);
        }
        catch (const std::exception& e)
        {
            OLO_CORE_WARN("ShaderResourceRegistry: Failed to parse GLSL source for texture at binding {}: {}", binding, e.what());
        }

        return "";
    }
}
