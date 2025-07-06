#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Buffer.h"

#include <unordered_map>
#include <vector>
#include <memory>
#include <functional>
#include <queue>

namespace OloEngine
{
    /**
     * @brief Generic resource pool for efficient resource reuse and memory management
     * 
     * Provides object pooling for expensive-to-create resources like buffers and textures.
     * Reduces allocation overhead and memory fragmentation by reusing resources.
     * Supports automatic cleanup, size limits, and resource validation.
     * 
     * @tparam ResourceType The type of resources to pool (e.g., UniformBuffer, StorageBuffer)
     */
    template<typename ResourceType>
    class ResourcePool
    {
    public:
        using ResourcePtr = Ref<ResourceType>;
        using FactoryFunction = std::function<ResourcePtr()>;
        using ValidatorFunction = std::function<bool(const ResourcePtr&)>;
        using ResetFunction = std::function<void(ResourcePtr&)>;

        struct PoolConfiguration
        {
            u32 InitialSize = 4;           // Initial number of resources to pre-allocate
            u32 MaxSize = 64;              // Maximum pool size (0 = unlimited)
            u32 GrowthSize = 4;            // Number of resources to create when pool is empty
            bool EnableValidation = true;   // Whether to validate resources before reuse
            bool AutoShrink = true;        // Whether to shrink pool when resources aren't used
            f32 ShrinkThreshold = 0.25f;   // Shrink when usage drops below this ratio
        };

    private:
        PoolConfiguration m_Config;
        FactoryFunction m_Factory;
        ValidatorFunction m_Validator;
        ResetFunction m_Reset;
        
        std::vector<ResourcePtr> m_AvailableResources;
        std::vector<ResourcePtr> m_InUseResources;
        
        // Statistics
        u32 m_TotalCreated = 0;
        u32 m_TotalAcquired = 0;
        u32 m_TotalReleased = 0;
        u32 m_TotalValidationFailures = 0;
        
        mutable std::mutex m_Mutex; // Thread safety for resource pools

    public:
        explicit ResourcePool(FactoryFunction factory, const PoolConfiguration& config = {})
            : m_Config(config), m_Factory(std::move(factory))
        {
            // Pre-allocate initial resources
            for (u32 i = 0; i < m_Config.InitialSize; ++i)
            {
                if (auto resource = CreateResource())
                {
                    m_AvailableResources.push_back(resource);
                }
            }
            
            OLO_CORE_TRACE("ResourcePool created with {0} initial resources", m_AvailableResources.size());
        }

        ~ResourcePool()
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            
            OLO_CORE_TRACE("ResourcePool destroyed - Created: {0}, Acquired: {1}, Released: {2}, Validation Failures: {3}",
                          m_TotalCreated, m_TotalAcquired, m_TotalReleased, m_TotalValidationFailures);
            
            // Clear all resources
            m_AvailableResources.clear();
            m_InUseResources.clear();
        }

        /**
         * @brief Set resource validator function
         */
        void SetValidator(ValidatorFunction validator)
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Validator = std::move(validator);
        }

        /**
         * @brief Set resource reset function (called when resource is returned to pool)
         */
        void SetResetFunction(ResetFunction reset)
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Reset = std::move(reset);
        }

        /**
         * @brief Acquire a resource from the pool
         * @return Valid resource pointer or nullptr if acquisition failed
         */
        ResourcePtr Acquire()
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            
            // Try to get an available resource
            while (!m_AvailableResources.empty())
            {
                auto resource = m_AvailableResources.back();
                m_AvailableResources.pop_back();
                
                // Validate resource if validator is set
                if (m_Config.EnableValidation && m_Validator && !m_Validator(resource))
                {
                    m_TotalValidationFailures++;
                    OLO_CORE_WARN("ResourcePool: Resource failed validation, discarding");
                    continue;
                }
                
                m_InUseResources.push_back(resource);
                m_TotalAcquired++;
                return resource;
            }
            
            // No available resources, try to create new ones
            if (m_Config.MaxSize == 0 || GetTotalSize() < m_Config.MaxSize)
            {
                for (u32 i = 0; i < m_Config.GrowthSize && (m_Config.MaxSize == 0 || GetTotalSize() < m_Config.MaxSize); ++i)
                {
                    if (auto resource = CreateResource())
                    {
                        m_InUseResources.push_back(resource);
                        m_TotalAcquired++;
                        
                        if (i == 0) // Return the first one, keep the rest for future use
                        {
                            return resource;
                        }
                        else
                        {
                            // Put additional resources in available pool
                            m_AvailableResources.push_back(resource);
                            m_InUseResources.pop_back(); // Remove from in-use since we're putting it in available
                        }
                    }
                }
            }
            
            OLO_CORE_ERROR("ResourcePool: Failed to acquire resource - pool exhausted");
            return nullptr;
        }

        /**
         * @brief Release a resource back to the pool
         * @param resource Resource to release
         */
        void Release(ResourcePtr resource)
        {
            if (!resource)
            {
                OLO_CORE_WARN("ResourcePool: Attempted to release null resource");
                return;
            }
            
            std::lock_guard<std::mutex> lock(m_Mutex);
            
            // Find and remove from in-use list
            auto it = std::find(m_InUseResources.begin(), m_InUseResources.end(), resource);
            if (it == m_InUseResources.end())
            {
                OLO_CORE_WARN("ResourcePool: Attempted to release resource not acquired from this pool");
                return;
            }
            
            m_InUseResources.erase(it);
            
            // Reset resource if reset function is provided
            if (m_Reset)
            {
                m_Reset(resource);
            }
            
            // Return to available pool
            m_AvailableResources.push_back(resource);
            m_TotalReleased++;
            
            // Check if we should shrink the pool
            if (m_Config.AutoShrink && ShouldShrink())
            {
                ShrinkPool();
            }
        }

        /**
         * @brief Get pool statistics
         */
        struct Statistics
        {
            u32 AvailableCount = 0;
            u32 InUseCount = 0;
            u32 TotalCount = 0;
            u32 TotalCreated = 0;
            u32 TotalAcquired = 0;
            u32 TotalReleased = 0;
            u32 ValidationFailures = 0;
            f32 UtilizationRatio = 0.0f;
        };

        Statistics GetStatistics() const
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            
            Statistics stats;
            stats.AvailableCount = static_cast<u32>(m_AvailableResources.size());
            stats.InUseCount = static_cast<u32>(m_InUseResources.size());
            stats.TotalCount = stats.AvailableCount + stats.InUseCount;
            stats.TotalCreated = m_TotalCreated;
            stats.TotalAcquired = m_TotalAcquired;
            stats.TotalReleased = m_TotalReleased;
            stats.ValidationFailures = m_TotalValidationFailures;
            stats.UtilizationRatio = stats.TotalCount > 0 ? static_cast<f32>(stats.InUseCount) / stats.TotalCount : 0.0f;
            
            return stats;
        }

        /**
         * @brief Clear all available resources (keeps in-use resources)
         */
        void Clear()
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_AvailableResources.clear();
            OLO_CORE_TRACE("ResourcePool cleared - {0} resources in use remain", m_InUseResources.size());
        }

        /**
         * @brief Force pool to pre-allocate more resources
         */
        void Warmup(u32 count)
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            
            for (u32 i = 0; i < count && (m_Config.MaxSize == 0 || GetTotalSize() < m_Config.MaxSize); ++i)
            {
                if (auto resource = CreateResource())
                {
                    m_AvailableResources.push_back(resource);
                }
            }
            
            OLO_CORE_TRACE("ResourcePool warmed up with {0} additional resources", count);
        }

    private:
        ResourcePtr CreateResource()
        {
            try
            {
                auto resource = m_Factory();
                if (resource)
                {
                    m_TotalCreated++;
                    return resource;
                }
            }
            catch (const std::exception& e)
            {
                OLO_CORE_ERROR("ResourcePool: Failed to create resource - {0}", e.what());
            }
            
            return nullptr;
        }

        u32 GetTotalSize() const
        {
            return static_cast<u32>(m_AvailableResources.size() + m_InUseResources.size());
        }

        bool ShouldShrink() const
        {
            u32 totalSize = GetTotalSize();
            if (totalSize <= m_Config.InitialSize)
                return false;
                
            f32 utilizationRatio = totalSize > 0 ? static_cast<f32>(m_InUseResources.size()) / totalSize : 0.0f;
            return utilizationRatio < m_Config.ShrinkThreshold;
        }

        void ShrinkPool()
        {
            u32 targetSize = std::max(m_Config.InitialSize, static_cast<u32>(m_InUseResources.size() * 1.5f));
            u32 currentSize = static_cast<u32>(m_AvailableResources.size());
            
            if (currentSize > targetSize)
            {
                u32 toRemove = currentSize - targetSize;
                toRemove = std::min(toRemove, currentSize);
                
                m_AvailableResources.resize(currentSize - toRemove);
                OLO_CORE_TRACE("ResourcePool shrunk by {0} resources", toRemove);
            }
        }
    };

    // Type aliases for common resource pools
    using UniformBufferPool = ResourcePool<UniformBuffer>;
    using StorageBufferPool = ResourcePool<StorageBuffer>;
}
