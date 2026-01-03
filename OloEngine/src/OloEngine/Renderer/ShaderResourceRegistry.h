#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/ShaderResourceTypes.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/ShaderReflection.h"
#include "OloEngine/Renderer/InflightFrameManager.h"
#include "OloEngine/Renderer/Shader.h"

#include <string>
#include <unordered_map>
#include <memory>
#include <variant>

namespace OloEngine
{

    // @brief Shader resource variant - supports all bindable resource types
    using ShaderResource = std::variant<
        std::monostate, // Represents "no resource"
        Ref<UniformBuffer>,
        Ref<Texture2D>,
        Ref<TextureCubemap>>;

    // @brief Shader resource input structure for legacy compatibility
    struct ShaderResourceInput
    {
        ShaderResourceType Type = ShaderResourceType::None;
        u32 BindingPoint = 0;
        ShaderResource Resource;

        // Legacy constructors for backward compatibility
        ShaderResourceInput() = default;
        explicit ShaderResourceInput(Ref<UniformBuffer> buffer)
            : Type(ShaderResourceType::UniformBuffer), Resource(buffer) {}
        explicit ShaderResourceInput(Ref<Texture2D> texture)
            : Type(ShaderResourceType::Texture2D), Resource(texture) {}
        explicit ShaderResourceInput(Ref<TextureCubemap> texture)
            : Type(ShaderResourceType::TextureCube), Resource(texture) {}
    };

    // @brief Resource binding information
    struct ResourceBinding
    {
        ShaderResource Resource;
        u32 BindingPoint;
        std::string Name;
        ShaderResourceType Type;
        u32 Offset = 0; // For buffers
        u32 Size = 0;   // For buffers

        bool IsValid() const;
        u32 GetHandle() const;
    };

    // @brief Shader resource registry for managing all shader resource types
    //
    // This class provides a unified system for managing uniform buffers, textures,
    // and other shader resources with SPIR-V reflection and frame-in-flight support.
    class ShaderResourceRegistry
    {
      public:
        ShaderResourceRegistry() = default;
        explicit ShaderResourceRegistry(const Ref<Shader>& shader);
        ~ShaderResourceRegistry() = default;

        // Core functionality
        void Initialize();
        void Shutdown();

        // @brief Set the associated shader for this registry
        void SetShader(const Ref<Shader>& shader)
        {
            m_Shader = shader;
        }

        // @brief Get the associated shader
        Ref<Shader> GetShader() const
        {
            return m_Shader;
        }

        // Resource discovery from reflection
        // @brief Discover resources from SPIR-V reflection data
        void DiscoverResources(u32 stage, const std::vector<u32>& spirvData, const std::string& filePath = "");

        // @brief Register all resources from reflection data
        void RegisterFromReflection(const ShaderReflection& reflection);

        // Resource management
        // @brief Set a uniform buffer
        void SetUniformBuffer(const std::string& name, Ref<UniformBuffer> buffer);

        // @brief Set a texture resource
        void SetTexture(const std::string& name, Ref<Texture2D> texture);
        void SetTexture(const std::string& name, Ref<TextureCubemap> texture);

        // @brief Generic resource setter (using variant)
        void SetResource(const std::string& name, const ShaderResource& resource);

        // @brief Set resource using input structure (legacy compatibility)
        bool SetResource(const std::string& name, const ShaderResourceInput& input);

        // @brief Template method for type-safe resource setting
        template<typename T>
        bool SetResource(const std::string& name, const Ref<T>& resource)
        {
            if constexpr (std::is_same_v<T, UniformBuffer>)
            {
                SetUniformBuffer(name, resource);
                return true;
            }
            else if constexpr (std::is_same_v<T, Texture2D>)
            {
                SetTexture(name, resource);
                return true;
            }
            else if constexpr (std::is_same_v<T, TextureCubemap>)
            {
                SetTexture(name, resource);
                return true;
            }
            else
            {
                static_assert(sizeof(T) == 0, "Unsupported resource type for ShaderResourceRegistry::SetResource");
                return false; // Unreachable, but satisfies return type
            }
        }

        // Resource retrieval
        // @brief Get uniform buffer by name
        Ref<UniformBuffer> GetUniformBuffer(const std::string& name) const;

        // @brief Get texture by name (returns as variant)
        ShaderResource GetResource(const std::string& name) const;

        // Binding operations
        // @brief Bind all registered resources
        void BindAll();

        // @brief Bind specific resource by name
        void BindResource(const std::string& name);

        // @brief Check if resource is bound
        bool IsResourceBound(const std::string& name) const;

        // Legacy compatibility methods
        // @brief Get bound resources for sharing between registries
        std::unordered_map<std::string, ShaderResource> GetBoundResources() const;

        // @brief Get binding information for a resource name
        const ResourceBinding* GetBindingInfo(const std::string& resourceName) const;

        // @brief Apply all bindings (legacy compatibility)
        void ApplyBindings()
        {
            BindAll();
        }

        // Frame-in-flight management
        // @brief Set frame-in-flight manager for multi-frame buffering
        void SetInflightFrameManager(Ref<InflightFrameManager> manager);

        // @brief Called at the beginning of each frame
        void OnFrameBegin(u32 frameIndex);

        // Validation and debug
        // @brief Validate that all required resources are bound
        bool Validate() const;

        // @brief Get binding information for a resource
        const ResourceBinding* GetBinding(const std::string& name) const;

        // @brief Get all bindings
        const std::unordered_map<std::string, ResourceBinding>& GetBindings() const
        {
            return m_Bindings;
        }

        // Standardized Binding Layout Validation
        // @brief Validate shader binding layout against standards
        bool ValidateStandardBindings() const;

        // @brief Check if UBO binding matches standard layout
        bool IsStandardUBOBinding(u32 binding, const std::string& name) const;

        // @brief Check if texture binding matches standard layout
        bool IsStandardTextureBinding(u32 binding, const std::string& name) const;

      private:
        Ref<Shader> m_Shader;
        std::unordered_map<std::string, ResourceBinding> m_Bindings;
        Ref<InflightFrameManager> m_FrameManager;
        u32 m_CurrentFrame = 0;
        bool m_Initialized = false;

        // Helper methods
        void BindUniformBuffer(const ResourceBinding& binding);
        void BindTexture(const ResourceBinding& binding);

        // GLSL source parsing fallbacks
        std::string ParseUBONameFromGLSL(u32 binding) const;
        std::string ParseUBONameFromGLSL(u32 binding, const std::string& filePath) const;
        std::string ParseTextureNameFromGLSL(u32 binding) const;
        std::string ParseTextureNameFromGLSL(u32 binding, const std::string& filePath) const;
    };
} // namespace OloEngine
