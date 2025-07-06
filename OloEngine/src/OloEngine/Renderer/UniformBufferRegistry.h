#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/ArrayResource.h"
#include "OloEngine/Renderer/FrameInFlightManager.h"

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
        Image2D,
        // Array resource types (Phase 1.2)
        UniformBufferArray,
        StorageBufferArray,
        Texture2DArray,
        TextureCubeArray
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
        
        // Array support (Phase 1.2)
        bool IsArray = false;       // Whether this is an array resource
        u32 ArraySize = 0;          // Number of elements in the array (0 if not array)
        u32 BaseBindingPoint = 0;   // Base binding point for arrays

        ShaderResourceBinding() = default;
        ShaderResourceBinding(ShaderResourceType type, u32 bindingPoint, u32 set, 
                            const std::string& name, sizet size = 0)
            : Type(type), BindingPoint(bindingPoint), Set(set), Name(name), Size(size), IsActive(false)
        {}
        
        // Constructor for array resources
        ShaderResourceBinding(ShaderResourceType type, u32 baseBindingPoint, u32 set, 
                            const std::string& name, u32 arraySize, sizet elementSize = 0)
            : Type(type), BindingPoint(baseBindingPoint), Set(set), Name(name), 
              Size(elementSize), IsActive(false), IsArray(true), ArraySize(arraySize), 
              BaseBindingPoint(baseBindingPoint)
        {}
    };

    /**
     * @brief Variant type holding different shader resource types
     */
    using ShaderResource = std::variant<
        std::monostate,                         // Empty state
        Ref<UniformBuffer>,                     // Uniform buffer
        Ref<StorageBuffer>,                     // Storage buffer (SSBO)
        Ref<Texture2D>,                         // 2D texture
        Ref<TextureCubemap>,                    // Cubemap texture
        // Array resource types (Phase 1.2)
        Ref<UniformBufferArray>,                // Array of uniform buffers
        Ref<StorageBufferArray>,                // Array of storage buffers
        Ref<Texture2DArray>,                    // Array of 2D textures
        Ref<TextureCubemapArray>                // Array of cubemap textures
        // Note: Image2D will be added in future updates
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

        explicit ShaderResourceInput(const Ref<StorageBuffer>& buffer)
            : Type(ShaderResourceType::StorageBuffer), Resource(buffer) {}

        explicit ShaderResourceInput(const Ref<Texture2D>& texture)
            : Type(ShaderResourceType::Texture2D), Resource(texture) {}

        explicit ShaderResourceInput(const Ref<TextureCubemap>& texture)
            : Type(ShaderResourceType::TextureCube), Resource(texture) {}

        // Array resource constructors (Phase 1.2)
        explicit ShaderResourceInput(const Ref<UniformBufferArray>& array)
            : Type(ShaderResourceType::UniformBufferArray), Resource(array) {}

        explicit ShaderResourceInput(const Ref<StorageBufferArray>& array)
            : Type(ShaderResourceType::StorageBufferArray), Resource(array) {}

        explicit ShaderResourceInput(const Ref<Texture2DArray>& array)
            : Type(ShaderResourceType::Texture2DArray), Resource(array) {}

        explicit ShaderResourceInput(const Ref<TextureCubemapArray>& array)
            : Type(ShaderResourceType::TextureCubeArray), Resource(array) {}
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
         * @brief Create and bind an array resource
         * @tparam T Base resource type (StorageBuffer, Texture2D, etc.)
         * @param name Array resource name as defined in shader
         * @param baseBindingPoint Starting binding point for the array
         * @param maxSize Maximum number of resources in the array
         * @return Shared pointer to the created array resource
         */
        template<typename T>
        Ref<ArrayResource<T>> CreateArrayResource(const std::string& name, u32 baseBindingPoint, u32 maxSize = 32)
        {
            auto arrayResource = CreateRef<ArrayResource<T>>(name, baseBindingPoint, maxSize);
            
            // Create the appropriate variant type and bind it
            if constexpr (std::is_same_v<T, StorageBuffer>)
            {
                SetResource(name, ShaderResourceInput(std::static_pointer_cast<StorageBufferArray>(arrayResource)));
            }
            else if constexpr (std::is_same_v<T, UniformBuffer>)
            {
                SetResource(name, ShaderResourceInput(std::static_pointer_cast<UniformBufferArray>(arrayResource)));
            }
            else if constexpr (std::is_same_v<T, Texture2D>)
            {
                SetResource(name, ShaderResourceInput(std::static_pointer_cast<Texture2DArray>(arrayResource)));
            }
            else if constexpr (std::is_same_v<T, TextureCubemap>)
            {
                SetResource(name, ShaderResourceInput(std::static_pointer_cast<TextureCubemapArray>(arrayResource)));
            }
            
            return arrayResource;
        }

        /**
         * @brief Get an array resource by name
         * @tparam T Base resource type (StorageBuffer, Texture2D, etc.)
         * @param name Array resource name
         * @return Array resource if found and type matches, nullptr otherwise
         */
        template<typename T>
        Ref<ArrayResource<T>> GetArrayResource(const std::string& name) const
        {
            auto it = m_BoundResources.find(name);
            if (it == m_BoundResources.end())
                return nullptr;

            if constexpr (std::is_same_v<T, StorageBuffer>)
            {
                if (std::holds_alternative<Ref<StorageBufferArray>>(it->second))
                    return std::get<Ref<StorageBufferArray>>(it->second);
            }
            else if constexpr (std::is_same_v<T, UniformBuffer>)
            {
                if (std::holds_alternative<Ref<UniformBufferArray>>(it->second))
                    return std::get<Ref<UniformBufferArray>>(it->second);
            }
            else if constexpr (std::is_same_v<T, Texture2D>)
            {
                if (std::holds_alternative<Ref<Texture2DArray>>(it->second))
                    return std::get<Ref<Texture2DArray>>(it->second);
            }
            else if constexpr (std::is_same_v<T, TextureCubemap>)
            {
                if (std::holds_alternative<Ref<TextureCubemapArray>>(it->second))
                    return std::get<Ref<TextureCubemapArray>>(it->second);
            }

            return nullptr;
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

        // Frame-in-flight management (Phase 1.3)
        
        /**
         * @brief Enable frame-in-flight resource management
         * @param framesInFlight Number of frames to keep in flight (default: 3)
         */
        void EnableFrameInFlight(u32 framesInFlight = 3);

        /**
         * @brief Disable frame-in-flight resource management
         */
        void DisableFrameInFlight();

        /**
         * @brief Check if frame-in-flight is enabled
         */
        bool IsFrameInFlightEnabled() const { return m_FrameInFlightEnabled; }

        /**
         * @brief Register a resource for frame-in-flight management
         * @param name Resource name
         * @param size Resource size (for buffers)
         * @param usage Buffer usage pattern
         * @param arraySize Array size (for array resources, 0 for single resources)
         * @param baseBindingPoint Base binding point (for array resources)
         */
        void RegisterFrameInFlightResource(const std::string& name, ShaderResourceType type, u32 size = 0, 
                                         BufferUsage usage = BufferUsage::Dynamic, u32 arraySize = 0, u32 baseBindingPoint = 0);

        /**
         * @brief Advance to the next frame (call at the beginning of each frame)
         */
        void NextFrame();

        /**
         * @brief Get current frame's resource
         * @tparam T Resource type
         * @param name Resource name
         * @return Current frame's resource
         */
        template<typename T>
        Ref<T> GetCurrentFrameResource(const std::string& name) const
        {
            if (!m_FrameInFlightEnabled || !m_FrameInFlightManager)
                return GetResource<T>(name);

            if constexpr (std::is_same_v<T, UniformBuffer>)
            {
                return m_FrameInFlightManager->GetCurrentUniformBuffer(name);
            }
            else if constexpr (std::is_same_v<T, StorageBuffer>)
            {
                return m_FrameInFlightManager->GetCurrentStorageBuffer(name);
            }
            else if constexpr (std::is_same_v<T, UniformBufferArray>)
            {
                return m_FrameInFlightManager->GetCurrentUniformBufferArray(name);
            }
            else if constexpr (std::is_same_v<T, StorageBufferArray>)
            {
                return m_FrameInFlightManager->GetCurrentStorageBufferArray(name);
            }

            // Fallback to regular resource
            return GetResource<T>(name);
        }

        /**
         * @brief Get frame-in-flight manager statistics
         */
        FrameInFlightManager::Statistics GetFrameInFlightStatistics() const;

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

        // Frame-in-flight support (Phase 1.3)
        std::unique_ptr<FrameInFlightManager> m_FrameInFlightManager = nullptr;
        bool m_FrameInFlightEnabled = false;
    };
}
