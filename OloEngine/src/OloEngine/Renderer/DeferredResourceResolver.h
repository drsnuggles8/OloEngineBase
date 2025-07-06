#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/UniformBufferRegistry.h"
#include "OloEngine/Renderer/ResourceHierarchy.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <memory>
#include <queue>

namespace OloEngine
{
    /**
     * @brief Resource resolution status
     */
    enum class ResolutionStatus : u8
    {
        Pending = 0,        // Resource not yet resolved
        Resolving = 1,      // Currently being resolved
        Resolved = 2,       // Successfully resolved
        Failed = 3,         // Resolution failed
        Deferred = 4        // Resolution deferred to later
    };

    /**
     * @brief Resource resolution strategy
     */
    enum class ResolutionStrategy : u8
    {
        Immediate = 0,      // Resolve immediately when requested
        FrameEnd = 1,       // Resolve at the end of the current frame
        NextFrame = 2,      // Resolve at the beginning of next frame
        Lazy = 3,           // Resolve only when actually needed
        Background = 4      // Resolve in background thread
    };

    /**
     * @brief Forward declaration for resource resolver function
     */
    class DeferredResourceResolver;
    using ResourceResolverFunction = std::function<bool(const std::string&, ShaderResourceInput&)>;

    /**
     * @brief Information about a deferred resource request
     */
    struct DeferredResourceRequest
    {
        std::string Name;
        ShaderResourceType ExpectedType = ShaderResourceType::None;
        ResolutionStrategy Strategy = ResolutionStrategy::FrameEnd;
        ResolutionStatus Status = ResolutionStatus::Pending;
        
        // Context information
        std::string RequesterName;                          // Name of the shader/system requesting this resource
        ResourcePriority Priority = ResourcePriority::Instance;
        u32 FrameRequested = 0;                            // Frame number when requested
        u32 AttemptCount = 0;                              // Number of resolution attempts
        u32 MaxAttempts = 3;                               // Maximum attempts before giving up
        
        // Resolution function
        ResourceResolverFunction Resolver;
        
        // Dependencies
        std::unordered_set<std::string> Dependencies;      // Resources this request depends on
        std::unordered_set<std::string> Dependents;        // Other requests waiting for this one
        
        // Result
        ShaderResourceInput ResolvedResource;
        std::string ErrorMessage;
        
        // Timing
        std::chrono::steady_clock::time_point RequestTime;
        std::chrono::steady_clock::time_point ResolveTime;
        
        DeferredResourceRequest() = default;
        DeferredResourceRequest(const std::string& name, ShaderResourceType type,
                              ResolutionStrategy strategy = ResolutionStrategy::FrameEnd)
            : Name(name), ExpectedType(type), Strategy(strategy),
              RequestTime(std::chrono::steady_clock::now()) {}
    };

    /**
     * @brief Batch of resource requests to be resolved together
     */
    struct ResourceBatch
    {
        std::string Name;
        std::vector<std::string> RequestNames;
        ResolutionStrategy Strategy = ResolutionStrategy::FrameEnd;
        ResourcePriority Priority = ResourcePriority::Instance;
        u32 FrameScheduled = 0;
        bool IsExecuting = false;
        
        ResourceBatch() = default;
        ResourceBatch(const std::string& name, ResolutionStrategy strategy = ResolutionStrategy::FrameEnd)
            : Name(name), Strategy(strategy) {}
    };

    /**
     * @brief Deferred resource resolution system
     * 
     * Manages lazy loading and resolution of shader resources, allowing for
     * complex dependency chains, batched resolution, and different resolution strategies.
     */
    class DeferredResourceResolver
    {
    public:
        DeferredResourceResolver() = default;
        ~DeferredResourceResolver() = default;

        // No copy semantics
        DeferredResourceResolver(const DeferredResourceResolver&) = delete;
        DeferredResourceResolver& operator=(const DeferredResourceResolver&) = delete;

        // Move semantics
        DeferredResourceResolver(DeferredResourceResolver&&) = default;
        DeferredResourceResolver& operator=(DeferredResourceResolver&&) = default;

        /**
         * @brief Initialize the resolver system
         */
        void Initialize();

        /**
         * @brief Shutdown and clear all pending requests
         */
        void Shutdown();

        /**
         * @brief Request a resource to be resolved later
         * @param name Resource name
         * @param expectedType Expected resource type
         * @param resolver Function to resolve the resource
         * @param strategy Resolution strategy
         * @param requesterName Name of the system requesting this resource
         * @return true if request was queued successfully
         */
        bool RequestResource(const std::string& name, ShaderResourceType expectedType,
                           ResourceResolverFunction resolver,
                           ResolutionStrategy strategy = ResolutionStrategy::FrameEnd,
                           const std::string& requesterName = "");

        /**
         * @brief Request a resource with dependencies
         * @param name Resource name
         * @param expectedType Expected resource type
         * @param dependencies Resources this request depends on
         * @param resolver Function to resolve the resource
         * @param strategy Resolution strategy
         * @param requesterName Name of the system requesting this resource
         * @return true if request was queued successfully
         */
        bool RequestResourceWithDependencies(const std::string& name, ShaderResourceType expectedType,
                                            const std::unordered_set<std::string>& dependencies,
                                            ResourceResolverFunction resolver,
                                            ResolutionStrategy strategy = ResolutionStrategy::FrameEnd,
                                            const std::string& requesterName = "");

        /**
         * @brief Cancel a pending resource request
         * @param name Resource name to cancel
         * @return true if request was found and cancelled
         */
        bool CancelRequest(const std::string& name);

        /**
         * @brief Check if a resource request is pending
         * @param name Resource name
         * @return true if request is pending
         */
        bool IsRequestPending(const std::string& name) const;

        /**
         * @brief Get the status of a resource request
         * @param name Resource name
         * @return Resolution status
         */
        ResolutionStatus GetRequestStatus(const std::string& name) const;

        /**
         * @brief Get resolved resource if available
         * @param name Resource name
         * @return Pointer to resolved resource input, nullptr if not resolved
         */
        const ShaderResourceInput* GetResolvedResource(const std::string& name) const;

        /**
         * @brief Create a resource batch for bulk resolution
         * @param batchName Name for the batch
         * @param strategy Resolution strategy for the entire batch
         * @return true if batch was created successfully
         */
        bool CreateBatch(const std::string& batchName, ResolutionStrategy strategy = ResolutionStrategy::FrameEnd);

        /**
         * @brief Add a request to an existing batch
         * @param batchName Name of the batch
         * @param requestName Name of the request to add
         * @return true if request was added to batch
         */
        bool AddRequestToBatch(const std::string& batchName, const std::string& requestName);

        /**
         * @brief Execute a specific batch
         * @param batchName Name of the batch to execute
         * @return true if batch execution started successfully
         */
        bool ExecuteBatch(const std::string& batchName);

        /**
         * @brief Resolve all immediate strategy requests
         * @return Number of requests resolved
         */
        u32 ResolveImmediateRequests();

        /**
         * @brief Resolve all frame-end strategy requests
         * Called at the end of each frame
         * @return Number of requests resolved
         */
        u32 ResolveFrameEndRequests();

        /**
         * @brief Resolve all next-frame strategy requests
         * Called at the beginning of each frame
         * @return Number of requests resolved
         */
        u32 ResolveNextFrameRequests();

        /**
         * @brief Resolve lazy requests that are actually needed
         * @param requestedNames List of resource names that are needed now
         * @return Number of requests resolved
         */
        u32 ResolveLazyRequests(const std::unordered_set<std::string>& requestedNames);

        /**
         * @brief Process background resolution (call from background thread)
         * @return Number of requests resolved
         */
        u32 ProcessBackgroundResolution();

        /**
         * @brief Set the target resource hierarchy for resolved resources
         * @param hierarchy Resource hierarchy to populate
         */
        void SetTargetHierarchy(ResourceHierarchy* hierarchy) { m_TargetHierarchy = hierarchy; }

        /**
         * @brief Clear all resolved resources (keep pending requests)
         */
        void ClearResolvedResources();

        /**
         * @brief Clear all requests and resolved resources
         */
        void ClearAllRequests();

        /**
         * @brief Advance to the next frame
         */
        void NextFrame();

        /**
         * @brief Get statistics about the resolver
         */
        struct Statistics
        {
            u32 PendingRequests = 0;
            u32 ResolvingRequests = 0;
            u32 ResolvedRequests = 0;
            u32 FailedRequests = 0;
            u32 DeferredRequests = 0;
            u32 TotalBatches = 0;
            u32 ExecutingBatches = 0;
            u32 TotalDependencies = 0;
            f32 AverageResolutionTimeMs = 0.0f;
            u32 CurrentFrame = 0;
        };

        Statistics GetStatistics() const;

        /**
         * @brief Get string representation of resolution status
         * @param status Status to convert
         * @return String representation
         */
        static const char* GetStatusString(ResolutionStatus status);

        /**
         * @brief Get string representation of resolution strategy
         * @param strategy Strategy to convert
         * @return String representation
         */
        static const char* GetStrategyString(ResolutionStrategy strategy);

        /**
         * @brief Render ImGui debug interface
         */
        void RenderDebugInterface();

    private:
        // Pending resource requests
        std::unordered_map<std::string, DeferredResourceRequest> m_PendingRequests;
        
        // Resolved resources
        std::unordered_map<std::string, ShaderResourceInput> m_ResolvedResources;
        
        // Resource batches
        std::unordered_map<std::string, ResourceBatch> m_Batches;
        
        // Target hierarchy for resolved resources
        ResourceHierarchy* m_TargetHierarchy = nullptr;
        
        // Frame tracking
        u32 m_CurrentFrame = 0;
        
        // Initialization state
        bool m_Initialized = false;
        
        // Background processing
        std::mutex m_BackgroundMutex;
        std::queue<std::string> m_BackgroundQueue;

        /**
         * @brief Resolve a specific request
         * @param requestName Name of the request to resolve
         * @return true if resolution was successful
         */
        bool ResolveRequest(const std::string& requestName);

        /**
         * @brief Check if all dependencies of a request are resolved
         * @param request The request to check
         * @return true if all dependencies are satisfied
         */
        bool AreDependenciesResolved(const DeferredResourceRequest& request) const;

        /**
         * @brief Get all requests that can be resolved (dependencies satisfied)
         * @param strategy Filter by resolution strategy
         * @return Vector of request names ready for resolution
         */
        std::vector<std::string> GetResolvableRequests(ResolutionStrategy strategy) const;

        /**
         * @brief Update dependent requests when a request is resolved
         * @param resolvedRequestName Name of the resolved request
         */
        void UpdateDependents(const std::string& resolvedRequestName);

        /**
         * @brief Sort requests by dependency order
         * @param requestNames Vector of request names to sort
         * @return true if sorting was successful (no circular dependencies)
         */
        bool SortByDependencies(std::vector<std::string>& requestNames) const;

        /**
         * @brief Add resolved resource to target hierarchy if available
         * @param name Resource name
         * @param resource Resolved resource
         */
        void AddToTargetHierarchy(const std::string& name, const ShaderResourceInput& resource);

        /**
         * @brief Calculate average resolution time
         * @return Average resolution time in milliseconds
         */
        f32 CalculateAverageResolutionTime() const;
    };
}
