#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/ShaderResourceTypes.h"
#include "Platform/OpenGL/OpenGLMultiBind.h"

#include <glad/gl.h>
#include <vector>
#include <unordered_map>
#include <array>

namespace OloEngine
{
    /**
     * @brief OpenGL adaptation of Hazel's descriptor set concept
     * 
     * Since OpenGL doesn't have native descriptor sets like Vulkan, this class
     * maps descriptor sets to binding ranges and provides efficient batch binding.
     */
    class OpenGLDescriptorSetManager
    {
    public:
        /**
         * @brief Descriptor set configuration for OpenGL
         */
        struct DescriptorSetLayout
        {
            u32 SetIndex = 0;
            std::string Name;
            u32 UniformBufferBaseBinding = 0;      // Starting binding for uniform buffers
            u32 StorageBufferBaseBinding = 0;      // Starting binding for storage buffers  
            u32 TextureBaseBinding = 0;            // Starting binding for textures
            u32 ImageBaseBinding = 0;              // Starting binding for images
            u32 MaxUniformBuffers = 8;             // Maximum uniform buffers in this set
            u32 MaxStorageBuffers = 8;             // Maximum storage buffers in this set
            u32 MaxTextures = 16;                  // Maximum textures in this set
            u32 MaxImages = 8;                     // Maximum images in this set
            bool IsActive = true;                  // Whether this set is active
            f32 Priority = 1.0f;                   // Binding priority (higher = bind first)
            
            DescriptorSetLayout() = default;
            DescriptorSetLayout(u32 setIndex, const std::string& name)
                : SetIndex(setIndex), Name(name) {}
        };

        /**
         * @brief Resource binding within a descriptor set
         */
        struct ResourceBinding
        {
            std::string Name;
            ShaderResourceType Type = ShaderResourceType::None;
            u32 LocalBinding = 0;                  // Binding within the set
            u32 GlobalBinding = 0;                 // Global OpenGL binding point
            u32 ArraySize = 1;                     // Array size (1 for non-arrays)
            bool IsArray = false;
            bool IsBound = false;                  // Whether resource is currently bound
            u32 BoundResourceID = 0;               // OpenGL ID of bound resource
            GLenum BoundTarget = 0;                // OpenGL target of bound resource
            
            ResourceBinding() = default;
            ResourceBinding(const std::string& name, ShaderResourceType type, u32 localBinding)
                : Name(name), Type(type), LocalBinding(localBinding) {}
        };

        /**
         * @brief Complete descriptor set instance
         */
        struct DescriptorSet
        {
            DescriptorSetLayout Layout;
            std::unordered_map<std::string, ResourceBinding> Bindings;
            std::vector<u32> UniformBufferIDs;    // OpenGL buffer IDs
            std::vector<u32> StorageBufferIDs;    // OpenGL buffer IDs
            std::vector<u32> TextureIDs;          // OpenGL texture IDs
            std::vector<GLenum> TextureTargets;   // Texture targets
            std::vector<u32> ImageIDs;            // OpenGL image IDs
            bool IsDirty = true;                   // Whether set needs rebinding
            u32 LastBoundFrame = 0;               // Frame when last bound
            
            void MarkDirty() { IsDirty = true; }
            void MarkClean() { IsDirty = false; }
            bool IsEmpty() const { return Bindings.empty(); }
            
            // Clear all bound resources
            void Clear()
            {
                UniformBufferIDs.clear();
                StorageBufferIDs.clear();
                TextureIDs.clear();
                TextureTargets.clear();
                ImageIDs.clear();
                for (auto& [name, binding] : Bindings)
                {
                    binding.IsBound = false;
                    binding.BoundResourceID = 0;
                    binding.BoundTarget = 0;
                }
                MarkDirty();
            }
        };

        /**
         * @brief Binding statistics for performance monitoring
         */
        struct BindingStatistics
        {
            u32 TotalBindings = 0;
            u32 SetBindings = 0;
            u32 IndividualBindings = 0;
            u32 CacheHits = 0;
            u32 CacheMisses = 0;
            u32 RedundantBindingsPrevented = 0;
            f32 AverageBindingsPerSet = 0.0f;
            std::unordered_map<u32, u32> SetUsage; // Set index -> usage count
            
            void Reset() { *this = BindingStatistics{}; }
            f32 GetCacheHitRatio() const 
            { 
                return (CacheHits + CacheMisses) > 0 ? (f32)CacheHits / (CacheHits + CacheMisses) : 0.0f; 
            }
        };

    public:
        OpenGLDescriptorSetManager();
        ~OpenGLDescriptorSetManager() = default;

        // Set layout management
        /**
         * @brief Create a descriptor set layout
         * @param setIndex Index of the descriptor set (0-based)
         * @param name Name for debugging
         * @param layout Layout configuration
         */
        void CreateSetLayout(u32 setIndex, const std::string& name, const DescriptorSetLayout& layout);

        /**
         * @brief Remove a descriptor set layout
         * @param setIndex Index of the set to remove
         */
        void RemoveSetLayout(u32 setIndex);

        /**
         * @brief Get descriptor set layout
         * @param setIndex Index of the set
         * @return Pointer to layout or nullptr if not found
         */
        const DescriptorSetLayout* GetSetLayout(u32 setIndex) const;

        /**
         * @brief Configure automatic binding ranges based on total binding counts
         * @param totalUniformBuffers Total uniform buffers across all sets
         * @param totalStorageBuffers Total storage buffers across all sets
         * @param totalTextures Total textures across all sets
         * @param totalImages Total images across all sets
         * @param setCount Number of descriptor sets to create
         */
        void ConfigureAutomaticBindingRanges(u32 totalUniformBuffers, u32 totalStorageBuffers, 
                                           u32 totalTextures, u32 totalImages, u32 setCount = 4);

        // Resource binding
        /**
         * @brief Bind a resource to a descriptor set
         * @param setIndex Descriptor set index
         * @param resourceName Name of the resource
         * @param resourceType Type of resource
         * @param localBinding Binding point within the set
         * @param resourceID OpenGL resource ID
         * @param target OpenGL target (for textures)
         * @param arraySize Array size (1 for non-arrays)
         */
        void BindResource(u32 setIndex, const std::string& resourceName, ShaderResourceType resourceType,
                         u32 localBinding, u32 resourceID, GLenum target = 0, u32 arraySize = 1);

        /**
         * @brief Unbind a resource from a descriptor set
         * @param setIndex Descriptor set index
         * @param resourceName Name of the resource to unbind
         */
        void UnbindResource(u32 setIndex, const std::string& resourceName);

        /**
         * @brief Bind an entire descriptor set to the OpenGL context
         * @param setIndex Index of the set to bind
         * @param forceRebind Whether to force rebinding even if not dirty
         */
        void BindDescriptorSet(u32 setIndex, bool forceRebind = false);

        /**
         * @brief Bind multiple descriptor sets in priority order
         * @param setIndices Vector of set indices to bind
         * @param forceRebind Whether to force rebinding
         */
        void BindDescriptorSets(const std::vector<u32>& setIndices, bool forceRebind = false);

        /**
         * @brief Bind all active descriptor sets in priority order
         * @param forceRebind Whether to force rebinding
         */
        void BindAllSets(bool forceRebind = false);

        // State management
        /**
         * @brief Mark a descriptor set as dirty (needs rebinding)
         * @param setIndex Index of the set to mark dirty
         */
        void MarkSetDirty(u32 setIndex);

        /**
         * @brief Mark all descriptor sets as dirty
         */
        void MarkAllSetsDirty();

        /**
         * @brief Clear all bound resources from a descriptor set
         * @param setIndex Index of the set to clear
         */
        void ClearDescriptorSet(u32 setIndex);

        /**
         * @brief Clear all descriptor sets
         */
        void ClearAllSets();

        /**
         * @brief Check if a descriptor set exists
         * @param setIndex Index to check
         * @return true if set exists
         */
        bool HasDescriptorSet(u32 setIndex) const;

        /**
         * @brief Check if a descriptor set is dirty
         * @param setIndex Index to check
         * @return true if set is dirty
         */
        bool IsSetDirty(u32 setIndex) const;

        /**
         * @brief Get all active descriptor set indices
         * @return Vector of active set indices
         */
        std::vector<u32> GetActiveSetIndices() const;

        /**
         * @brief Get binding order based on set priorities
         * @return Vector of set indices in binding order
         */
        std::vector<u32> GetBindingOrder() const;

        // OpenGL integration
        /**
         * @brief Set the multi-bind manager for efficient batch binding
         * @param multiBind Pointer to multi-bind manager (can be nullptr for direct binding)
         */
        void SetMultiBindManager(OpenGLMultiBind* multiBind) { m_MultiBind = multiBind; }

        /**
         * @brief Get the multi-bind manager
         * @return Pointer to multi-bind manager
         */
        OpenGLMultiBind* GetMultiBindManager() const { return m_MultiBind; }

        /**
         * @brief Enable or disable state caching
         * @param enabled Whether to enable state caching
         */
        void SetStateCachingEnabled(bool enabled);

        /**
         * @brief Invalidate all cached state
         */
        void InvalidateCache();

        // Statistics and debugging
        const BindingStatistics& GetStatistics() const { return m_Statistics; }
        void ResetStatistics() { m_Statistics.Reset(); }

        /**
         * @brief Generate performance report
         * @return Formatted performance report string
         */
        std::string GeneratePerformanceReport() const;

        /**
         * @brief Render ImGui debug interface
         */
        void RenderDebugInterface();

        // Hazel compatibility helpers
        /**
         * @brief Create standard PBR descriptor set layout
         * Sets: 0=System, 1=Global, 2=Material, 3=Instance
         */
        void CreateStandardPBRLayout();

        /**
         * @brief Create compute shader descriptor set layout
         */
        void CreateComputeLayout();

        /**
         * @brief Create post-process descriptor set layout
         */
        void CreatePostProcessLayout();

        /**
         * @brief Map Hazel resource declaration to OpenGL binding
         * @param resourceName Name of the resource
         * @param set Descriptor set from Hazel
         * @param binding Binding point from Hazel
         * @param resourceType Type of resource
         * @return Global OpenGL binding point
         */
        u32 MapHazelBinding(const std::string& resourceName, u32 set, u32 binding, ShaderResourceType resourceType);

    private:
        // Internal binding implementation
        void BindDescriptorSetInternal(DescriptorSet& descriptorSet, bool forceRebind);
        void BindUniformBuffers(const DescriptorSet& descriptorSet);
        void BindStorageBuffers(const DescriptorSet& descriptorSet);
        void BindTextures(const DescriptorSet& descriptorSet);
        void BindImages(const DescriptorSet& descriptorSet);

        // Resource management helpers
        u32 CalculateGlobalBinding(const DescriptorSetLayout& layout, ShaderResourceType type, u32 localBinding) const;
        void UpdateBindingStatistics(const DescriptorSet& descriptorSet);
        bool ValidateBinding(u32 setIndex, const std::string& resourceName, ShaderResourceType resourceType, u32 localBinding) const;

        // State caching helpers
        bool IsResourceCached(u32 globalBinding, u32 resourceID, GLenum target) const;
        void UpdateResourceCache(u32 globalBinding, u32 resourceID, GLenum target);

    private:
        // Descriptor sets storage
        std::unordered_map<u32, DescriptorSet> m_DescriptorSets;
        
        // Binding range configuration
        struct GlobalBindingRanges
        {
            u32 UniformBufferStart = 0;
            u32 StorageBufferStart = 32;
            u32 TextureStart = 0;
            u32 ImageStart = 32;
        } m_GlobalRanges;

        // OpenGL integration
        OpenGLMultiBind* m_MultiBind = nullptr;
        bool m_StateCachingEnabled = true;
        
        // State caching for redundancy elimination
        struct StateCache
        {
            std::unordered_map<u32, std::pair<u32, GLenum>> BoundResources; // binding -> (resourceID, target)
            bool IsValid = false;
            
            void Invalidate() { BoundResources.clear(); IsValid = false; }
        } m_StateCache;

        // Statistics
        mutable BindingStatistics m_Statistics;
        u32 m_CurrentFrame = 0;

        // Binding order cache
        mutable std::vector<u32> m_CachedBindingOrder;
        mutable bool m_BindingOrderDirty = true;
    };
}
