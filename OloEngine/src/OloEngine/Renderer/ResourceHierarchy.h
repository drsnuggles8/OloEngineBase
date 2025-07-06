#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/UniformBufferRegistry.h"
#include "OloEngine/Renderer/FrameInFlightManager.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <functional>

namespace OloEngine
{
    /**
     * @brief Priority levels for resource binding hierarchy
     */
    enum class ResourcePriority : u8
    {
        System = 0,         // Highest priority: System-level resources (camera, lighting)
        Global = 1,         // High priority: Global scene resources
        Material = 2,       // Medium priority: Material-specific resources
        Instance = 3,       // Low priority: Per-instance resources
        Debug = 4           // Lowest priority: Debug and temporary resources
    };

    /**
     * @brief Resource scope defines the lifetime and accessibility of resources
     */
    enum class ResourceScope : u8
    {
        Frame = 0,          // Resource valid for one frame only
        Scene = 1,          // Resource valid for the current scene
        Global = 2,         // Resource valid throughout application lifetime
        Persistent = 3      // Resource persists across scene changes
    };

    /**
     * @brief Information about a hierarchical resource node
     */
    struct ResourceNode
    {
        std::string Name;
        ShaderResourceType Type = ShaderResourceType::None;
        ResourcePriority Priority = ResourcePriority::Instance;
        ResourceScope Scope = ResourceScope::Frame;
        ShaderResource Resource;
        
        // Hierarchy information
        std::string ParentName;                             // Parent node name (empty for root)
        std::vector<std::string> ChildrenNames;             // Direct children node names
        std::unordered_set<std::string> Dependencies;       // Resources this node depends on
        std::unordered_set<std::string> Dependents;         // Resources that depend on this node
        
        // Binding information
        u32 BindingPoint = 0;
        bool IsActive = false;
        bool IsDirty = false;
        
        // Metadata
        sizet LastModified = 0;                             // Frame number when last modified
        std::string Description;                            // Human-readable description
        
        ResourceNode() = default;
        ResourceNode(const std::string& name, ShaderResourceType type, 
                    ResourcePriority priority = ResourcePriority::Instance,
                    ResourceScope scope = ResourceScope::Frame)
            : Name(name), Type(type), Priority(priority), Scope(scope) {}
    };

    /**
     * @brief Hierarchical resource organization and management system
     * 
     * Provides a tree-like structure for organizing shader resources with priority-based
     * binding, dependency tracking, and automatic resource resolution.
     */
    class ResourceHierarchy
    {
    public:
        ResourceHierarchy() = default;
        ~ResourceHierarchy() = default;

        // No copy semantics
        ResourceHierarchy(const ResourceHierarchy&) = delete;
        ResourceHierarchy& operator=(const ResourceHierarchy&) = delete;

        // Move semantics
        ResourceHierarchy(ResourceHierarchy&&) = default;
        ResourceHierarchy& operator=(ResourceHierarchy&&) = default;

        /**
         * @brief Initialize the hierarchy system
         */
        void Initialize();

        /**
         * @brief Shutdown and clear all resources
         */
        void Shutdown();

        /**
         * @brief Register a resource in the hierarchy
         * @param name Resource name
         * @param type Resource type
         * @param priority Priority level for binding order
         * @param scope Resource scope/lifetime
         * @param parentName Parent resource name (empty for root level)
         * @return true if registration successful
         */
        bool RegisterResource(const std::string& name, ShaderResourceType type,
                            ResourcePriority priority = ResourcePriority::Instance,
                            ResourceScope scope = ResourceScope::Frame,
                            const std::string& parentName = "");

        /**
         * @brief Set a resource in the hierarchy
         * @param name Resource name
         * @param resource Resource to bind
         * @return true if successful
         */
        bool SetResource(const std::string& name, const ShaderResourceInput& resource);

        /**
         * @brief Template method for setting typed resources
         * @tparam T Resource type
         * @param name Resource name
         * @param resource Resource to bind
         * @return true if successful
         */
        template<typename T>
        bool SetResource(const std::string& name, const Ref<T>& resource)
        {
            return SetResource(name, ShaderResourceInput(resource));
        }

        /**
         * @brief Remove a resource from the hierarchy
         * @param name Resource name
         * @return true if removed successfully
         */
        bool RemoveResource(const std::string& name);

        /**
         * @brief Add a dependency between two resources
         * @param dependentName Resource that depends on another
         * @param dependencyName Resource that is depended upon
         * @return true if dependency added successfully
         */
        bool AddDependency(const std::string& dependentName, const std::string& dependencyName);

        /**
         * @brief Remove a dependency between two resources
         * @param dependentName Resource that depends on another
         * @param dependencyName Resource that is depended upon
         * @return true if dependency removed successfully
         */
        bool RemoveDependency(const std::string& dependentName, const std::string& dependencyName);

        /**
         * @brief Get a resource by name
         * @param name Resource name
         * @return Pointer to resource node if found, nullptr otherwise
         */
        const ResourceNode* GetResource(const std::string& name) const;

        /**
         * @brief Get all resources at a specific priority level
         * @param priority Priority level to filter by
         * @return Vector of resource nodes at the specified priority
         */
        std::vector<const ResourceNode*> GetResourcesByPriority(ResourcePriority priority) const;

        /**
         * @brief Get all resources in a specific scope
         * @param scope Resource scope to filter by
         * @return Vector of resource nodes in the specified scope
         */
        std::vector<const ResourceNode*> GetResourcesByScope(ResourceScope scope) const;

        /**
         * @brief Get children of a specific resource
         * @param parentName Parent resource name
         * @return Vector of child resource nodes
         */
        std::vector<const ResourceNode*> GetChildren(const std::string& parentName) const;

        /**
         * @brief Get all resources in dependency order (topological sort)
         * @return Vector of resource nodes in dependency-resolved order
         */
        std::vector<const ResourceNode*> GetResourcesInDependencyOrder() const;

        /**
         * @brief Get all resources in priority-then-dependency order
         * @return Vector of resource nodes in binding order
         */
        std::vector<const ResourceNode*> GetResourcesInBindingOrder() const;

        /**
         * @brief Clear resources by scope (useful for frame/scene cleanup)
         * @param scope Scope to clear
         */
        void ClearResourcesByScope(ResourceScope scope);

        /**
         * @brief Mark a resource as dirty (needs rebinding)
         * @param name Resource name
         */
        void MarkResourceDirty(const std::string& name);

        /**
         * @brief Mark all resources with dependencies on the given resource as dirty
         * @param name Resource name that changed
         */
        void MarkDependentsDirty(const std::string& name);

        /**
         * @brief Get all dirty resources that need rebinding
         * @return Vector of dirty resource nodes
         */
        std::vector<const ResourceNode*> GetDirtyResources() const;

        /**
         * @brief Clear dirty flags for all resources
         */
        void ClearDirtyFlags();

        /**
         * @brief Validate the hierarchy for circular dependencies
         * @return true if hierarchy is valid, false if circular dependencies found
         */
        bool ValidateHierarchy() const;

        /**
         * @brief Get statistics about the hierarchy
         */
        struct Statistics
        {
            u32 TotalResources = 0;
            u32 SystemResources = 0;
            u32 GlobalResources = 0;
            u32 MaterialResources = 0;
            u32 InstanceResources = 0;
            u32 DebugResources = 0;
            u32 ActiveResources = 0;
            u32 DirtyResources = 0;
            u32 TotalDependencies = 0;
            u32 MaxDepth = 0;
        };

        Statistics GetStatistics() const;

        /**
         * @brief Get string representation of priority level
         * @param priority Priority to convert
         * @return String representation
         */
        static const char* GetPriorityString(ResourcePriority priority);

        /**
         * @brief Get string representation of scope
         * @param scope Scope to convert
         * @return String representation
         */
        static const char* GetScopeString(ResourceScope scope);

        /**
         * @brief Render ImGui debug interface for the hierarchy
         */
        void RenderDebugInterface();

    private:
        // Resource storage
        std::unordered_map<std::string, ResourceNode> m_Resources;
        
        // Root-level resources (no parent)
        std::unordered_set<std::string> m_RootResources;
        
        // Initialization state
        bool m_Initialized = false;
        
        // Frame counter for tracking modifications
        sizet m_FrameNumber = 0;

        /**
         * @brief Perform topological sort for dependency resolution
         * @param nodes Vector to sort
         * @return true if sort successful, false if circular dependency detected
         */
        bool TopologicalSort(std::vector<const ResourceNode*>& nodes) const;

        /**
         * @brief Detect circular dependencies starting from a node
         * @param nodeName Starting node name
         * @param visited Set of visited nodes
         * @param recursionStack Current recursion stack
         * @return true if circular dependency detected
         */
        bool HasCircularDependency(const std::string& nodeName, 
                                  std::unordered_set<std::string>& visited,
                                  std::unordered_set<std::string>& recursionStack) const;

        /**
         * @brief Remove a resource from parent's children list
         * @param childName Child resource name
         * @param parentName Parent resource name
         */
        void RemoveFromParent(const std::string& childName, const std::string& parentName);

        /**
         * @brief Calculate maximum depth of the hierarchy
         * @return Maximum depth
         */
        u32 CalculateMaxDepth() const;

        /**
         * @brief Calculate depth for a specific node recursively
         * @param nodeName Node to calculate depth for
         * @param visited Set to track visited nodes (cycle detection)
         * @return Depth of the node
         */
        u32 CalculateNodeDepth(const std::string& nodeName, 
                              std::unordered_set<std::string>& visited) const;
    };
}
