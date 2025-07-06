#include "OloEnginePCH.h"
#include "DeferredResourceResolver.h"
#include "OloEngine/Core/Log.h"

#include <imgui.h>
#include <algorithm>

namespace OloEngine
{
    void DeferredResourceResolver::Initialize()
    {
        if (m_Initialized)
        {
            OLO_CORE_WARN("DeferredResourceResolver already initialized");
            return;
        }

        m_PendingRequests.clear();
        m_ResolvedResources.clear();
        m_Batches.clear();
        m_CurrentFrame = 0;
        m_Initialized = true;

        OLO_CORE_TRACE("DeferredResourceResolver initialized");
    }

    void DeferredResourceResolver::Shutdown()
    {
        m_PendingRequests.clear();
        m_ResolvedResources.clear();
        m_Batches.clear();
        m_TargetHierarchy = nullptr;
        m_Initialized = false;

        OLO_CORE_TRACE("DeferredResourceResolver shutdown");
    }

    bool DeferredResourceResolver::RequestResource(const std::string& name, ShaderResourceType expectedType,
                                                 ResourceResolverFunction resolver,
                                                 ResolutionStrategy strategy,
                                                 const std::string& requesterName)
    {
        if (!m_Initialized)
        {
            OLO_CORE_ERROR("DeferredResourceResolver not initialized");
            return false;
        }

        if (name.empty() || !resolver)
        {
            OLO_CORE_ERROR("Invalid resource request: name='{0}', resolver={1}", name, resolver ? "valid" : "null");
            return false;
        }

        // Check if request already exists
        if (m_PendingRequests.find(name) != m_PendingRequests.end())
        {
            OLO_CORE_WARN("Resource request '{0}' already exists", name);
            return false;
        }

        // Check if already resolved
        if (m_ResolvedResources.find(name) != m_ResolvedResources.end())
        {
            OLO_CORE_WARN("Resource '{0}' already resolved", name);
            return false;
        }

        // Create the request
        DeferredResourceRequest request(name, expectedType, strategy);
        request.RequesterName = requesterName;
        request.FrameRequested = m_CurrentFrame;
        request.Resolver = std::move(resolver);

        m_PendingRequests[name] = std::move(request);

        OLO_CORE_TRACE("Queued resource request '{0}' (type: {1}, strategy: {2}, requester: '{3}')",
                      name, static_cast<u32>(expectedType), static_cast<u32>(strategy), requesterName);

        // If immediate strategy, try to resolve now
        if (strategy == ResolutionStrategy::Immediate)
        {
            ResolveRequest(name);
        }

        return true;
    }

    bool DeferredResourceResolver::RequestResourceWithDependencies(const std::string& name, ShaderResourceType expectedType,
                                                                 const std::unordered_set<std::string>& dependencies,
                                                                 ResourceResolverFunction resolver,
                                                                 ResolutionStrategy strategy,
                                                                 const std::string& requesterName)
    {
        if (!RequestResource(name, expectedType, std::move(resolver), strategy, requesterName))
        {
            return false;
        }

        // Add dependencies
        auto& request = m_PendingRequests[name];
        request.Dependencies = dependencies;

        // Update dependents in dependency requests
        for (const std::string& dependencyName : dependencies)
        {
            auto depIt = m_PendingRequests.find(dependencyName);
            if (depIt != m_PendingRequests.end())
            {
                depIt->second.Dependents.insert(name);
            }
        }

        OLO_CORE_TRACE("Resource request '{0}' has {1} dependencies", name, dependencies.size());
        return true;
    }

    bool DeferredResourceResolver::CancelRequest(const std::string& name)
    {
        auto it = m_PendingRequests.find(name);
        if (it == m_PendingRequests.end())
        {
            return false;
        }

        const DeferredResourceRequest& request = it->second;

        // Remove from dependents of dependencies
        for (const std::string& dependencyName : request.Dependencies)
        {
            auto depIt = m_PendingRequests.find(dependencyName);
            if (depIt != m_PendingRequests.end())
            {
                depIt->second.Dependents.erase(name);
            }
        }

        // Update dependents to remove this request from their dependencies
        for (const std::string& dependentName : request.Dependents)
        {
            auto depIt = m_PendingRequests.find(dependentName);
            if (depIt != m_PendingRequests.end())
            {
                depIt->second.Dependencies.erase(name);
            }
        }

        m_PendingRequests.erase(it);

        OLO_CORE_TRACE("Cancelled resource request '{0}'", name);
        return true;
    }

    bool DeferredResourceResolver::IsRequestPending(const std::string& name) const
    {
        return m_PendingRequests.find(name) != m_PendingRequests.end();
    }

    ResolutionStatus DeferredResourceResolver::GetRequestStatus(const std::string& name) const
    {
        auto it = m_PendingRequests.find(name);
        if (it != m_PendingRequests.end())
        {
            return it->second.Status;
        }

        // Check if resolved
        if (m_ResolvedResources.find(name) != m_ResolvedResources.end())
        {
            return ResolutionStatus::Resolved;
        }

        return ResolutionStatus::Failed; // Not found anywhere
    }

    const ShaderResourceInput* DeferredResourceResolver::GetResolvedResource(const std::string& name) const
    {
        auto it = m_ResolvedResources.find(name);
        return it != m_ResolvedResources.end() ? &it->second : nullptr;
    }

    bool DeferredResourceResolver::CreateBatch(const std::string& batchName, ResolutionStrategy strategy)
    {
        if (!m_Initialized)
        {
            OLO_CORE_ERROR("DeferredResourceResolver not initialized");
            return false;
        }

        if (m_Batches.find(batchName) != m_Batches.end())
        {
            OLO_CORE_WARN("Batch '{0}' already exists", batchName);
            return false;
        }

        ResourceBatch batch(batchName, strategy);
        batch.FrameScheduled = m_CurrentFrame;
        m_Batches[batchName] = std::move(batch);

        OLO_CORE_TRACE("Created resource batch '{0}' with strategy {1}", batchName, static_cast<u32>(strategy));
        return true;
    }

    bool DeferredResourceResolver::AddRequestToBatch(const std::string& batchName, const std::string& requestName)
    {
        auto batchIt = m_Batches.find(batchName);
        if (batchIt == m_Batches.end())
        {
            OLO_CORE_ERROR("Batch '{0}' not found", batchName);
            return false;
        }

        auto requestIt = m_PendingRequests.find(requestName);
        if (requestIt == m_PendingRequests.end())
        {
            OLO_CORE_ERROR("Request '{0}' not found", requestName);
            return false;
        }

        batchIt->second.RequestNames.push_back(requestName);
        OLO_CORE_TRACE("Added request '{0}' to batch '{1}'", requestName, batchName);
        return true;
    }

    bool DeferredResourceResolver::ExecuteBatch(const std::string& batchName)
    {
        auto batchIt = m_Batches.find(batchName);
        if (batchIt == m_Batches.end())
        {
            OLO_CORE_ERROR("Batch '{0}' not found", batchName);
            return false;
        }

        ResourceBatch& batch = batchIt->second;
        if (batch.IsExecuting)
        {
            OLO_CORE_WARN("Batch '{0}' is already executing", batchName);
            return false;
        }

        batch.IsExecuting = true;

        // Sort requests by dependencies
        std::vector<std::string> sortedRequests = batch.RequestNames;
        if (!SortByDependencies(sortedRequests))
        {
            OLO_CORE_ERROR("Failed to resolve dependencies for batch '{0}' - circular dependency detected", batchName);
            batch.IsExecuting = false;
            return false;
        }

        // Resolve requests in dependency order
        u32 resolvedCount = 0;
        for (const std::string& requestName : sortedRequests)
        {
            if (ResolveRequest(requestName))
            {
                resolvedCount++;
            }
        }

        batch.IsExecuting = false;

        OLO_CORE_TRACE("Executed batch '{0}': {1}/{2} requests resolved", batchName, resolvedCount, sortedRequests.size());
        return true;
    }

    u32 DeferredResourceResolver::ResolveImmediateRequests()
    {
        return static_cast<u32>(GetResolvableRequests(ResolutionStrategy::Immediate).size());
    }

    u32 DeferredResourceResolver::ResolveFrameEndRequests()
    {
        std::vector<std::string> resolvableRequests = GetResolvableRequests(ResolutionStrategy::FrameEnd);
        SortByDependencies(resolvableRequests);

        u32 resolvedCount = 0;
        for (const std::string& requestName : resolvableRequests)
        {
            if (ResolveRequest(requestName))
            {
                resolvedCount++;
            }
        }

        OLO_CORE_TRACE("Resolved {0} frame-end requests", resolvedCount);
        return resolvedCount;
    }

    u32 DeferredResourceResolver::ResolveNextFrameRequests()
    {
        std::vector<std::string> resolvableRequests = GetResolvableRequests(ResolutionStrategy::NextFrame);
        SortByDependencies(resolvableRequests);

        u32 resolvedCount = 0;
        for (const std::string& requestName : resolvableRequests)
        {
            if (ResolveRequest(requestName))
            {
                resolvedCount++;
            }
        }

        OLO_CORE_TRACE("Resolved {0} next-frame requests", resolvedCount);
        return resolvedCount;
    }

    u32 DeferredResourceResolver::ResolveLazyRequests(const std::unordered_set<std::string>& requestedNames)
    {
        std::vector<std::string> lazyRequests = GetResolvableRequests(ResolutionStrategy::Lazy);
        
        // Filter to only the requested names
        lazyRequests.erase(std::remove_if(lazyRequests.begin(), lazyRequests.end(),
            [&requestedNames](const std::string& name) {
                return requestedNames.find(name) == requestedNames.end();
            }), lazyRequests.end());

        SortByDependencies(lazyRequests);

        u32 resolvedCount = 0;
        for (const std::string& requestName : lazyRequests)
        {
            if (ResolveRequest(requestName))
            {
                resolvedCount++;
            }
        }

        OLO_CORE_TRACE("Resolved {0} lazy requests", resolvedCount);
        return resolvedCount;
    }

    u32 DeferredResourceResolver::ProcessBackgroundResolution()
    {
        std::lock_guard<std::mutex> lock(m_BackgroundMutex);
        
        u32 resolvedCount = 0;
        while (!m_BackgroundQueue.empty())
        {
            std::string requestName = m_BackgroundQueue.front();
            m_BackgroundQueue.pop();

            if (ResolveRequest(requestName))
            {
                resolvedCount++;
            }
        }

        return resolvedCount;
    }

    void DeferredResourceResolver::ClearResolvedResources()
    {
        m_ResolvedResources.clear();
        OLO_CORE_TRACE("Cleared resolved resources");
    }

    void DeferredResourceResolver::ClearAllRequests()
    {
        m_PendingRequests.clear();
        m_ResolvedResources.clear();
        m_Batches.clear();
        OLO_CORE_TRACE("Cleared all requests and resolved resources");
    }

    void DeferredResourceResolver::NextFrame()
    {
        m_CurrentFrame++;
        
        // Move background strategy requests to background queue
        for (const auto& [name, request] : m_PendingRequests)
        {
            if (request.Strategy == ResolutionStrategy::Background && request.Status == ResolutionStatus::Pending)
            {
                std::lock_guard<std::mutex> lock(m_BackgroundMutex);
                m_BackgroundQueue.push(name);
            }
        }
    }

    DeferredResourceResolver::Statistics DeferredResourceResolver::GetStatistics() const
    {
        Statistics stats;
        stats.CurrentFrame = m_CurrentFrame;

        for (const auto& [name, request] : m_PendingRequests)
        {
            switch (request.Status)
            {
                case ResolutionStatus::Pending:   stats.PendingRequests++;   break;
                case ResolutionStatus::Resolving: stats.ResolvingRequests++; break;
                case ResolutionStatus::Resolved:  stats.ResolvedRequests++;  break;
                case ResolutionStatus::Failed:    stats.FailedRequests++;    break;
                case ResolutionStatus::Deferred:  stats.DeferredRequests++;  break;
            }

            stats.TotalDependencies += static_cast<u32>(request.Dependencies.size());
        }

        stats.ResolvedRequests += static_cast<u32>(m_ResolvedResources.size());
        stats.TotalBatches = static_cast<u32>(m_Batches.size());

        for (const auto& [name, batch] : m_Batches)
        {
            if (batch.IsExecuting)
                stats.ExecutingBatches++;
        }

        stats.AverageResolutionTimeMs = CalculateAverageResolutionTime();

        return stats;
    }

    const char* DeferredResourceResolver::GetStatusString(ResolutionStatus status)
    {
        switch (status)
        {
            case ResolutionStatus::Pending:   return "Pending";
            case ResolutionStatus::Resolving: return "Resolving";
            case ResolutionStatus::Resolved:  return "Resolved";
            case ResolutionStatus::Failed:    return "Failed";
            case ResolutionStatus::Deferred:  return "Deferred";
            default:                          return "Unknown";
        }
    }

    const char* DeferredResourceResolver::GetStrategyString(ResolutionStrategy strategy)
    {
        switch (strategy)
        {
            case ResolutionStrategy::Immediate:  return "Immediate";
            case ResolutionStrategy::FrameEnd:   return "Frame End";
            case ResolutionStrategy::NextFrame:  return "Next Frame";
            case ResolutionStrategy::Lazy:       return "Lazy";
            case ResolutionStrategy::Background: return "Background";
            default:                             return "Unknown";
        }
    }

    void DeferredResourceResolver::RenderDebugInterface()
    {
        if (!m_Initialized)
            return;

        ImGui::Text("Deferred Resource Resolver");
        ImGui::Separator();

        // Statistics
        auto stats = GetStatistics();
        if (ImGui::CollapsingHeader("Statistics", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Current Frame: %u", stats.CurrentFrame);
            ImGui::Text("Pending: %u", stats.PendingRequests);
            ImGui::Text("Resolving: %u", stats.ResolvingRequests);
            ImGui::Text("Resolved: %u", stats.ResolvedRequests);
            ImGui::Text("Failed: %u", stats.FailedRequests);
            ImGui::Text("Deferred: %u", stats.DeferredRequests);
            ImGui::Text("Total Batches: %u", stats.TotalBatches);
            ImGui::Text("Executing Batches: %u", stats.ExecutingBatches);
            ImGui::Text("Total Dependencies: %u", stats.TotalDependencies);
            ImGui::Text("Avg Resolution Time: %.2f ms", stats.AverageResolutionTimeMs);
        }

        // Pending requests
        if (ImGui::CollapsingHeader("Pending Requests"))
        {
            if (ImGui::BeginTable("PendingRequestsTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Strategy");
                ImGui::TableSetupColumn("Status");
                ImGui::TableSetupColumn("Dependencies");
                ImGui::TableSetupColumn("Attempts");
                ImGui::TableHeadersRow();

                for (const auto& [name, request] : m_PendingRequests)
                {
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%s", request.Name.c_str());

                    ImGui::TableSetColumnIndex(1);
                    // Display resource type
                    const char* typeStr = "Unknown";
                    switch (request.ExpectedType)
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
                    ImGui::Text("%s", GetStrategyString(request.Strategy));

                    ImGui::TableSetColumnIndex(3);
                    const char* statusStr = GetStatusString(request.Status);
                    ImVec4 statusColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                    
                    switch (request.Status)
                    {
                        case ResolutionStatus::Pending:   statusColor = ImVec4(1.0f, 1.0f, 0.0f, 1.0f); break;
                        case ResolutionStatus::Resolving: statusColor = ImVec4(0.0f, 0.0f, 1.0f, 1.0f); break;
                        case ResolutionStatus::Resolved:  statusColor = ImVec4(0.0f, 1.0f, 0.0f, 1.0f); break;
                        case ResolutionStatus::Failed:    statusColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f); break;
                        case ResolutionStatus::Deferred:  statusColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f); break;
                    }
                    
                    ImGui::TextColored(statusColor, "%s", statusStr);

                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%zu", request.Dependencies.size());

                    ImGui::TableSetColumnIndex(5);
                    ImGui::Text("%u/%u", request.AttemptCount, request.MaxAttempts);
                }

                ImGui::EndTable();
            }
        }

        // Resolved resources
        if (ImGui::CollapsingHeader("Resolved Resources"))
        {
            ImGui::Text("Total Resolved: %zu", m_ResolvedResources.size());
            
            for (const auto& [name, resource] : m_ResolvedResources)
            {
                ImGui::BulletText("%s", name.c_str());
            }
        }

        // Batches
        if (ImGui::CollapsingHeader("Batches"))
        {
            for (const auto& [name, batch] : m_Batches)
            {
                if (ImGui::TreeNode(batch.Name.c_str()))
                {
                    ImGui::Text("Strategy: %s", GetStrategyString(batch.Strategy));
                    ImGui::Text("Frame Scheduled: %u", batch.FrameScheduled);
                    ImGui::Text("Is Executing: %s", batch.IsExecuting ? "Yes" : "No");
                    ImGui::Text("Requests (%zu):", batch.RequestNames.size());
                    
                    ImGui::Indent();
                    for (const std::string& requestName : batch.RequestNames)
                    {
                        ImGui::BulletText("%s", requestName.c_str());
                    }
                    ImGui::Unindent();
                    
                    ImGui::TreePop();
                }
            }
        }

        // Controls
        ImGui::Separator();
        if (ImGui::Button("Resolve Frame End"))
        {
            ResolveFrameEndRequests();
        }
        ImGui::SameLine();
        if (ImGui::Button("Resolve Next Frame"))
        {
            ResolveNextFrameRequests();
        }
        ImGui::SameLine();
        if (ImGui::Button("Next Frame"))
        {
            NextFrame();
        }
    }

    // Private methods implementation

    bool DeferredResourceResolver::ResolveRequest(const std::string& requestName)
    {
        auto it = m_PendingRequests.find(requestName);
        if (it == m_PendingRequests.end())
        {
            return false;
        }

        DeferredResourceRequest& request = it->second;

        // Check if already resolved or resolving
        if (request.Status == ResolutionStatus::Resolved || request.Status == ResolutionStatus::Resolving)
        {
            return request.Status == ResolutionStatus::Resolved;
        }

        // Check if dependencies are satisfied
        if (!AreDependenciesResolved(request))
        {
            request.Status = ResolutionStatus::Deferred;
            return false;
        }

        // Check attempt limit
        if (request.AttemptCount >= request.MaxAttempts)
        {
            request.Status = ResolutionStatus::Failed;
            request.ErrorMessage = "Maximum resolution attempts exceeded";
            OLO_CORE_ERROR("Resource '{0}' failed to resolve after {1} attempts", requestName, request.MaxAttempts);
            return false;
        }

        // Mark as resolving
        request.Status = ResolutionStatus::Resolving;
        request.AttemptCount++;

        // Call the resolver function
        ShaderResourceInput resolvedResource;
        bool success = false;
        
        try
        {
            success = request.Resolver(requestName, resolvedResource);
        }
        catch (const std::exception& e)
        {
            request.ErrorMessage = e.what();
            OLO_CORE_ERROR("Exception during resource resolution for '{0}': {1}", requestName, e.what());
        }

        if (success)
        {
            // Validate the resolved resource type
            if (resolvedResource.Type != request.ExpectedType)
            {
                request.Status = ResolutionStatus::Failed;
                request.ErrorMessage = "Resolved resource type does not match expected type";
                OLO_CORE_ERROR("Resource '{0}' type mismatch: expected {1}, got {2}",
                              requestName, static_cast<u32>(request.ExpectedType), static_cast<u32>(resolvedResource.Type));
                return false;
            }

            // Store the resolved resource
            request.Status = ResolutionStatus::Resolved;
            request.ResolvedResource = resolvedResource;
            request.ResolveTime = std::chrono::steady_clock::now();

            m_ResolvedResources[requestName] = resolvedResource;

            // Add to target hierarchy if available
            AddToTargetHierarchy(requestName, resolvedResource);

            // Update dependents
            UpdateDependents(requestName);

            // Remove from pending requests
            m_PendingRequests.erase(it);

            OLO_CORE_TRACE("Successfully resolved resource '{0}'", requestName);
            return true;
        }
        else
        {
            request.Status = ResolutionStatus::Failed;
            if (request.ErrorMessage.empty())
            {
                request.ErrorMessage = "Resolver function returned false";
            }
            
            OLO_CORE_ERROR("Failed to resolve resource '{0}': {1}", requestName, request.ErrorMessage);
            return false;
        }
    }

    bool DeferredResourceResolver::AreDependenciesResolved(const DeferredResourceRequest& request) const
    {
        for (const std::string& dependencyName : request.Dependencies)
        {
            // Check if dependency is resolved
            if (m_ResolvedResources.find(dependencyName) == m_ResolvedResources.end())
            {
                // Check if dependency is still pending
                auto depIt = m_PendingRequests.find(dependencyName);
                if (depIt != m_PendingRequests.end() && depIt->second.Status != ResolutionStatus::Resolved)
                {
                    return false;
                }
            }
        }
        return true;
    }

    std::vector<std::string> DeferredResourceResolver::GetResolvableRequests(ResolutionStrategy strategy) const
    {
        std::vector<std::string> result;

        for (const auto& [name, request] : m_PendingRequests)
        {
            if (request.Strategy == strategy && 
                request.Status == ResolutionStatus::Pending &&
                AreDependenciesResolved(request))
            {
                result.push_back(name);
            }
        }

        return result;
    }

    void DeferredResourceResolver::UpdateDependents(const std::string& resolvedRequestName)
    {
        auto it = m_PendingRequests.find(resolvedRequestName);
        if (it == m_PendingRequests.end())
            return;

        // Update all dependents that this request has been resolved
        for (const std::string& dependentName : it->second.Dependents)
        {
            auto depIt = m_PendingRequests.find(dependentName);
            if (depIt != m_PendingRequests.end())
            {
                // If all dependencies are now resolved, try to resolve this dependent
                if (AreDependenciesResolved(depIt->second) && depIt->second.Status == ResolutionStatus::Deferred)
                {
                    depIt->second.Status = ResolutionStatus::Pending;
                }
            }
        }
    }

    bool DeferredResourceResolver::SortByDependencies(std::vector<std::string>& requestNames) const
    {
        // Simple topological sort using Kahn's algorithm
        std::unordered_map<std::string, u32> inDegree;
        std::unordered_map<std::string, std::vector<std::string>> adjList;

        // Initialize
        for (const std::string& name : requestNames)
        {
            inDegree[name] = 0;
            adjList[name] = std::vector<std::string>();
        }

        // Build graph
        for (const std::string& name : requestNames)
        {
            auto requestIt = m_PendingRequests.find(name);
            if (requestIt != m_PendingRequests.end())
            {
                for (const std::string& dependency : requestIt->second.Dependencies)
                {
                    if (inDegree.find(dependency) != inDegree.end()) // Only consider dependencies in our set
                    {
                        adjList[dependency].push_back(name);
                        inDegree[name]++;
                    }
                }
            }
        }

        // Process
        std::queue<std::string> zeroInDegree;
        for (const auto& [name, degree] : inDegree)
        {
            if (degree == 0)
            {
                zeroInDegree.push(name);
            }
        }

        std::vector<std::string> result;
        while (!zeroInDegree.empty())
        {
            std::string current = zeroInDegree.front();
            zeroInDegree.pop();
            result.push_back(current);

            for (const std::string& neighbor : adjList[current])
            {
                inDegree[neighbor]--;
                if (inDegree[neighbor] == 0)
                {
                    zeroInDegree.push(neighbor);
                }
            }
        }

        if (result.size() != requestNames.size())
        {
            return false; // Circular dependency
        }

        requestNames = std::move(result);
        return true;
    }

    void DeferredResourceResolver::AddToTargetHierarchy(const std::string& name, const ShaderResourceInput& resource)
    {
        if (m_TargetHierarchy)
        {
            m_TargetHierarchy->SetResource(name, resource);
        }
    }

    f32 DeferredResourceResolver::CalculateAverageResolutionTime() const
    {
        if (m_ResolvedResources.empty())
            return 0.0f;

        f64 totalTime = 0.0;
        u32 count = 0;

        for (const auto& [name, request] : m_PendingRequests)
        {
            if (request.Status == ResolutionStatus::Resolved)
            {
                auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                    request.ResolveTime - request.RequestTime);
                totalTime += duration.count() / 1000.0; // Convert to milliseconds
                count++;
            }
        }

        return count > 0 ? static_cast<f32>(totalTime / count) : 0.0f;
    }
}
