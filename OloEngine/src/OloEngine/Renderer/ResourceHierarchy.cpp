#include "OloEnginePCH.h"
#include "ResourceHierarchy.h"
#include "OloEngine/Core/Log.h"

#include <imgui.h>
#include <algorithm>
#include <queue>

namespace OloEngine
{
    void ResourceHierarchy::Initialize()
    {
        if (m_Initialized)
        {
            OLO_CORE_WARN("ResourceHierarchy already initialized");
            return;
        }

        m_Resources.clear();
        m_RootResources.clear();
        m_FrameNumber = 0;
        m_Initialized = true;

        OLO_CORE_TRACE("ResourceHierarchy initialized");
    }

    void ResourceHierarchy::Shutdown()
    {
        m_Resources.clear();
        m_RootResources.clear();
        m_Initialized = false;

        OLO_CORE_TRACE("ResourceHierarchy shutdown");
    }

    bool ResourceHierarchy::RegisterResource(const std::string& name, ShaderResourceType type,
                                           ResourcePriority priority, ResourceScope scope,
                                           const std::string& parentName)
    {
        if (!m_Initialized)
        {
            OLO_CORE_ERROR("ResourceHierarchy not initialized");
            return false;
        }

        if (name.empty())
        {
            OLO_CORE_ERROR("Resource name cannot be empty");
            return false;
        }

        // Check if resource already exists
        if (m_Resources.find(name) != m_Resources.end())
        {
            OLO_CORE_WARN("Resource '{0}' already registered", name);
            return false;
        }

        // Validate parent exists if specified
        if (!parentName.empty())
        {
            auto parentIt = m_Resources.find(parentName);
            if (parentIt == m_Resources.end())
            {
                OLO_CORE_ERROR("Parent resource '{0}' not found for '{1}'", parentName, name);
                return false;
            }
        }

        // Create the resource node
        ResourceNode node(name, type, priority, scope);
        node.ParentName = parentName;
        node.LastModified = m_FrameNumber;

        // Add to resources map
        m_Resources[name] = std::move(node);

        // Update parent-child relationships
        if (parentName.empty())
        {
            m_RootResources.insert(name);
        }
        else
        {
            auto& parentNode = m_Resources[parentName];
            parentNode.ChildrenNames.push_back(name);
        }

        OLO_CORE_TRACE("Registered resource '{0}' (type: {1}, priority: {2}, scope: {3}, parent: '{4}')",
                      name, static_cast<u32>(type), static_cast<u32>(priority), 
                      static_cast<u32>(scope), parentName.empty() ? "none" : parentName);

        return true;
    }

    bool ResourceHierarchy::SetResource(const std::string& name, const ShaderResourceInput& resource)
    {
        if (!m_Initialized)
        {
            OLO_CORE_ERROR("ResourceHierarchy not initialized");
            return false;
        }

        auto it = m_Resources.find(name);
        if (it == m_Resources.end())
        {
            OLO_CORE_ERROR("Resource '{0}' not registered", name);
            return false;
        }

        ResourceNode& node = it->second;

        // Validate resource type matches
        if (node.Type != resource.Type)
        {
            OLO_CORE_ERROR("Resource type mismatch for '{0}'. Expected {1}, got {2}",
                          name, static_cast<u32>(node.Type), static_cast<u32>(resource.Type));
            return false;
        }

        // Update the resource
        node.Resource = resource.Resource;
        node.IsActive = true;
        node.IsDirty = true;
        node.LastModified = m_FrameNumber;

        // Mark dependents as dirty
        MarkDependentsDirty(name);

        OLO_CORE_TRACE("Set resource '{0}' (type: {1})", name, static_cast<u32>(resource.Type));
        return true;
    }

    bool ResourceHierarchy::RemoveResource(const std::string& name)
    {
        if (!m_Initialized)
        {
            OLO_CORE_ERROR("ResourceHierarchy not initialized");
            return false;
        }

        auto it = m_Resources.find(name);
        if (it == m_Resources.end())
        {
            OLO_CORE_WARN("Resource '{0}' not found for removal", name);
            return false;
        }

        const ResourceNode& node = it->second;

        // Remove from parent's children list
        if (!node.ParentName.empty())
        {
            RemoveFromParent(name, node.ParentName);
        }
        else
        {
            m_RootResources.erase(name);
        }

        // Remove children (make them root resources or remove entirely)
        for (const std::string& childName : node.ChildrenNames)
        {
            auto childIt = m_Resources.find(childName);
            if (childIt != m_Resources.end())
            {
                childIt->second.ParentName.clear();
                m_RootResources.insert(childName);
            }
        }

        // Remove dependencies
        for (const std::string& dependencyName : node.Dependencies)
        {
            auto depIt = m_Resources.find(dependencyName);
            if (depIt != m_Resources.end())
            {
                depIt->second.Dependents.erase(name);
            }
        }

        // Remove from dependents
        for (const std::string& dependentName : node.Dependents)
        {
            auto depIt = m_Resources.find(dependentName);
            if (depIt != m_Resources.end())
            {
                depIt->second.Dependencies.erase(name);
            }
        }

        // Remove the resource
        m_Resources.erase(it);

        OLO_CORE_TRACE("Removed resource '{0}'", name);
        return true;
    }

    bool ResourceHierarchy::AddDependency(const std::string& dependentName, const std::string& dependencyName)
    {
        if (!m_Initialized)
        {
            OLO_CORE_ERROR("ResourceHierarchy not initialized");
            return false;
        }

        auto dependentIt = m_Resources.find(dependentName);
        auto dependencyIt = m_Resources.find(dependencyName);

        if (dependentIt == m_Resources.end())
        {
            OLO_CORE_ERROR("Dependent resource '{0}' not found", dependentName);
            return false;
        }

        if (dependencyIt == m_Resources.end())
        {
            OLO_CORE_ERROR("Dependency resource '{0}' not found", dependencyName);
            return false;
        }

        // Check for circular dependency
        std::unordered_set<std::string> visited;
        std::unordered_set<std::string> recursionStack;
        
        // Temporarily add the dependency to check for cycles
        dependentIt->second.Dependencies.insert(dependencyName);
        dependencyIt->second.Dependents.insert(dependentName);

        bool hasCircularDep = HasCircularDependency(dependentName, visited, recursionStack);

        if (hasCircularDep)
        {
            // Remove the temporarily added dependency
            dependentIt->second.Dependencies.erase(dependencyName);
            dependencyIt->second.Dependents.erase(dependentName);
            
            OLO_CORE_ERROR("Circular dependency detected between '{0}' and '{1}'", dependentName, dependencyName);
            return false;
        }

        OLO_CORE_TRACE("Added dependency: '{0}' depends on '{1}'", dependentName, dependencyName);
        return true;
    }

    bool ResourceHierarchy::RemoveDependency(const std::string& dependentName, const std::string& dependencyName)
    {
        if (!m_Initialized)
        {
            OLO_CORE_ERROR("ResourceHierarchy not initialized");
            return false;
        }

        auto dependentIt = m_Resources.find(dependentName);
        auto dependencyIt = m_Resources.find(dependencyName);

        if (dependentIt == m_Resources.end() || dependencyIt == m_Resources.end())
        {
            OLO_CORE_WARN("One or both resources not found for dependency removal: '{0}' -> '{1}'",
                         dependentName, dependencyName);
            return false;
        }

        dependentIt->second.Dependencies.erase(dependencyName);
        dependencyIt->second.Dependents.erase(dependentName);

        OLO_CORE_TRACE("Removed dependency: '{0}' no longer depends on '{1}'", dependentName, dependencyName);
        return true;
    }

    const ResourceNode* ResourceHierarchy::GetResource(const std::string& name) const
    {
        auto it = m_Resources.find(name);
        return it != m_Resources.end() ? &it->second : nullptr;
    }

    std::vector<const ResourceNode*> ResourceHierarchy::GetResourcesByPriority(ResourcePriority priority) const
    {
        std::vector<const ResourceNode*> result;
        
        for (const auto& [name, node] : m_Resources)
        {
            if (node.Priority == priority)
            {
                result.push_back(&node);
            }
        }

        return result;
    }

    std::vector<const ResourceNode*> ResourceHierarchy::GetResourcesByScope(ResourceScope scope) const
    {
        std::vector<const ResourceNode*> result;
        
        for (const auto& [name, node] : m_Resources)
        {
            if (node.Scope == scope)
            {
                result.push_back(&node);
            }
        }

        return result;
    }

    std::vector<const ResourceNode*> ResourceHierarchy::GetChildren(const std::string& parentName) const
    {
        std::vector<const ResourceNode*> result;
        
        auto parentIt = m_Resources.find(parentName);
        if (parentIt == m_Resources.end())
            return result;

        for (const std::string& childName : parentIt->second.ChildrenNames)
        {
            auto childIt = m_Resources.find(childName);
            if (childIt != m_Resources.end())
            {
                result.push_back(&childIt->second);
            }
        }

        return result;
    }

    std::vector<const ResourceNode*> ResourceHierarchy::GetResourcesInDependencyOrder() const
    {
        std::vector<const ResourceNode*> result;
        
        for (const auto& [name, node] : m_Resources)
        {
            result.push_back(&node);
        }

        if (!TopologicalSort(result))
        {
            OLO_CORE_ERROR("Failed to sort resources - circular dependency detected");
            result.clear();
        }

        return result;
    }

    std::vector<const ResourceNode*> ResourceHierarchy::GetResourcesInBindingOrder() const
    {
        // First get all resources
        std::vector<const ResourceNode*> allResources;
        for (const auto& [name, node] : m_Resources)
        {
            allResources.push_back(&node);
        }

        // Sort by priority first, then by dependencies
        std::sort(allResources.begin(), allResources.end(),
            [](const ResourceNode* a, const ResourceNode* b) {
                if (a->Priority != b->Priority)
                    return a->Priority < b->Priority;
                return a->Name < b->Name; // Stable sort for same priority
            });

        // Within each priority group, sort by dependencies
        auto currentPriorityStart = allResources.begin();
        while (currentPriorityStart != allResources.end())
        {
            // Find the end of the current priority group
            auto currentPriorityEnd = std::find_if(currentPriorityStart, allResources.end(),
                [currentPriorityStart](const ResourceNode* node) {
                    return node->Priority != (*currentPriorityStart)->Priority;
                });

            // Sort this priority group by dependencies
            std::vector<const ResourceNode*> priorityGroup(currentPriorityStart, currentPriorityEnd);
            if (TopologicalSort(priorityGroup))
            {
                std::copy(priorityGroup.begin(), priorityGroup.end(), currentPriorityStart);
            }

            currentPriorityStart = currentPriorityEnd;
        }

        return allResources;
    }

    void ResourceHierarchy::ClearResourcesByScope(ResourceScope scope)
    {
        std::vector<std::string> toRemove;
        
        for (const auto& [name, node] : m_Resources)
        {
            if (node.Scope == scope)
            {
                toRemove.push_back(name);
            }
        }

        for (const std::string& name : toRemove)
        {
            RemoveResource(name);
        }

        OLO_CORE_TRACE("Cleared {0} resources with scope {1}", toRemove.size(), static_cast<u32>(scope));
    }

    void ResourceHierarchy::MarkResourceDirty(const std::string& name)
    {
        auto it = m_Resources.find(name);
        if (it != m_Resources.end())
        {
            it->second.IsDirty = true;
            it->second.LastModified = m_FrameNumber;
        }
    }

    void ResourceHierarchy::MarkDependentsDirty(const std::string& name)
    {
        auto it = m_Resources.find(name);
        if (it == m_Resources.end())
            return;

        // Recursively mark all dependents as dirty
        std::queue<std::string> toProcess;
        std::unordered_set<std::string> processed;

        toProcess.push(name);

        while (!toProcess.empty())
        {
            std::string currentName = toProcess.front();
            toProcess.pop();

            if (processed.find(currentName) != processed.end())
                continue;

            processed.insert(currentName);

            auto currentIt = m_Resources.find(currentName);
            if (currentIt == m_Resources.end())
                continue;

            // Mark all dependents as dirty and add them to processing queue
            for (const std::string& dependentName : currentIt->second.Dependents)
            {
                auto dependentIt = m_Resources.find(dependentName);
                if (dependentIt != m_Resources.end())
                {
                    dependentIt->second.IsDirty = true;
                    dependentIt->second.LastModified = m_FrameNumber;
                    toProcess.push(dependentName);
                }
            }
        }
    }

    std::vector<const ResourceNode*> ResourceHierarchy::GetDirtyResources() const
    {
        std::vector<const ResourceNode*> result;
        
        for (const auto& [name, node] : m_Resources)
        {
            if (node.IsDirty)
            {
                result.push_back(&node);
            }
        }

        return result;
    }

    void ResourceHierarchy::ClearDirtyFlags()
    {
        for (auto& [name, node] : m_Resources)
        {
            node.IsDirty = false;
        }
    }

    bool ResourceHierarchy::ValidateHierarchy() const
    {
        std::unordered_set<std::string> visited;
        std::unordered_set<std::string> recursionStack;

        for (const auto& [name, node] : m_Resources)
        {
            if (visited.find(name) == visited.end())
            {
                if (HasCircularDependency(name, visited, recursionStack))
                {
                    return false;
                }
            }
        }

        return true;
    }

    ResourceHierarchy::Statistics ResourceHierarchy::GetStatistics() const
    {
        Statistics stats;

        for (const auto& [name, node] : m_Resources)
        {
            stats.TotalResources++;

            switch (node.Priority)
            {
                case ResourcePriority::System:   stats.SystemResources++;   break;
                case ResourcePriority::Global:   stats.GlobalResources++;   break;
                case ResourcePriority::Material: stats.MaterialResources++; break;
                case ResourcePriority::Instance: stats.InstanceResources++; break;
                case ResourcePriority::Debug:    stats.DebugResources++;    break;
            }

            if (node.IsActive)
                stats.ActiveResources++;

            if (node.IsDirty)
                stats.DirtyResources++;

            stats.TotalDependencies += static_cast<u32>(node.Dependencies.size());
        }

        stats.MaxDepth = CalculateMaxDepth();

        return stats;
    }

    const char* ResourceHierarchy::GetPriorityString(ResourcePriority priority)
    {
        switch (priority)
        {
            case ResourcePriority::System:   return "System";
            case ResourcePriority::Global:   return "Global";
            case ResourcePriority::Material: return "Material";
            case ResourcePriority::Instance: return "Instance";
            case ResourcePriority::Debug:    return "Debug";
            default:                         return "Unknown";
        }
    }

    const char* ResourceHierarchy::GetScopeString(ResourceScope scope)
    {
        switch (scope)
        {
            case ResourceScope::Frame:      return "Frame";
            case ResourceScope::Scene:      return "Scene";
            case ResourceScope::Global:     return "Global";
            case ResourceScope::Persistent: return "Persistent";
            default:                        return "Unknown";
        }
    }

    void ResourceHierarchy::RenderDebugInterface()
    {
        if (!m_Initialized)
            return;

        ImGui::Text("Resource Hierarchy (%zu resources)", m_Resources.size());
        ImGui::Separator();

        // Statistics
        auto stats = GetStatistics();
        if (ImGui::CollapsingHeader("Statistics", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Total Resources: %u", stats.TotalResources);
            ImGui::Text("Active Resources: %u", stats.ActiveResources);
            ImGui::Text("Dirty Resources: %u", stats.DirtyResources);
            ImGui::Text("Total Dependencies: %u", stats.TotalDependencies);
            ImGui::Text("Maximum Depth: %u", stats.MaxDepth);
            
            ImGui::Separator();
            ImGui::Text("By Priority:");
            ImGui::BulletText("System: %u", stats.SystemResources);
            ImGui::BulletText("Global: %u", stats.GlobalResources);
            ImGui::BulletText("Material: %u", stats.MaterialResources);
            ImGui::BulletText("Instance: %u", stats.InstanceResources);
            ImGui::BulletText("Debug: %u", stats.DebugResources);
        }

        // Resource list
        if (ImGui::CollapsingHeader("Resources", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // Filter options
            static int priorityFilter = -1; // -1 means all
            static int scopeFilter = -1;
            static bool showOnlyActive = false;
            static bool showOnlyDirty = false;

            ImGui::PushItemWidth(150);
            const char* priorityItems[] = { "All", "System", "Global", "Material", "Instance", "Debug" };
            ImGui::Combo("Priority Filter", &priorityFilter, priorityItems, IM_ARRAYSIZE(priorityItems));
            
            ImGui::SameLine();
            const char* scopeItems[] = { "All", "Frame", "Scene", "Global", "Persistent" };
            ImGui::Combo("Scope Filter", &scopeFilter, scopeItems, IM_ARRAYSIZE(scopeItems));
            ImGui::PopItemWidth();

            ImGui::Checkbox("Show Only Active", &showOnlyActive);
            ImGui::SameLine();
            ImGui::Checkbox("Show Only Dirty", &showOnlyDirty);

            ImGui::Separator();

            // Resource table
            if (ImGui::BeginTable("ResourceTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
            {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Priority");
                ImGui::TableSetupColumn("Scope");
                ImGui::TableSetupColumn("Status");
                ImGui::TableSetupColumn("Dependencies");
                ImGui::TableHeadersRow();

                for (const auto& [name, node] : m_Resources)
                {
                    // Apply filters
                    if (priorityFilter >= 0 && static_cast<int>(node.Priority) != priorityFilter - 1)
                        continue;
                    if (scopeFilter >= 0 && static_cast<int>(node.Scope) != scopeFilter - 1)
                        continue;
                    if (showOnlyActive && !node.IsActive)
                        continue;
                    if (showOnlyDirty && !node.IsDirty)
                        continue;

                    ImGui::TableNextRow();
                    
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%s", node.Name.c_str());
                    
                    ImGui::TableSetColumnIndex(1);
                    // Display resource type
                    const char* typeStr = "Unknown";
                    switch (node.Type)
                    {
                        case ShaderResourceType::UniformBuffer: typeStr = "UBO"; break;
                        case ShaderResourceType::StorageBuffer: typeStr = "SSBO"; break;
                        case ShaderResourceType::Texture2D: typeStr = "Tex2D"; break;
                        case ShaderResourceType::TextureCube: typeStr = "TexCube"; break;
                        case ShaderResourceType::UniformBufferArray: typeStr = "UBO[]"; break;
                        case ShaderResourceType::StorageBufferArray: typeStr = "SSBO[]"; break;
                        case ShaderResourceType::Texture2DArray: typeStr = "Tex2D[]"; break;
                        case ShaderResourceType::TextureCubeArray: typeStr = "TexCube[]"; break;
                        default: break;
                    }
                    ImGui::Text("%s", typeStr);
                    
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%s", GetPriorityString(node.Priority));
                    
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%s", GetScopeString(node.Scope));
                    
                    ImGui::TableSetColumnIndex(4);
                    if (node.IsActive)
                    {
                        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Active");
                    }
                    else
                    {
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Inactive");
                    }
                    
                    if (node.IsDirty)
                    {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Dirty");
                    }
                    
                    ImGui::TableSetColumnIndex(5);
                    ImGui::Text("%zu", node.Dependencies.size());
                }
                
                ImGui::EndTable();
            }
        }

        // Frame counter
        ImGui::Separator();
        ImGui::Text("Frame: %zu", m_FrameNumber);
        if (ImGui::Button("Next Frame"))
        {
            m_FrameNumber++;
        }
    }

    // Private methods implementation

    bool ResourceHierarchy::TopologicalSort(std::vector<const ResourceNode*>& nodes) const
    {
        // Kahn's algorithm for topological sorting
        std::unordered_map<std::string, u32> inDegree;
        std::unordered_map<std::string, std::vector<std::string>> adjList;

        // Initialize in-degree count and adjacency list
        for (const ResourceNode* node : nodes)
        {
            inDegree[node->Name] = 0;
            adjList[node->Name] = std::vector<std::string>();
        }

        // Build the graph and calculate in-degrees
        for (const ResourceNode* node : nodes)
        {
            for (const std::string& dependency : node->Dependencies)
            {
                // Only consider dependencies that are in our current set of nodes
                if (inDegree.find(dependency) != inDegree.end())
                {
                    adjList[dependency].push_back(node->Name);
                    inDegree[node->Name]++;
                }
            }
        }

        // Find all nodes with no incoming edges
        std::queue<std::string> zeroInDegree;
        for (const auto& [name, degree] : inDegree)
        {
            if (degree == 0)
            {
                zeroInDegree.push(name);
            }
        }

        std::vector<const ResourceNode*> result;
        std::unordered_map<std::string, const ResourceNode*> nameToNode;
        
        for (const ResourceNode* node : nodes)
        {
            nameToNode[node->Name] = node;
        }

        // Process nodes in topological order
        while (!zeroInDegree.empty())
        {
            std::string current = zeroInDegree.front();
            zeroInDegree.pop();
            
            result.push_back(nameToNode[current]);

            // Update in-degrees of neighbors
            for (const std::string& neighbor : adjList[current])
            {
                inDegree[neighbor]--;
                if (inDegree[neighbor] == 0)
                {
                    zeroInDegree.push(neighbor);
                }
            }
        }

        // Check if we processed all nodes (no cycles)
        if (result.size() != nodes.size())
        {
            return false; // Circular dependency detected
        }

        nodes = std::move(result);
        return true;
    }

    bool ResourceHierarchy::HasCircularDependency(const std::string& nodeName,
                                                 std::unordered_set<std::string>& visited,
                                                 std::unordered_set<std::string>& recursionStack) const
    {
        auto nodeIt = m_Resources.find(nodeName);
        if (nodeIt == m_Resources.end())
            return false;

        visited.insert(nodeName);
        recursionStack.insert(nodeName);

        // Check all dependencies
        for (const std::string& dependency : nodeIt->second.Dependencies)
        {
            if (recursionStack.find(dependency) != recursionStack.end())
            {
                return true; // Back edge found - circular dependency
            }

            if (visited.find(dependency) == visited.end())
            {
                if (HasCircularDependency(dependency, visited, recursionStack))
                {
                    return true;
                }
            }
        }

        recursionStack.erase(nodeName);
        return false;
    }

    void ResourceHierarchy::RemoveFromParent(const std::string& childName, const std::string& parentName)
    {
        auto parentIt = m_Resources.find(parentName);
        if (parentIt != m_Resources.end())
        {
            auto& children = parentIt->second.ChildrenNames;
            children.erase(std::remove(children.begin(), children.end(), childName), children.end());
        }
    }

    u32 ResourceHierarchy::CalculateMaxDepth() const
    {
        u32 maxDepth = 0;
        std::unordered_set<std::string> visited;

        for (const std::string& rootName : m_RootResources)
        {
            visited.clear();
            u32 depth = CalculateNodeDepth(rootName, visited);
            maxDepth = std::max(maxDepth, depth);
        }

        return maxDepth;
    }

    u32 ResourceHierarchy::CalculateNodeDepth(const std::string& nodeName,
                                             std::unordered_set<std::string>& visited) const
    {
        if (visited.find(nodeName) != visited.end())
            return 0; // Avoid infinite recursion

        visited.insert(nodeName);

        auto nodeIt = m_Resources.find(nodeName);
        if (nodeIt == m_Resources.end())
            return 0;

        u32 maxChildDepth = 0;
        for (const std::string& childName : nodeIt->second.ChildrenNames)
        {
            u32 childDepth = CalculateNodeDepth(childName, visited);
            maxChildDepth = std::max(maxChildDepth, childDepth);
        }

        return maxChildDepth + 1;
    }
}
