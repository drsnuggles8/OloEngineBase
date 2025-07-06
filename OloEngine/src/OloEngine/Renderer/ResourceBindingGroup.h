#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/UniformBufferRegistry.h"
#include "OloEngine/Renderer/ResourceStateTracker.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemap.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <functional>
#include <chrono>
#include <algorithm>

#include <glad/gl.h>

namespace OloEngine
{
    /**
     * @brief Represents a cohesive group of shader resources that are bound together
     * 
     * Provides efficient batch binding operations, automatic dependency resolution,
     * and optimized state management for groups of related resources.
     */
    class ResourceBindingGroup
    {
    public:
        struct ResourceBinding
        {
            std::string Name;
            ShaderResourceType Type;
            u32 Binding;
            Ref<void> Resource;
            bool IsDirty = true;
            std::chrono::steady_clock::time_point LastBound;
            
            ResourceBinding() = default;
            ResourceBinding(const std::string& name, ShaderResourceType type, u32 binding, Ref<void> resource)
                : Name(name), Type(type), Binding(binding), Resource(std::move(resource)) {}
        };

        struct BindingGroupStats
        {
            u32 TotalBindings = 0;
            u32 ActiveBindings = 0;
            u32 DirtyBindings = 0;
            u32 TotalBindOperations = 0;
            u32 SkippedBindings = 0;
            f32 AverageBindTime = 0.0f;
            std::chrono::steady_clock::time_point LastFullBind;
        };

        enum class BindingStrategy
        {
            Immediate,     // Bind all resources immediately
            Lazy,          // Only bind dirty resources
            Batched,       // Group bindings by type for efficiency
            StateTracked   // Use state tracker to optimize bindings
        };

    private:
        std::string m_Name;
        std::unordered_map<std::string, ResourceBinding> m_Bindings;
        std::vector<std::string> m_BindingOrder;
        std::unordered_map<u32, std::string> m_BindingPointMap;
        
        BindingStrategy m_Strategy = BindingStrategy::Lazy;
        BindingGroupStats m_Stats;
        
        UniformBufferRegistry* m_Registry = nullptr;
        ResourceStateTracker* m_StateTracker = nullptr;
        
        // Dependencies and validation
        std::vector<std::string> m_Dependencies;
        std::function<bool(const ResourceBindingGroup&)> m_Validator;
        
        // Performance optimization
        bool m_EnableOptimization = true;
        u32 m_MaxConcurrentBindings = 16;
        f32 m_BindingTimeThreshold = 1.0f; // ms

    public:
        explicit ResourceBindingGroup(const std::string& name)
            : m_Name(name)
        {
            m_Stats.LastFullBind = std::chrono::steady_clock::now();
        }

        /**
         * @brief Set the resource registry
         */
        void SetRegistry(UniformBufferRegistry* registry) { m_Registry = registry; }

        /**
         * @brief Set the state tracker
         */
        void SetStateTracker(ResourceStateTracker* tracker) { m_StateTracker = tracker; }

        /**
         * @brief Set binding strategy
         */
        void SetBindingStrategy(BindingStrategy strategy) { m_Strategy = strategy; }

        /**
         * @brief Add a resource to the binding group
         */
        template<typename T>
        void AddResource(const std::string& name, u32 binding, Ref<T> resource)
        {
            ShaderResourceType type = DeduceResourceType<T>();
            
            ResourceBinding resourceBinding(name, type, binding, std::static_pointer_cast<void>(resource));
            
            m_Bindings[name] = std::move(resourceBinding);
            m_BindingOrder.push_back(name);
            m_BindingPointMap[binding] = name;
            
            m_Stats.TotalBindings++;
            
            OLO_CORE_TRACE("ResourceBindingGroup '{0}': Added resource '{1}' at binding {2}", 
                          m_Name, name, binding);
        }

        /**
         * @brief Remove a resource from the binding group
         */
        void RemoveResource(const std::string& name)
        {
            auto it = m_Bindings.find(name);
            if (it != m_Bindings.end())
            {
                u32 binding = it->second.Binding;
                
                m_Bindings.erase(it);
                m_BindingPointMap.erase(binding);
                
                // Remove from binding order
                auto orderIt = std::find(m_BindingOrder.begin(), m_BindingOrder.end(), name);
                if (orderIt != m_BindingOrder.end())
                {
                    m_BindingOrder.erase(orderIt);
                }
                
                m_Stats.TotalBindings--;
                
                OLO_CORE_TRACE("ResourceBindingGroup '{0}': Removed resource '{1}'", m_Name, name);
            }
        }

        /**
         * @brief Update a resource in the binding group
         */
        template<typename T>
        void UpdateResource(const std::string& name, Ref<T> newResource)
        {
            auto it = m_Bindings.find(name);
            if (it != m_Bindings.end())
            {
                it->second.Resource = std::static_pointer_cast<void>(newResource);
                it->second.IsDirty = true;
                
                if (m_StateTracker)
                {
                    m_StateTracker->RecordUpdate(name, it->second.Type, 0); // Size unknown for generic update
                }
                
                OLO_CORE_TRACE("ResourceBindingGroup '{0}': Updated resource '{1}'", m_Name, name);
            }
        }

        /**
         * @brief Mark a resource as dirty (needs rebinding)
         */
        void MarkDirty(const std::string& name)
        {
            auto it = m_Bindings.find(name);
            if (it != m_Bindings.end())
            {
                it->second.IsDirty = true;
            }
        }

        /**
         * @brief Mark all resources as dirty
         */
        void MarkAllDirty()
        {
            for (auto& [name, binding] : m_Bindings)
            {
                binding.IsDirty = true;
            }
        }

        /**
         * @brief Bind all resources in the group using Phase 3 multi-set optimization
         */
        void Bind()
        {
            auto startTime = std::chrono::high_resolution_clock::now();
            
            // Phase 3 Integration: Use registry's multi-set binding if available
            if (m_Registry && m_Registry->GetSpecification().UseSetPriority)
            {
                BindWithMultiSetOptimization();
            }
            else
            {
                // Fall back to traditional binding strategies
                switch (m_Strategy)
                {
                    case BindingStrategy::Immediate:
                        BindImmediate();
                        break;
                    case BindingStrategy::Lazy:
                        BindLazy();
                        break;
                    case BindingStrategy::Batched:
                        BindBatched();
                        break;
                    case BindingStrategy::StateTracked:
                        BindStateTracked();
                        break;
                }
            }
            
            auto endTime = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
            
            UpdateBindingStats(duration.count() / 1000.0f); // Convert to milliseconds
            
            m_Stats.LastFullBind = std::chrono::steady_clock::now();
            
            if (m_StateTracker)
            {
                for (const auto& [name, binding] : m_Bindings)
                {
                    m_StateTracker->RecordAccess(name, binding.Type);
                }
            }
        }

        /**
         * @brief Bind only dirty resources
         */
        void BindDirty()
        {
            u32 boundCount = 0;
            
            for (auto& [name, binding] : m_Bindings)
            {
                if (binding.IsDirty)
                {
                    BindSingleResource(binding);
                    binding.IsDirty = false;
                    boundCount++;
                }
            }
            
            m_Stats.TotalBindOperations += boundCount;
            
            OLO_CORE_TRACE("ResourceBindingGroup '{0}': Bound {1} dirty resources", m_Name, boundCount);
        }

        /**
         * @brief Unbind all resources in the group
         */
        void Unbind()
        {
            for (const auto& [name, binding] : m_Bindings)
            {
                UnbindSingleResource(binding);
            }
            
            OLO_CORE_TRACE("ResourceBindingGroup '{0}': Unbound all resources", m_Name);
        }

        /**
         * @brief Validate all resources in the group
         */
        bool ValidateBindings() const
        {
            if (m_Validator && !m_Validator(*this))
            {
                OLO_CORE_ERROR("ResourceBindingGroup '{0}': Custom validation failed", m_Name);
                return false;
            }
            
            // Check for binding conflicts
            std::unordered_set<u32> usedBindings;
            for (const auto& [name, binding] : m_Bindings)
            {
                if (usedBindings.contains(binding.Binding))
                {
                    OLO_CORE_ERROR("ResourceBindingGroup '{0}': Binding conflict at point {1}", 
                                  m_Name, binding.Binding);
                    return false;
                }
                usedBindings.insert(binding.Binding);
                
                // Validate resource is not null
                if (!binding.Resource)
                {
                    OLO_CORE_ERROR("ResourceBindingGroup '{0}': Null resource '{1}'", m_Name, name);
                    return false;
                }
            }
            
            return true;
        }

        /**
         * @brief Check if the group has any dirty resources
         */
        bool HasDirtyResources() const
        {
            for (const auto& [name, binding] : m_Bindings)
            {
                if (binding.IsDirty)
                    return true;
            }
            return false;
        }

        /**
         * @brief Get binding group statistics
         */
        const BindingGroupStats& GetStats() const { return m_Stats; }

        /**
         * @brief Get resource binding by name
         */
        const ResourceBinding* GetBinding(const std::string& name) const
        {
            auto it = m_Bindings.find(name);
            return (it != m_Bindings.end()) ? &it->second : nullptr;
        }

        /**
         * @brief Get all bindings
         */
        const std::unordered_map<std::string, ResourceBinding>& GetBindings() const { return m_Bindings; }

        /**
         * @brief Set custom validator
         */
        void SetValidator(std::function<bool(const ResourceBindingGroup&)> validator)
        {
            m_Validator = std::move(validator);
        }

        /**
         * @brief Add dependency on another binding group
         */
        void AddDependency(const std::string& groupName)
        {
            m_Dependencies.push_back(groupName);
        }

        /**
         * @brief Get dependencies
         */
        const std::vector<std::string>& GetDependencies() const { return m_Dependencies; }

        /**
         * @brief Enable/disable binding optimizations
         */
        void SetOptimizationEnabled(bool enabled) { m_EnableOptimization = enabled; }

    private:
        template<typename T>
        ShaderResourceType DeduceResourceType()
        {
            if constexpr (std::is_same_v<T, UniformBuffer>)
                return ShaderResourceType::UniformBuffer;
            else if constexpr (std::is_same_v<T, StorageBuffer>)
                return ShaderResourceType::StorageBuffer;
            else if constexpr (std::is_same_v<T, Texture2D>)
                return ShaderResourceType::Texture2D;
            else if constexpr (std::is_same_v<T, TextureCubemap>)
                return ShaderResourceType::TextureCube;
            else
                return ShaderResourceType::Unknown;
        }

        void BindImmediate()
        {
            m_Stats.ActiveBindings = 0;
            
            for (const auto& name : m_BindingOrder)
            {
                auto& binding = m_Bindings[name];
                BindSingleResource(binding);
                binding.IsDirty = false;
                m_Stats.ActiveBindings++;
            }
            
            m_Stats.TotalBindOperations += m_Stats.ActiveBindings;
        }

        void BindLazy()
        {
            m_Stats.ActiveBindings = 0;
            m_Stats.SkippedBindings = 0;
            
            for (auto& [name, binding] : m_Bindings)
            {
                if (binding.IsDirty)
                {
                    BindSingleResource(binding);
                    binding.IsDirty = false;
                    m_Stats.ActiveBindings++;
                }
                else
                {
                    m_Stats.SkippedBindings++;
                }
            }
            
            m_Stats.TotalBindOperations += m_Stats.ActiveBindings;
        }

        void BindBatched()
        {
            // Group bindings by type for more efficient state changes
            std::unordered_map<ShaderResourceType, std::vector<ResourceBinding*>> batchedBindings;
            
            for (auto& [name, binding] : m_Bindings)
            {
                if (binding.IsDirty)
                {
                    batchedBindings[binding.Type].push_back(&binding);
                }
            }
            
            m_Stats.ActiveBindings = 0;
            
            // Bind resources in batches by type
            for (auto& [type, bindings] : batchedBindings)
            {
                SetupResourceTypeState(type);
                
                for (auto* binding : bindings)
                {
                    BindSingleResource(*binding);
                    binding->IsDirty = false;
                    m_Stats.ActiveBindings++;
                }
            }
            
            m_Stats.TotalBindOperations += m_Stats.ActiveBindings;
        }

        void BindStateTracked()
        {
            if (!m_StateTracker)
            {
                // Fallback to lazy binding
                BindLazy();
                return;
            }
            
            // Use state tracker information to optimize binding order
            std::vector<std::pair<std::string, f32>> prioritizedBindings;
            
            for (const auto& [name, binding] : m_Bindings)
            {
                if (binding.IsDirty)
                {
                    // Get access frequency from state tracker using resource info
                    auto resourceInfo = m_StateTracker->GetResourceInfo(name);
                    f32 frequency = static_cast<f32>(resourceInfo.TotalAccesses);
                    prioritizedBindings.emplace_back(name, frequency);
                }
            }
            
            // Sort by frequency (most frequent first)
            std::sort(prioritizedBindings.begin(), prioritizedBindings.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
            
            m_Stats.ActiveBindings = 0;
            
            for (const auto& [name, frequency] : prioritizedBindings)
            {
                auto& binding = m_Bindings[name];
                BindSingleResource(binding);
                binding.IsDirty = false;
                m_Stats.ActiveBindings++;
            }
            
            m_Stats.TotalBindOperations += m_Stats.ActiveBindings;
        }

        void BindSingleResource(ResourceBinding& binding)
        {
            auto startTime = std::chrono::high_resolution_clock::now();
            
            switch (binding.Type)
            {
                case ShaderResourceType::UniformBuffer:
                    {
                        auto uniformBuffer = std::static_pointer_cast<UniformBuffer>(binding.Resource);
                        // UniformBuffer binds automatically when created with binding point
                        // No explicit bind call needed for OpenGL uniform buffers
                    }
                    break;
                    
                case ShaderResourceType::StorageBuffer:
                    {
                        auto storageBuffer = std::static_pointer_cast<StorageBuffer>(binding.Resource);
                        storageBuffer->Bind(binding.Binding);
                    }
                    break;
                    
                case ShaderResourceType::Texture2D:
                    {
                        auto texture = std::static_pointer_cast<Texture2D>(binding.Resource);
                        texture->Bind(binding.Binding);
                    }
                    break;
                    
                case ShaderResourceType::TextureCube:
                    {
                        auto texture = std::static_pointer_cast<TextureCubemap>(binding.Resource);
                        texture->Bind(binding.Binding);
                    }
                    break;
                    
                default:
                    OLO_CORE_WARN("ResourceBindingGroup '{0}': Unknown resource type for '{1}'", 
                                 m_Name, binding.Name);
                    break;
            }
            
            auto endTime = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
            
            binding.LastBound = std::chrono::steady_clock::now();
            
            // Check for performance issues
            f32 bindTime = duration.count() / 1000.0f; // Convert to milliseconds
            if (bindTime > m_BindingTimeThreshold)
            {
                OLO_CORE_WARN("ResourceBindingGroup '{0}': Slow binding for '{1}' ({2:.2f}ms)", 
                             m_Name, binding.Name, bindTime);
            }
        }

        void UnbindSingleResource(const ResourceBinding& binding)
        {
            // In OpenGL, resources typically don't need explicit unbinding,
            // but this could be used for validation or other APIs
            
            if (m_StateTracker)
            {
                // This could be used to track resource release patterns
                // m_StateTracker->RecordRelease(binding.Name, binding.Type);
            }
        }

        void SetupResourceTypeState(ShaderResourceType type)
        {
            // This would contain OpenGL state setup specific to each resource type
            // For example, setting up texture units, buffer binding targets, etc.
            
            switch (type)
            {
                case ShaderResourceType::Texture2D:
                case ShaderResourceType::TextureCube:
                    // Setup texture state
                    glActiveTexture(GL_TEXTURE0); // Example
                    break;
                    
                case ShaderResourceType::UniformBuffer:
                case ShaderResourceType::StorageBuffer:
                    // Setup buffer state
                    break;
                    
                default:
                    break;
            }
        }

        void UpdateBindingStats(f32 bindTime)
        {
            // Update running average of bind time
            const f32 alpha = 0.1f; // Smoothing factor
            m_Stats.AverageBindTime = m_Stats.AverageBindTime * (1.0f - alpha) + bindTime * alpha;
            
            // Count dirty bindings
            m_Stats.DirtyBindings = 0;
            for (const auto& [name, binding] : m_Bindings)
            {
                if (binding.IsDirty)
                    m_Stats.DirtyBindings++;
            }
        }

        /**
         * @brief Phase 3 Integration: Bind resources using multi-set optimization
         */
        void BindWithMultiSetOptimization()
        {
            if (!m_Registry)
            {
                BindLazy(); // Fallback
                return;
            }

            // Group resources by descriptor set for optimal binding order
            std::map<u32, std::vector<std::string>> setGroups;
            
            for (const auto& [name, binding] : m_Bindings)
            {
                if (binding.IsDirty)
                {
                    u32 setIndex = m_Registry->GetResourceSetIndex(name);
                    if (setIndex != UINT32_MAX)
                    {
                        setGroups[setIndex].push_back(name);
                    }
                    else
                    {
                        // Resource not assigned to a set, bind individually
                        auto& resourceBinding = m_Bindings[name];
                        BindSingleResource(resourceBinding);
                        resourceBinding.IsDirty = false;
                        m_Stats.ActiveBindings++;
                    }
                }
            }

            // Bind resources in set priority order
            const auto& setBindingOrder = m_Registry->GetSetBindingOrder();
            for (u32 setIndex : setBindingOrder)
            {
                auto setIt = setGroups.find(setIndex);
                if (setIt != setGroups.end())
                {
                    // Set up state for this descriptor set
                    const auto* setInfo = m_Registry->GetDescriptorSetInfo(setIndex);
                    if (setInfo)
                    {
                        OLO_CORE_TRACE("ResourceBindingGroup '{0}': Binding set {1} '{2}' ({3} resources)", 
                                      m_Name, setIndex, setInfo->Name, setIt->second.size());
                    }

                    // Bind all resources in this set
                    for (const std::string& resourceName : setIt->second)
                    {
                        auto& resourceBinding = m_Bindings[resourceName];
                        BindSingleResource(resourceBinding);
                        resourceBinding.IsDirty = false;
                        m_Stats.ActiveBindings++;
                    }
                }
            }

            m_Stats.TotalBindOperations += m_Stats.ActiveBindings;
        }
    };

    /**
     * @brief Manager for multiple resource binding groups
     */
    class ResourceBindingGroupManager
    {
    private:
        std::unordered_map<std::string, Scope<ResourceBindingGroup>> m_Groups;
        std::vector<std::string> m_BindingOrder;
        UniformBufferRegistry* m_Registry = nullptr;
        ResourceStateTracker* m_StateTracker = nullptr;
        
    public:
        void SetRegistry(UniformBufferRegistry* registry) { m_Registry = registry; }
        void SetStateTracker(ResourceStateTracker* tracker) { m_StateTracker = tracker; }

        /**
         * @brief Create a new binding group
         */
        ResourceBindingGroup* CreateGroup(const std::string& name)
        {
            auto group = CreateScope<ResourceBindingGroup>(name);
            group->SetRegistry(m_Registry);
            group->SetStateTracker(m_StateTracker);
            
            auto* ptr = group.get();
            m_Groups[name] = std::move(group);
            m_BindingOrder.push_back(name);
            
            OLO_CORE_TRACE("ResourceBindingGroupManager: Created group '{0}'", name);
            return ptr;
        }

        /**
         * @brief Get a binding group by name
         */
        ResourceBindingGroup* GetGroup(const std::string& name)
        {
            auto it = m_Groups.find(name);
            return (it != m_Groups.end()) ? it->second.get() : nullptr;
        }

        /**
         * @brief Remove a binding group
         */
        void RemoveGroup(const std::string& name)
        {
            auto it = m_Groups.find(name);
            if (it != m_Groups.end())
            {
                m_Groups.erase(it);
                
                auto orderIt = std::find(m_BindingOrder.begin(), m_BindingOrder.end(), name);
                if (orderIt != m_BindingOrder.end())
                {
                    m_BindingOrder.erase(orderIt);
                }
                
                OLO_CORE_TRACE("ResourceBindingGroupManager: Removed group '{0}'", name);
            }
        }

        /**
         * @brief Bind all groups in dependency order
         */
        void BindAll()
        {
            // TODO: Implement dependency resolution
            for (const auto& name : m_BindingOrder)
            {
                auto* group = GetGroup(name);
                if (group && group->ValidateBindings())
                {
                    group->Bind();
                }
            }
        }

        /**
         * @brief Bind only groups with dirty resources
         */
        void BindDirty()
        {
            for (const auto& name : m_BindingOrder)
            {
                auto* group = GetGroup(name);
                if (group && group->HasDirtyResources())
                {
                    group->BindDirty();
                }
            }
        }
    };

    /**
     * @brief Example usage:
     * 
     * auto manager = CreateScope<ResourceBindingGroupManager>();
     * auto* materialGroup = manager->CreateGroup("MaterialResources");
     * 
     * materialGroup->AddResource("DiffuseTexture", 0, diffuseTexture);
     * materialGroup->AddResource("MaterialBuffer", 1, materialBuffer);
     * materialGroup->SetBindingStrategy(BindingStrategy::Batched);
     * 
     * // Later in render loop:
     * materialGroup->Bind(); // Efficiently binds all resources
     */
}
