#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/UniformBufferRegistry.h"
#include "OloEngine/Renderer/ResourceStateTracker.h"
#include "OloEngine/Renderer/ResourcePool.h"

#include <functional>
#include <optional>

namespace OloEngine
{
    /**
     * @brief Fluent builder pattern for creating and configuring shader resources
     * 
     * Provides a modern, type-safe API for resource creation with method chaining.
     * Supports validation, default values, and complex configuration scenarios.
     * Integrates with the resource management system for optimal performance.
     */
    template<typename ResourceType>
    class ResourceBuilder
    {
    public:
        using FactoryFunction = std::function<Ref<ResourceType>()>;
        using ValidationFunction = std::function<bool(const Ref<ResourceType>&)>;
        using ConfigurationFunction = std::function<void(Ref<ResourceType>&)>;

        struct BuildConfiguration
        {
            std::string Name;
            ShaderResourceType Type = ShaderResourceType::UniformBuffer;
            ResourcePriority Priority = ResourcePriority::Material;
            ResourceScope Scope = ResourceScope::Scene;
            u32 Binding = 0;
            sizet Size = 0;
            BufferUsage Usage = BufferUsage::Dynamic;
            
            // Optional configurations
            std::optional<std::string> ParentResource;
            std::optional<void*> InitialData;
            std::optional<u32> ArraySize;
            std::optional<u32> BaseBindingPoint;
            
            // Pool configuration
            bool UsePooling = false;
            std::string PoolName;
            
            // State tracking
            bool EnableStateTracking = true;
            
            // Validation
            bool EnableValidation = true;
            ValidationFunction CustomValidator;
            
            // Post-creation configuration
            std::vector<ConfigurationFunction> Configurators;
        };

    private:
        BuildConfiguration m_Config;
        UniformBufferRegistry* m_Registry = nullptr;
        ResourceStateTracker* m_StateTracker = nullptr;
        std::unordered_map<std::string, void*> m_ResourcePools;
        
    public:
        explicit ResourceBuilder(const std::string& name)
        {
            m_Config.Name = name;
        }

        /**
         * @brief Set the resource registry to use
         */
        ResourceBuilder& WithRegistry(UniformBufferRegistry* registry)
        {
            m_Registry = registry;
            return *this;
        }

        /**
         * @brief Set the state tracker to use
         */
        ResourceBuilder& WithStateTracker(ResourceStateTracker* tracker)
        {
            m_StateTracker = tracker;
            return *this;
        }

        /**
         * @brief Set resource type
         */
        ResourceBuilder& OfType(ShaderResourceType type)
        {
            m_Config.Type = type;
            return *this;
        }

        /**
         * @brief Set resource priority
         */
        ResourceBuilder& WithPriority(ResourcePriority priority)
        {
            m_Config.Priority = priority;
            return *this;
        }

        /**
         * @brief Set resource scope
         */
        ResourceBuilder& WithScope(ResourceScope scope)
        {
            m_Config.Scope = scope;
            return *this;
        }

        /**
         * @brief Set binding point
         */
        ResourceBuilder& AtBinding(u32 binding)
        {
            m_Config.Binding = binding;
            return *this;
        }

        /**
         * @brief Set resource size
         */
        ResourceBuilder& WithSize(sizet size)
        {
            m_Config.Size = size;
            return *this;
        }

        /**
         * @brief Set buffer usage pattern
         */
        ResourceBuilder& WithUsage(BufferUsage usage)
        {
            m_Config.Usage = usage;
            return *this;
        }

        /**
         * @brief Set parent resource for hierarchical organization
         */
        ResourceBuilder& AsChildOf(const std::string& parentName)
        {
            m_Config.ParentResource = parentName;
            return *this;
        }

        /**
         * @brief Set initial data
         */
        template<typename T>
        ResourceBuilder& WithInitialData(const T& data)
        {
            m_Config.InitialData = const_cast<void*>(static_cast<const void*>(&data));
            m_Config.Size = sizeof(T);
            return *this;
        }

        /**
         * @brief Configure as array resource
         */
        ResourceBuilder& AsArray(u32 arraySize, u32 baseBinding)
        {
            m_Config.ArraySize = arraySize;
            m_Config.BaseBindingPoint = baseBinding;
            return *this;
        }

        /**
         * @brief Enable resource pooling
         */
        ResourceBuilder& WithPooling(const std::string& poolName = "default")
        {
            m_Config.UsePooling = true;
            m_Config.PoolName = poolName;
            return *this;
        }

        /**
         * @brief Disable state tracking
         */
        ResourceBuilder& WithoutStateTracking()
        {
            m_Config.EnableStateTracking = false;
            return *this;
        }

        /**
         * @brief Add custom validator
         */
        ResourceBuilder& WithValidator(ValidationFunction validator)
        {
            m_Config.CustomValidator = std::move(validator);
            return *this;
        }

        /**
         * @brief Disable validation
         */
        ResourceBuilder& WithoutValidation()
        {
            m_Config.EnableValidation = false;
            return *this;
        }

        /**
         * @brief Add post-creation configurator
         */
        ResourceBuilder& Configure(ConfigurationFunction configurator)
        {
            m_Config.Configurators.push_back(std::move(configurator));
            return *this;
        }

        /**
         * @brief Configure uniform buffer specific settings
         */
        template<typename T = ResourceType>
        typename std::enable_if<std::is_same_v<T, UniformBuffer>, ResourceBuilder&>::type
        AsUniformBuffer()
        {
            m_Config.Type = ShaderResourceType::UniformBuffer;
            return *this;
        }

        /**
         * @brief Configure storage buffer specific settings
         */
        template<typename T = ResourceType>
        typename std::enable_if<std::is_same_v<T, StorageBuffer>, ResourceBuilder&>::type
        AsStorageBuffer()
        {
            m_Config.Type = ShaderResourceType::StorageBuffer;
            return *this;
        }

        /**
         * @brief Configure texture specific settings
         */
        template<typename T = ResourceType>
        typename std::enable_if<std::is_same_v<T, Texture2D>, ResourceBuilder&>::type
        AsTexture2D()
        {
            m_Config.Type = ShaderResourceType::Texture2D;
            return *this;
        }

        /**
         * @brief Build the resource with current configuration
         */
        Ref<ResourceType> Build()
        {
            // Validate configuration
            if (!ValidateConfiguration())
            {
                OLO_CORE_ERROR("ResourceBuilder: Configuration validation failed for '{0}'", m_Config.Name);
                return nullptr;
            }

            // Attempt to get resource from pool if pooling is enabled
            Ref<ResourceType> resource = nullptr;
            if (m_Config.UsePooling)
            {
                resource = AcquireFromPool();
            }

            // Create new resource if not obtained from pool
            if (!resource)
            {
                resource = CreateResource();
            }

            if (!resource)
            {
                OLO_CORE_ERROR("ResourceBuilder: Failed to create resource '{0}'", m_Config.Name);
                return nullptr;
            }

            // Apply post-creation configuration
            for (const auto& configurator : m_Config.Configurators)
            {
                configurator(resource);
            }

            // Register with resource management systems
            RegisterResource(resource);

            // Apply initial data if provided
            if (m_Config.InitialData.has_value() && m_Config.Size > 0)
            {
                ApplyInitialData(resource);
            }

            OLO_CORE_TRACE("ResourceBuilder: Successfully built resource '{0}' (type: {1})", 
                          m_Config.Name, static_cast<u32>(m_Config.Type));

            return resource;
        }

        /**
         * @brief Build and register resource in one step
         */
        Ref<ResourceType> BuildAndRegister()
        {
            auto resource = Build();
            if (resource && m_Registry)
            {
                // Create appropriate input wrapper
                ShaderResourceInput input;
                input.Type = m_Config.Type;
                input.Resource = std::static_pointer_cast<void>(resource);
                
                m_Registry->SetResource(m_Config.Name, input);
            }
            return resource;
        }

        /**
         * @brief Build array resource
         */
        template<typename T = ResourceType>
        Ref<ArrayResource<T>> BuildArray()
        {
            if (!m_Config.ArraySize.has_value() || !m_Config.BaseBindingPoint.has_value())
            {
                OLO_CORE_ERROR("ResourceBuilder: Array configuration missing for '{0}'", m_Config.Name);
                return nullptr;
            }

            auto arrayResource = CreateRef<ArrayResource<T>>(m_Config.BaseBindingPoint.value(), m_Config.ArraySize.value());
            
            // Register with systems if configured
            if (m_Registry && m_Config.EnableStateTracking && m_StateTracker)
            {
                // Register each element in the array
                for (u32 i = 0; i < m_Config.ArraySize.value(); ++i)
                {
                    std::string elementName = m_Config.Name + "[" + std::to_string(i) + "]";
                    m_StateTracker->RecordAccess(elementName, m_Config.Type);
                }
            }

            OLO_CORE_TRACE("ResourceBuilder: Successfully built array resource '{0}' (size: {1})", 
                          m_Config.Name, m_Config.ArraySize.value());

            return arrayResource;
        }

    private:
        bool ValidateConfiguration() const
        {
            if (m_Config.Name.empty())
            {
                OLO_CORE_ERROR("ResourceBuilder: Resource name cannot be empty");
                return false;
            }

            if (m_Config.Size == 0 && !m_Config.ArraySize.has_value())
            {
                OLO_CORE_WARN("ResourceBuilder: Resource size is 0 for '{0}'", m_Config.Name);
            }

            // Array-specific validation
            if (m_Config.ArraySize.has_value())
            {
                if (m_Config.ArraySize.value() == 0)
                {
                    OLO_CORE_ERROR("ResourceBuilder: Array size cannot be 0 for '{0}'", m_Config.Name);
                    return false;
                }

                if (!m_Config.BaseBindingPoint.has_value())
                {
                    OLO_CORE_ERROR("ResourceBuilder: Base binding point required for array resource '{0}'", m_Config.Name);
                    return false;
                }
            }

            return true;
        }

        Ref<ResourceType> CreateResource()
        {
            // This is a simplified factory - in a real implementation, you'd have
            // type-specific creation logic based on ResourceType
            
            if constexpr (std::is_same_v<ResourceType, UniformBuffer>)
            {
                return UniformBuffer::Create(static_cast<u32>(m_Config.Size), m_Config.Binding);
            }
            else if constexpr (std::is_same_v<ResourceType, StorageBuffer>)
            {
                return StorageBuffer::Create(static_cast<u32>(m_Config.Size), m_Config.InitialData.value_or(nullptr), m_Config.Usage);
            }
            // Add other resource type creation logic here
            
            OLO_CORE_ERROR("ResourceBuilder: Unsupported resource type for '{0}'", m_Config.Name);
            return nullptr;
        }

        Ref<ResourceType> AcquireFromPool()
        {
            // Pool acquisition logic would go here
            // This is a placeholder for the actual pool integration
            return nullptr;
        }

        void RegisterResource(const Ref<ResourceType>& resource)
        {
            // Register with state tracker
            if (m_Config.EnableStateTracking && m_StateTracker)
            {
                m_StateTracker->RecordAccess(m_Config.Name, m_Config.Type);
            }

            // Register with resource hierarchy if parent is specified
            if (m_Registry && m_Config.ParentResource.has_value())
            {
                // This would integrate with the ResourceHierarchy system
                OLO_CORE_TRACE("ResourceBuilder: Registering '{0}' as child of '{1}'", 
                              m_Config.Name, m_Config.ParentResource.value());
            }
        }

        void ApplyInitialData(const Ref<ResourceType>& resource)
        {
            if constexpr (std::is_same_v<ResourceType, UniformBuffer>)
            {
                auto uniformBuffer = std::static_pointer_cast<UniformBuffer>(resource);
                uniformBuffer->SetData(m_Config.InitialData.value(), static_cast<u32>(m_Config.Size));
            }
            else if constexpr (std::is_same_v<ResourceType, StorageBuffer>)
            {
                auto storageBuffer = std::static_pointer_cast<StorageBuffer>(resource);
                storageBuffer->SetData(m_Config.InitialData.value(), static_cast<u32>(m_Config.Size));
            }
            
            // Record update in state tracker
            if (m_Config.EnableStateTracking && m_StateTracker)
            {
                m_StateTracker->RecordUpdate(m_Config.Name, m_Config.Type, m_Config.Size);
            }
        }
    };

    /**
     * @brief Convenience factory functions for common resource types
     */
    namespace ResourceBuilders
    {
        inline ResourceBuilder<UniformBuffer> UniformBuffer(const std::string& name)
        {
            return ResourceBuilder<OloEngine::UniformBuffer>(name).AsUniformBuffer();
        }

        inline ResourceBuilder<StorageBuffer> StorageBuffer(const std::string& name)
        {
            return ResourceBuilder<OloEngine::StorageBuffer>(name).AsStorageBuffer();
        }

        inline ResourceBuilder<Texture2D> Texture2D(const std::string& name)
        {
            return ResourceBuilder<OloEngine::Texture2D>(name).AsTexture2D();
        }
    }

    /**
     * @brief Example usage:
     * 
     * auto buffer = ResourceBuilders::UniformBuffer("MaterialData")
     *     .WithSize(sizeof(MaterialData))
     *     .AtBinding(2)
     *     .WithPriority(ResourcePriority::Material)
     *     .WithInitialData(materialData)
     *     .WithPooling("material_buffers")
     *     .Configure([](auto& buf) { 
     *         // Custom configuration
     *     })
     *     .BuildAndRegister();
     */
}
