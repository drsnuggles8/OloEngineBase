#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/TransientPool.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/RGPassSetup.h"
#include <functional>
#include <limits>
#include <vector>
#include <unordered_map>
#include <string>
#include <string_view>
#include <unordered_set>

namespace OloEngine
{
    // @brief Manages a graph of render passes forming a complete rendering pipeline.
    class RenderGraph : public RefCounted
    {
      public:
        RenderGraph() = default;
        ~RenderGraph() = default;

        void Init(u32 width, u32 height);
        void Shutdown();

        // Clear all topology bookkeeping (passes, edges, framebuffer piping,
        // cached execution order) WITHOUT touching the passes themselves.
        // Used by `Renderer3D::ConfigureRenderGraph(RenderingPath)` to rebuild
        // the graph when the user switches between Forward / Forward+ /
        // Deferred at runtime. Because passes are owned externally as
        // `Ref<>`s on `Renderer3D::s_Data`, their framebuffers and internal
        // state survive the reset. Callers must re-`AddPass` every pass they
        // want in the new topology and re-issue all edges before calling
        // `SetFinalPass` / `ValidateResourceHazards` again.
        void ResetTopology();

        // Only support RenderPass
        void AddPass(const Ref<RenderPass>& pass);
        // Connect two passes: establishes execution ordering AND framebuffer piping
        void ConnectPass(const std::string& outputPass, const std::string& inputPass);

        // Add an execution-ordering dependency without framebuffer piping.
        // Use this when the upstream pass produces outputs consumed via texture bindings
        // rather than framebuffer attachments (e.g., shadow maps).
        void AddExecutionDependency(const std::string& beforePass, const std::string& afterPass);

        // Execute all passes in the correct order
        void Execute();

        // Resize all passes in the graph
        void Resize(u32 width, u32 height);

        // Set the final pass in the graph
        void SetFinalPass(const std::string& passName);

        // @brief Get all render passes in the graph for debugging or inspection.
        // @return Vector of render passes in the execution order
        [[nodiscard]] std::vector<Ref<RenderPass>> GetAllPasses() const;

        // Get a pass by name and cast to the requested type
        template<typename T>
        Ref<T> GetPass(const std::string& name)
        {
            if (m_PassLookup.find(name) != m_PassLookup.end())
            {
                return m_PassLookup.at(name).As<T>();
            }
            return nullptr;
        }

        [[nodiscard]] bool IsFinalPass(const std::string& passName) const;

        struct ConnectionInfo
        {
            std::string OutputPass;
            std::string InputPass;
            u32 AttachmentIndex = 0;
        };

        [[nodiscard]] std::vector<ConnectionInfo> GetConnections() const;

        struct PassSubmissionInfo
        {
            std::string PassName;
            RenderPass::SubmissionModel Submission = RenderPass::SubmissionModel::Unknown;
            bool DeclaresResources = false;
        };
        [[nodiscard]] std::vector<PassSubmissionInfo> GetPassSubmissionInfo() const;

        // @brief Get the topologically-sorted pass execution order (for testing/inspection).
        [[nodiscard]] const std::vector<std::string>& GetPassOrder() const
        {
            return m_PassOrder;
        }

        // -------------------------------------------------------------------
        // Resource-aware hazard validation
        // -------------------------------------------------------------------
        // Walks the topologically-sorted execution order and, using each
        // pass's declared reads/writes (see RenderPass::DeclareRead/Write),
        // validates that every read has a transitive execution dependency
        // on its producer, that no two parallel passes write the same
        // resource, and that writers don't overwrite live reads. Catches
        // missing `ConnectPass` / `AddExecutionDependency` calls.
        //
        // Call this AFTER all passes are added and connections made — the
        // first call forces topological sort if needed. Returns the list of
        // hazards (empty vector = clean). Also logs each hazard as an error
        // through the engine logger.
        enum class HazardKind
        {
            ReadAfterWrite,                 // reader does not depend on writer
            WriteAfterWrite,                // later writer does not depend on previous writer
            WriteAfterRead,                 // later writer does not depend on prior reader
            ResourceKindMismatch,           // same logical resource declared with conflicting kinds
            FeedbackWithoutDeclaration,     // same pass reads+writes overlapping subresource
            ImportedResourceLifetimeMisuse, // imported resource is used without valid backing
            Cycle,                          // dependency graph has a cycle; hazards could not be validated
        };
        struct Hazard
        {
            HazardKind Kind;
            std::string Resource;
            std::string Producer; // writer (RAW / WAW) or reader (WAR)
            std::string Consumer; // reader (RAW), later writer (WAW / WAR)
            std::string Message;
        };
        [[nodiscard]] std::vector<Hazard> ValidateResourceHazards();

        struct ResourceInfo
        {
            std::string Name;
            RGResourceDesc Desc;
            RGTextureHandle TextureHandle;
            RGBufferHandle BufferHandle;
            RGFramebufferHandle FramebufferHandle;
            std::vector<std::string> Producers;
            std::vector<std::string> Consumers;
        };

        void ImportResource(std::string_view name, const RGResourceDesc& desc);
        void ClearImportedResources();
        [[nodiscard]] const std::vector<ResourceInfo>& GetRegisteredResources() const;
        [[nodiscard]] const ResourceInfo* FindRegisteredResource(std::string_view name) const;
        [[nodiscard]] RGTextureHandle GetTextureHandle(std::string_view name) const;
        [[nodiscard]] RGBufferHandle GetBufferHandle(std::string_view name) const;
        [[nodiscard]] RGFramebufferHandle GetFramebufferHandle(std::string_view name) const;
        [[nodiscard]] bool IsTextureHandleCurrent(RGTextureHandle handle) const;
        [[nodiscard]] bool IsBufferHandleCurrent(RGBufferHandle handle) const;
        [[nodiscard]] bool IsFramebufferHandleCurrent(RGFramebufferHandle handle) const;

        // -------------------------------------------------------------------
        // Phase B — Typed import / resolve / extract API
        // -------------------------------------------------------------------
        // Import a physical GL texture into the graph under a canonical name.
        // Returns a typed handle that can be stored in the FrameBlackboard or
        // passed to pass DeclareRead/DeclareWrite via the string overload.
        // `textureID == 0` is accepted for optional resources (e.g. velocity
        // when no velocity buffer exists) — the handle is still valid but
        // resolve will return 0.
        [[nodiscard]] RGTextureHandle ImportTexture(std::string_view name, u32 textureID,
                                                    const RGResourceDesc& desc = {});

        // Import a physical Framebuffer. `fb` may be null for optional targets.
        [[nodiscard]] RGFramebufferHandle ImportFramebuffer(std::string_view name,
                                                            const Ref<Framebuffer>& fb,
                                                            const RGResourceDesc& desc = {});

        // Import a UBO or SSBO by its GL name/binding.
        [[nodiscard]] RGBufferHandle ImportBuffer(std::string_view name, u32 bufferID,
                                                  const RGResourceDesc& desc = {});

        // Import a temporal history texture (same as ImportTexture but records
        // the IsHistory flag so the debug dump can distinguish live from history
        // resources). Returns an invalid handle when `textureID == 0`.
        [[nodiscard]] RGTextureHandle ImportHistory(std::string_view name, u32 textureID,
                                                    const RGResourceDesc& desc = {});

        // Resolve a handle back to its physical texture ID.
        // Returns 0 for an invalid handle or a handle whose backing resource
        // was imported with textureID == 0.
        [[nodiscard]] u32 ResolveTexture(RGTextureHandle handle) const;

        // Resolve a handle back to its physical Framebuffer.
        // Returns nullptr for an invalid handle or a null-imported framebuffer.
        [[nodiscard]] Ref<Framebuffer> ResolveFramebuffer(RGFramebufferHandle handle) const;

        // Resolve a handle back to its physical buffer ID.
        // Returns 0 for an invalid handle.
        [[nodiscard]] u32 ResolveBuffer(RGBufferHandle handle) const;

        // Resolve a typed handle back to its canonical graph resource name.
        // Returns empty when the handle is invalid or stale.
        [[nodiscard]] std::string_view GetResourceName(RGTextureHandle handle) const;
        [[nodiscard]] std::string_view GetResourceName(RGFramebufferHandle handle) const;
        [[nodiscard]] std::string_view GetResourceName(RGBufferHandle handle) const;

        // Queue an extraction callback that fires after Execute() completes.
        // The callback receives the resolved texture ID. Useful for persisting
        // a resource across frames (e.g. TAA history write-back).
        void ExtractTexture(RGTextureHandle handle, std::function<void(u32)> callback);

        // Queue a framebuffer extraction callback.
        void ExtractFramebuffer(RGFramebufferHandle handle, std::function<void(Ref<Framebuffer>)> callback);

        // Fire all pending extraction callbacks and clear the queue.
        // Called automatically by Execute() after all passes have run.
        void FlushExtractions();

        // -------------------------------------------------------------------
        // Frame blackboard
        // -------------------------------------------------------------------
        // Phase B — Frame blackboard
        // -------------------------------------------------------------------
        // The blackboard is populated by Renderer3D::SetupFrameBlackboard()
        // at the start of each frame.  Passes must not write to the
        // blackboard directly — they only read handles from it to feed their
        // DeclareRead/DeclareWrite calls, or to resolve physical resources
        // via ResolveTexture/ResolveFramebuffer inside Execute().
        [[nodiscard]] FrameBlackboard& GetBlackboard()
        {
            return m_Blackboard;
        }
        [[nodiscard]] const FrameBlackboard& GetBlackboard() const
        {
            return m_Blackboard;
        }
        void ClearBlackboard()
        {
            m_Blackboard.Reset();
        }

        // -------------------------------------------------------------------
        // Phase D — Transient resource pool access
        // -------------------------------------------------------------------
        // Get the transient pool for acquiring/releasing resources.
        // Call ReleaseAll() after frame execution to return acquired resources.
        [[nodiscard]] TransientPool& GetTransientPool()
        {
            return m_TransientPool;
        }

        // -------------------------------------------------------------------
        // Phase C — Builder support (transient resource allocation)
        // -------------------------------------------------------------------
        // Allocate a handle for a new transient (virtual) resource.
        // The physical resource is allocated and managed by the graph's
        // transient pool (Phase D). Until then, these are stubs.
        [[nodiscard]] RGTextureHandle AllocateTransientTextureHandle(std::string_view name, const RGResourceDesc& desc);
        [[nodiscard]] RGFramebufferHandle AllocateTransientFramebufferHandle(std::string_view name, const RGResourceDesc& desc);
        [[nodiscard]] RGBufferHandle AllocateTransientBufferHandle(std::string_view name, const RGResourceDesc& desc);

        struct TransientPlanEntry
        {
            std::string Resource;
            ResourceHandle::Kind Kind = ResourceHandle::Kind::Unknown;
            u32 FirstPassIndex = std::numeric_limits<u32>::max();
            u32 LastPassIndex = 0;
            std::string FirstPass;
            std::string LastPass;
            std::string AliasGroup;
            u32 AliasSlot = std::numeric_limits<u32>::max();
            u64 EstimatedBytes = 0;
            bool Reachable = false;
            bool WillAllocate = false;
            std::string SkipReason;
        };

        [[nodiscard]] const std::vector<TransientPlanEntry>& GetTransientPlan() const
        {
            return m_TransientPlan;
        }

        // -------------------------------------------------------------------
        // Phase C — Per-frame graph building
        // -------------------------------------------------------------------
        // Build the frame graph by running all registered pass setup callbacks.
        // Each setup callback declares its resource reads/writes to the builder.
        // After setup, the graph is topologically ordered and ready to execute.
        void BuildFrameGraph();

        struct FrameBuildStats
        {
            u32 PassesVisited = 0;
            u32 DeclaredReads = 0;
            u32 DeclaredWrites = 0;
            u32 DerivedEdges = 0;
        };

        [[nodiscard]] const FrameBuildStats& GetLastBuildStats() const
        {
            return m_LastBuildStats;
        }

        struct PlannedBarrier
        {
            std::string BeforePass;
            std::string Resource;
            MemoryBarrierFlags Flags = MemoryBarrierFlags::None;
        };

        struct PassTiming
        {
            std::string PassName;
            f64 CpuMs = 0.0;
        };

        [[nodiscard]] const std::vector<PlannedBarrier>& GetPlannedBarriers() const
        {
            return m_PlannedBarriers;
        }

        enum class BarrierDiagnosticKind
        {
            MissingProducer,
            CulledProducer,
            UnmappedTransition,
            StaleExtractionHandle,
            ExtractionOfCulledResource,
        };

        struct BarrierDiagnostic
        {
            BarrierDiagnosticKind Kind = BarrierDiagnosticKind::MissingProducer;
            std::string PassName;
            std::string Resource;
            std::string Message;
        };

        [[nodiscard]] const std::vector<BarrierDiagnostic>& GetBarrierDiagnostics() const
        {
            return m_BarrierDiagnostics;
        }

        [[nodiscard]] const std::vector<PassTiming>& GetLastPassTimings() const
        {
            return m_LastPassTimings;
        }

        // -------------------------------------------------------------------
        // Debug — Post-pass execution hook
        // -------------------------------------------------------------------
        // Fired after each pass->Execute() returns inside RenderGraph::Execute(),
        // BEFORE the post-pass extraction phase. Used by debug tooling
        // (e.g. RenderGraphFrameCapture) to snapshot intermediate state.
        // Pass the empty function (or assign {}) to disable.
        using PostPassHook = std::function<void(const std::string& passName, RenderGraph& graph)>;
        void SetPostPassHook(PostPassHook hook)
        {
            m_PostPassHook = std::move(hook);
        }
        [[nodiscard]] bool HasPostPassHook() const
        {
            return static_cast<bool>(m_PostPassHook);
        }

        void SetRuntimeBarrierExecutionEnabled(const bool enabled)
        {
            m_RuntimeBarrierExecutionEnabled = enabled;
        }

        void SetTransientMaterializationEnabled(const bool enabled)
        {
            m_RuntimeTransientMaterializationEnabled = enabled;
        }

        // Register a graph-native pass with setup and execute callbacks.
        // Setup captures per-frame parameters and declares resources.
        // Execute receives resolved resources and runs GPU work.
        void RegisterGraphPass(
            std::string_view name,
            PassSetupFn setup,
            PassExecuteFn execute);

        void ClearGraphPasses();

        // Explicitly set the final output pass (swap-chain writer).
        // Phase C graph passes use this to mark the backbuffer output.
        void SetGraphFinalPass(std::string_view passName)
        {
            // Phase C stub: just track the final pass name
            m_FinalPassName = passName;
            m_HasExplicitFinalPass = true;
        }

        // @brief Dump the current graph as a Graphviz DOT file.
        //
        // Emits a directed graph where nodes are passes (insertion order,
        // with the final pass double-ringed) and edges are coloured by
        // kind — solid black for framebuffer piping (ConnectPass), dashed
        // grey for ordering-only edges (AddExecutionDependency). Useful
        // for one-off visualisation and for debugging RenderGraph topology
        // changes. Render with `dot -Tsvg graph.dot -o graph.svg`.
        //
        // Returns true on success, false if the file could not be written.
        // The call is read-only — it does not force a topo sort.
        [[nodiscard]] bool DumpToDot(const std::string& filePath) const;

        // @brief Dump compiled graph data as JSON (passes/resources/culling/barriers).
        // Useful for automated tooling and richer offline inspection than DOT.
        [[nodiscard]] bool DumpToJson(const std::string& filePath) const;

      private:
        // Returns false if the graph contains a cycle (m_PassOrder will be
        // partial/empty in that case). Callers must abort further work when
        // false is returned — running validators or Execute() against a
        // partial topo order produces misleading diagnostics.
        [[nodiscard]] bool UpdateDependencyGraph();
        void ResolveFinalPass();

        // -------------------------------------------------------------------
        // Phase E — Backward reachability analysis and pass culling
        // -------------------------------------------------------------------
        // Compute backward reachability from the final pass.
        // Marks all passes that produce resources consumed (directly or
        // indirectly) by the final pass. Passes not marked as reachable
        // will be culled unless they have side effects.
        void ComputeReachability();
        void ComputeBarrierPlan();
        [[nodiscard]] static MemoryBarrierFlags ResolveProducerBarrierFlags(RGWriteUsage usage);
        [[nodiscard]] static MemoryBarrierFlags ResolveConsumerBarrierFlags(RGReadUsage usage);

        // Check if a pass is marked as reachable (not culled) after the last
        // ComputeReachability() call. Reachability is recalculated in
        // BuildFrameGraph() and Execute().
        [[nodiscard]] bool IsPassReachable(const std::string& passName) const;

        // Get the list of passes that were culled in the last reachability
        // analysis. Useful for debugging and profiling.
        [[nodiscard]] const std::vector<std::string>& GetCulledPasses() const
        {
            return m_CulledPasses;
        }

        std::unordered_map<std::string, Ref<RenderPass>> m_PassLookup;
        std::unordered_map<std::string, std::vector<std::string>> m_Dependencies;           // Execution ordering
        std::unordered_map<std::string, std::vector<std::string>> m_FramebufferConnections; // Framebuffer piping
        std::vector<std::string> m_InsertionOrder;                                          // Pass names in AddPass() order (stable topo tie-break)
        std::vector<std::string> m_PassOrder;
        std::string m_FinalPassName;
        bool m_HasExplicitFinalPass = false;
        bool m_DependencyGraphDirty = false;

        // Phase E — Reachability tracking
        std::unordered_set<std::string> m_ReachablePasses; // Passes that are reachable from final output
        std::vector<std::string> m_CulledPasses;           // Passes that were culled in last analysis

        // Phase E — Barrier planning/execution
        std::unordered_map<std::string, std::vector<RGAccessDeclaration>> m_PassAccessDeclarations;
        std::unordered_map<std::string, MemoryBarrierFlags> m_PassBarrierFlags;
        std::vector<PlannedBarrier> m_PlannedBarriers;
        std::vector<BarrierDiagnostic> m_BarrierDiagnostics;
        std::vector<PassTiming> m_LastPassTimings;
        bool m_RuntimeBarrierExecutionEnabled = true;
        bool m_RuntimeTransientMaterializationEnabled = false;

        PostPassHook m_PostPassHook;

        // Execution-ready cache — rebuilt when m_DependencyGraphDirty is set.
        // Avoids per-frame hash lookups in Execute().
        //
        // Lifetime: FramebufferPipe and its RenderPass* members are non-owning raw
        // pointers into m_PassLookup.  They must remain valid until
        // RebuildExecutionCache() is called.  Do not remove or destroy passes from
        // m_PassLookup while the cache is in use.
        struct FramebufferPipe
        {
            RenderPass* OutputPass = nullptr;
            std::vector<RenderPass*> InputPasses;
        };
        std::vector<FramebufferPipe> m_CachedPipes;
        std::vector<RenderPass*> m_CachedExecutionOrder;

        std::unordered_map<std::string, RGResourceDesc> m_ImportedResources;

        mutable bool m_ResourceRegistryDirty = true;
        mutable std::unordered_map<std::string, ResourceInfo> m_ResourceRegistry;
        mutable std::vector<ResourceInfo> m_RegisteredResources;
        mutable std::vector<Hazard> m_ResourceRegistryDiagnostics;
        mutable std::unordered_map<std::string, RGTextureHandle> m_TextureHandlesByName;
        mutable std::unordered_map<std::string, RGBufferHandle> m_BufferHandlesByName;
        mutable std::unordered_map<std::string, RGFramebufferHandle> m_FramebufferHandlesByName;

        struct HandleSlot
        {
            u32 Generation = 1;
            bool Alive = false;
            std::string Name;
        };
        mutable std::vector<HandleSlot> m_TextureHandleSlots;
        mutable std::vector<HandleSlot> m_BufferHandleSlots;
        mutable std::vector<HandleSlot> m_FramebufferHandleSlots;
        mutable std::vector<u32> m_FreeTextureHandleIndices;
        mutable std::vector<u32> m_FreeBufferHandleIndices;
        mutable std::vector<u32> m_FreeFramebufferHandleIndices;

        // -------------------------------------------------------------------
        // Phase B — Physical resource storage (parallel to handle slots)
        // -------------------------------------------------------------------
        struct PhysicalTexture
        {
            u32 TextureID = 0;
            bool IsHistory = false;
        };
        struct PhysicalFramebuffer
        {
            Ref<Framebuffer> FB;
        };
        struct PhysicalBuffer
        {
            u32 BufferID = 0;
        };

        // Parallel arrays — index by handle.Index (same slot system as HandleSlots).
        // Grown alongside m_TextureHandleSlots / m_FramebufferHandleSlots / m_BufferHandleSlots.
        mutable std::vector<PhysicalTexture> m_PhysicalTextures;
        mutable std::vector<PhysicalFramebuffer> m_PhysicalFramebuffers;
        mutable std::vector<PhysicalBuffer> m_PhysicalBuffers;

        // -------------------------------------------------------------------
        // Phase B — Extraction queue
        // -------------------------------------------------------------------
        struct TextureExtract
        {
            RGTextureHandle Handle;
            std::function<void(u32)> Callback;
        };
        struct FramebufferExtract
        {
            RGFramebufferHandle Handle;
            std::function<void(Ref<Framebuffer>)> Callback;
        };
        std::vector<TextureExtract> m_TextureExtracts;
        std::vector<FramebufferExtract> m_FramebufferExtracts;

        // -------------------------------------------------------------------
        // Phase B — Frame blackboard
        // -------------------------------------------------------------------
        FrameBlackboard m_Blackboard;
        // -------------------------------------------------------------------
        // Phase D — Transient resource pool
        // -------------------------------------------------------------------
        TransientPool m_TransientPool;

        // -------------------------------------------------------------------
        // Phase C — Graph-native pass registration and execution
        // -------------------------------------------------------------------
        struct GraphPass
        {
            std::string Name;
            PassSetupFn Setup;
            PassExecuteFn Execute;
        };
        std::vector<GraphPass> m_GraphPasses;
        FrameBuildStats m_LastBuildStats;

        // Allocate or recycle a texture handle slot and record the physical
        // resource. Called by ImportTexture / ImportHistory.
        RGTextureHandle AllocateTextureHandle(std::string_view name, u32 textureID, bool isHistory);
        RGFramebufferHandle AllocateFramebufferHandle(std::string_view name, const Ref<Framebuffer>& fb);
        RGBufferHandle AllocateBufferHandle(std::string_view name, u32 bufferID);

        void RebuildExecutionCache();
        void EnsureResourceRegistryBuilt() const;
        void RebuildTransientPlan();
        void MaterializeTransientResources();

        [[nodiscard]] static std::string BuildTransientAliasGroup(const RGResourceDesc& desc);
        [[nodiscard]] static u64 EstimateTransientBytes(const RGResourceDesc& desc);
        [[nodiscard]] static bool IsTransientDescriptorAllocatable(const RGResourceDesc& desc);
        [[nodiscard]] static std::string_view GetTransientDescriptorSkipReason(const RGResourceDesc& desc);
        [[nodiscard]] static ImageFormat ToImageFormat(RGResourceFormat format);
        [[nodiscard]] static FramebufferTextureFormat ToFramebufferFormat(RGResourceFormat format);

        std::unordered_map<std::string, RGResourceDesc> m_TransientResourceDescs;
        std::vector<TransientPlanEntry> m_TransientPlan;
    };
} // namespace OloEngine
