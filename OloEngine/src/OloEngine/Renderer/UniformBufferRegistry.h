#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/TextureCubemap.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <memory>

namespace OloEngine
{
    // Forward declarations
    class Shader;

    /**
     * @brief Enum defining all supported shader resource types
     */
    enum class ShaderResourceType : u8
    {
        None = 0,
        UniformBuffer,
        StorageBuffer,
        Texture2D,
        TextureCube,
        Image2D
    };

    /**
     * @brief Information about a shader resource binding discovered through SPIR-V reflection
     */
    struct ShaderResourceBinding
    {
        ShaderResourceType Type = ShaderResourceType::None;
        u32 BindingPoint = 0;       // OpenGL binding point
        u32 Set = 0;                // Vulkan descriptor set (for future compatibility)
        std::string Name;           // Resource name in shader
        sizet Size = 0;             // Size for buffers, 0 for textures
        bool IsActive = false;      // Whether this resource is currently bound

        ShaderResourceBinding() = default;
        ShaderResourceBinding(ShaderResourceType type, u32 bindingPoint, u32 set, 
                            const std::string& name, sizet size = 0)
            : Type(type), BindingPoint(bindingPoint), Set(set), Name(name), Size(size), IsActive(false)
        {}
    };

    /**
     * @brief Variant type holding different shader resource types
     */
    using ShaderResource = std::variant<
        std::monostate,          // Empty state
        Ref<UniformBuffer>,      // Uniform buffer
        Ref<Texture2D>,          // 2D texture
        Ref<TextureCubemap>      // Cubemap texture
        // Note: StorageBuffer and Image2D will be added in future updates
    >;

    /**
     * @brief Input structure for setting shader resources with type safety
     */
    struct ShaderResourceInput
    {
        ShaderResourceType Type;
        ShaderResource Resource;

        // Constructors for type-safe resource setting
        ShaderResourceInput() : Type(ShaderResourceType::None), Resource(std::monostate{}) {}

        explicit ShaderResourceInput(const Ref<UniformBuffer>& buffer)
            : Type(ShaderResourceType::UniformBuffer), Resource(buffer) {}

        explicit ShaderResourceInput(const Ref<Texture2D>& texture)
            : Type(ShaderResourceType::Texture2D), Resource(texture) {}

        explicit ShaderResourceInput(const Ref<TextureCubemap>& texture)
            : Type(ShaderResourceType::TextureCube), Resource(texture) {}
    };

    /**
     * @brief Registry for managing shader resources with automatic discovery and binding
     * 
     * This class provides a centralized system for managing shader resources similar to
     * Hazel's DescriptorSetManager but adapted for OpenGL. It automatically discovers
     * resources through SPIR-V reflection and provides type-safe resource binding.
     */
    class UniformBufferRegistry
    {
    public:
        UniformBufferRegistry() = default;
        explicit UniformBufferRegistry(const Ref<Shader>& shader);
        ~UniformBufferRegistry() = default;

        // No copy semantics - registries are tied to specific shaders
        UniformBufferRegistry(const UniformBufferRegistry&) = delete;
        UniformBufferRegistry& operator=(const UniformBufferRegistry&) = delete;

        // Move semantics
        UniformBufferRegistry(UniformBufferRegistry&&) = default;
        UniformBufferRegistry& operator=(UniformBufferRegistry&&) = default;

        /**
         * @brief Initialize the registry and discover resources from associated shader
         */
        void Initialize();

        /**
         * @brief Set the associated shader for this registry
         * @param shader Shader to associate with this registry
         */
        void SetShader(const Ref<Shader>& shader) { m_Shader = shader; }

        /**
         * @brief Shutdown the registry and clear all resources
         */
        void Shutdown();

        /**
         * @brief Discover shader resources from SPIR-V reflection data
         * @param stage Shader stage (GL_VERTEX_SHADER, GL_FRAGMENT_SHADER, etc.)
         * @param spirvData SPIR-V bytecode for reflection
         */
        void DiscoverResources(u32 stage, const std::vector<u32>& spirvData);

        /**
         * @brief Set a shader resource by name with type-safe input
         * @param name Resource name as defined in shader
         * @param input Type-safe resource input
         * @return true if resource was set successfully, false otherwise
         */
        bool SetResource(const std::string& name, const ShaderResourceInput& input);

        /**
         * @brief Template method for setting resources with automatic type deduction
         * @tparam T Resource type (UniformBuffer, Texture2D, etc.)
         * @param name Resource name as defined in shader
         * @param resource Resource to bind
         * @return true if resource was set successfully, false otherwise
         */
        template<typename T>
        bool SetResource(const std::string& name, const Ref<T>& resource)
        {
            return SetResource(name, ShaderResourceInput(resource));
        }

        /**
         * @brief Get a bound resource by name
         * @tparam T Expected resource type
         * @param name Resource name as defined in shader
         * @return Resource reference if found and type matches, nullptr otherwise
         */
        template<typename T>
        Ref<T> GetResource(const std::string& name) const
        {
            auto it = m_BoundResources.find(name);
            if (it == m_BoundResources.end())
                return nullptr;

            if (std::holds_alternative<Ref<T>>(it->second))
                return std::get<Ref<T>>(it->second);

            return nullptr;
        }

        /**
         * @brief Apply all bound resources to OpenGL state
         * This performs the actual OpenGL binding calls
         */
        void ApplyBindings();

        /**
         * @brief Validate that all required resources are bound
         * @return true if all required resources are bound, false otherwise
         */
        bool Validate() const;

        /**
         * @brief Check if a resource is bound by name
         * @param name Resource name to check
         * @return true if resource is bound, false otherwise
         */
        bool IsResourceBound(const std::string& name) const;

        /**
         * @brief Get binding information for a resource by name
         * @param name Resource name
         * @return Pointer to binding info if found, nullptr otherwise
         */
        const ShaderResourceBinding* GetBindingInfo(const std::string& name) const;

        /**
         * @brief Get all discovered resource bindings
         * @return Reference to the bindings map
         */
        const std::unordered_map<std::string, ShaderResourceBinding>& GetBindings() const { return m_ResourceBindings; }

        /**
         * @brief Get all bound resources
         * @return Reference to the bound resources map
         */
        const std::unordered_map<std::string, ShaderResource>& GetBoundResources() const { return m_BoundResources; }

        /**
         * @brief Get the associated shader
         * @return Shader reference
         */
        Ref<Shader> GetShader() const { return m_Shader; }

        /**
         * @brief Clear all bound resources but keep binding information
         */
        void ClearResources();

        /**
         * @brief Check if registry has any dirty bindings that need to be applied
         * @return true if there are dirty bindings, false otherwise
         */
        bool HasDirtyBindings() const { return !m_DirtyBindings.empty(); }

        /**
         * @brief Get statistics about the registry
         */
        struct Statistics
        {
            u32 TotalBindings = 0;
            u32 BoundResources = 0;
            u32 UniformBuffers = 0;
            u32 Textures = 0;
            u32 DirtyBindings = 0;
        };

        Statistics GetStatistics() const;

        // Debug support
        /**
         * @brief Get debug information about missing resources
         * @return Vector of names of missing required resources
         */
        std::vector<std::string> GetMissingResources() const;

        /**
         * @brief Render ImGui debug interface for this registry
         */
        void RenderDebugInterface();

    private:
        /**
         * @brief Validate resource type matches binding type
         * @param binding Binding information
         * @param input Resource input to validate
         * @return true if types match, false otherwise
         */
        bool ValidateResourceType(const ShaderResourceBinding& binding, const ShaderResourceInput& input) const;

        /**
         * @brief Apply a specific resource binding to OpenGL state
         * @param name Resource name
         * @param binding Binding information
         * @param resource Resource to bind
         */
        void ApplyResourceBinding(const std::string& name, const ShaderResourceBinding& binding, const ShaderResource& resource);

        /**
         * @brief Mark a binding as dirty for batched updates
         * @param name Resource name
         */
        void MarkBindingDirty(const std::string& name);

    private:
        // Associated shader
        Ref<Shader> m_Shader = nullptr;

        // Resource binding information discovered through reflection
        std::unordered_map<std::string, ShaderResourceBinding> m_ResourceBindings;

        // Currently bound resources
        std::unordered_map<std::string, ShaderResource> m_BoundResources;

        // Bindings that need to be applied (dirty tracking)
        std::unordered_set<std::string> m_DirtyBindings;

        // Binding point usage tracking for validation
        std::unordered_map<u32, std::string> m_BindingPointUsage;

        // Initialization state
        bool m_Initialized = false;
    };
}
