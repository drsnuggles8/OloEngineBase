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
            RenderPass::PassWorkType WorkType = RenderPass::PassWorkType::Graphics; // Phase G
            bool AsyncComputeCandidate = false;                                     // Phase G
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

        // Queue a texture extraction that explicitly writes back into a named
        // imported history resource for the next frame. The callback receives
        // the resolved source texture ID after Execute() completes.
        struct TemporalHistoryContract
        {
            std::string HistoryResource; ///< canonical imported-history name (previous-frame input)
            std::string SourceResource;  ///< current-frame resource extracted for next-frame reuse
            bool HistoryImported = false;
            bool SourceReachable = false;
        };
        void ExtractHistoryTexture(std::string_view historyResource,
                                   RGTextureHandle sourceHandle,
                                   std::function<void(u32)> callback);
        [[nodiscard]] const std::vector<TemporalHistoryContract>& GetTemporalHistoryContracts() const
        {
            return m_TemporalHistoryContracts;
        }

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
            RGSubresourceRange Range; ///< subresource range from the consuming access declaration
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
            InvalidHistoryContract,
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

        // -------------------------------------------------------------------
        // Phase G Slice 6 — Batch-event hook
        // -------------------------------------------------------------------
        // Fired when Execute() enters (isBegin=true) or exits (isBegin=false)
        // an async-compute batch boundary.  Useful for tests and profiling
        // tooling that need to verify batch boundary placement without a real
        // GL context.  Pass an empty function (or assign {}) to disable.
        using BatchEventCallback = std::function<void(u32 batchIndex, bool isBegin)>;
        void SetBatchEventHook(BatchEventCallback cb)
        {
            m_BatchEventHook = std::move(cb);
        }
        [[nodiscard]] bool HasBatchEventHook() const
        {
            return static_cast<bool>(m_BatchEventHook);
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

        // Phase G Slice 2 — Compute-pass scheduling hoist.
        // After UpdateDependencyGraph() builds a valid topological order, this
        // method applies a modified Kahn's pass over m_PassOrder: when multiple
        // passes are ready (all predecessors already scheduled), all
        // AsyncComputeCandidate passes are drained before any graphics pass is
        // advanced. The result is a still-valid topological order that gives
        // compute work a head start — simulating what a multi-queue backend
        // would achieve once async-compute scheduling lands in Phase G.2.
        // No-ops when no AsyncComputeCandidate pass is present.
        void HoistComputePasses();

      public:
        // -------------------------------------------------------------------
        // Phase G Slice 4 — Async-compute batch query
        // -------------------------------------------------------------------
        // A batch groups consecutive AsyncComputeCandidate passes from the
        // hoisted execution order together with the fence metadata needed for
        // queue synchronisation:
        //   WaitPasses  — non-batch passes this batch must wait for before it
        //                 can start (predecessor graphics work).
        //   SignalPasses — non-batch passes that must wait for this batch to
        //                 finish before they can start (successor graphics work).
        //
        // Backend mapping:
        //   GL 4.6   : glFenceSync after batch; glClientWaitSync before SignalPasses.
        //   Vulkan   : compute VkSubmitInfo with signal semaphores; graphics
        //              VkSubmitInfo with wait semaphores pointing at the batch.
        //   DX12     : D3D12_COMMAND_LIST_TYPE_COMPUTE + fence Signal/Wait pairs.

        // -------------------------------------------------------------------
        // Phase G Slice 8 — Cross-batch resource dependency surfacing
        // -------------------------------------------------------------------
        // A resource dependency that crosses an async-compute batch boundary.
        //   ExternalPass — the non-batch pass on the other side of the fence:
        //     InputResource  → the non-batch pass that last WROTE this resource
        //                      before the batch starts (the producer to wait for).
        //     OutputResource → the non-batch pass that first READS this resource
        //                      after the batch ends (the consumer that must wait).
        //
        // Backend usage:
        //   Vulkan : each InputResource maps to a wait-semaphore + image-layout
        //            transition on the compute queue.  Each OutputResource maps to
        //            a signal-semaphore + ownership-transfer release/acquire pair.
        //   DX12   : InputResources drive fence Wait() calls; OutputResources
        //            drive fence Signal() calls + resource-barrier transitions.
        struct BatchResourceDependency
        {
            std::string ResourceName; ///< virtual resource name registered in the graph
            std::string ExternalPass; ///< non-batch pass that produces (input) or first consumes (output) this resource
        };

        enum class QueueLane : u8
        {
            Graphics,
            Compute,
            Copy,
        };

        struct AsyncComputeBatch
        {
            std::vector<std::string> ComputePasses; ///< batch members, in execution order
            std::vector<std::string> WaitPasses;    ///< non-batch passes this batch waits for
            std::vector<std::string> SignalPasses;  ///< non-batch passes that wait for this batch
            QueueLane Lane = QueueLane::Compute;    ///< execution lane assignment for this batch (Phase G Slice 13)
            // Phase G Slice 8: per-resource cross-boundary dependency info
            std::vector<BatchResourceDependency> InputResources;  ///< resources entering the batch from outside
            std::vector<BatchResourceDependency> OutputResources; ///< resources leaving the batch to outside
        };

        // Partition the hoisted execution order into async-compute batches.
        // Returns an empty vector when no AsyncComputeCandidate pass exists.
        // Must be called AFTER Execute() (or after a forced topology update)
        // so that HoistComputePasses() has already run.
        [[nodiscard]] std::vector<AsyncComputeBatch> GetAsyncComputeBatches() const;

        // -------------------------------------------------------------------
        // Phase G Slice 5 — Submission-plan IR
        // -------------------------------------------------------------------
        // A backend-portable linearised sequence of operations that a
        // Vulkan/DX12/GL renderer can execute without re-reading the graph
        // topology or barrier tables.
        //
        // Command kinds:
        //   Pass          — submit / execute the named pass on the queue
        //                   indicated by PassWorkType.
        //   MemoryBarrier — insert a pipeline/memory barrier with the given
        //                   flags before the next pass begins.
        //   BatchBegin    — start a new async-compute submission slot;
        //                   backends insert a queue-wait here.
        //   BatchEnd      — close the current async-compute slot;
        //                   backends insert a queue-signal / fence here.
        //
        // Backend mapping:
        //   GL 4.6   : BatchEnd → glFenceSync; BatchBegin → glClientWaitSync.
        //              MemoryBarrier → glMemoryBarrier(flags).
        //   Vulkan   : BatchBegin/BatchEnd bound a VkSubmitInfo on the compute
        //              queue with matching wait/signal semaphores.
        //   DX12     : BatchBegin/BatchEnd bound a compute command-list with
        //              fence Signal/Wait pairs.
        struct SubmissionCommand
        {
            enum class Kind : u8
            {
                Pass,
                MemoryBarrier,
                BatchBegin,
                BatchEnd,
            };

            Kind CommandKind = Kind::Pass;
            std::string PassName;                                                   ///< non-empty for Pass commands
            MemoryBarrierFlags Barriers = MemoryBarrierFlags::None;                 ///< for MemoryBarrier commands
            u32 BatchIndex = 0;                                                     ///< for BatchBegin/BatchEnd: which async batch
            RenderPass::PassWorkType WorkType = RenderPass::PassWorkType::Graphics; ///< for Pass commands
            QueueLane Lane = QueueLane::Graphics;                                   ///< queue lane assignment for this command (Phase G Slice 13)

            // Phase G Slice 9: self-contained batch-boundary metadata so
            // backends can map waits/signals/resource ownership transitions
            // directly from GetSubmissionPlan() without side-channel queries.
            std::vector<std::string> WaitPasses;                  ///< for BatchBegin commands
            std::vector<std::string> SignalPasses;                ///< for BatchEnd commands
            std::vector<BatchResourceDependency> InputResources;  ///< for BatchBegin commands
            std::vector<BatchResourceDependency> OutputResources; ///< for BatchEnd commands
        };

        // Build the submission-plan IR for the current frame.
        // Integrates barrier plan (Phase E) with async-compute batch boundaries
        // (Phase G Slice 4) into a single linearised command stream.
        // Must be called AFTER Execute() so that barrier planning and
        // compute-hoist have already run.
        [[nodiscard]] std::vector<SubmissionCommand> GetSubmissionPlan() const;

        // -------------------------------------------------------------------
        // Phase G Slice 10 — Explicit resource transition records
        // -------------------------------------------------------------------
        // For each planned barrier, captures the before-state (producer's
        // write usage) and after-state (consumer's read usage) so
        // explicit-barrier backends can insert the correct image-layout
        // transitions and pipeline-stage masks without re-querying the
        // per-pass access-declaration tables.
        //
        // Each record corresponds one-to-one with an entry in
        // GetPlannedBarriers(): same resource/consumerPass pairing, with the
        // producer's write-usage and the consumer's read-usage added.
        //
        // Backend mapping:
        //   Vulkan  : FromUsage → VkImageLayout (e.g. RenderTarget →
        //             COLOR_ATTACHMENT_OPTIMAL); ToUsage → SHADER_READ_ONLY.
        //             Flags → VkAccessFlags / VkPipelineStageFlags.
        //   DX12    : FromUsage → D3D12_RESOURCE_STATE_RENDER_TARGET;
        //             ToUsage → D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE.
        //   GL 4.6  : glMemoryBarrier(Flags) already inserted; transition
        //             records are informational until explicit barriers land.
        struct ResourceTransition
        {
            std::string ResourceName;                            ///< virtual resource in the graph
            std::string ProducerPass;                            ///< last writer before this transition; "external" when only imported
            std::string ConsumerPass;                            ///< pass that reads the resource (barrier inserted before it)
            RGWriteUsage FromUsage = RGWriteUsage::RenderTarget; ///< write usage of ProducerPass
            RGReadUsage ToUsage = RGReadUsage::ShaderSample;     ///< read usage of ConsumerPass
            MemoryBarrierFlags Flags = MemoryBarrierFlags::None; ///< barrier flags from PlannedBarrier
            RGSubresourceRange Range;                            ///< subresource range from the consuming access declaration

            // Phase G Slice 14 — Cross-lane sync metadata (release/acquire intent).
            // Set when ProducerPass and ConsumerPass reside on different queue lanes.
            // On GL 4.6 this is informational only; on Vulkan/DX12 it drives
            // ownership-transfer barriers and semaphore waits.
            bool IsCrossLane = false;                     ///< true when producer and consumer are on different queue lanes
            QueueLane ProducerLane = QueueLane::Graphics; ///< lane of the producing pass; Graphics for "external" producers
            QueueLane ConsumerLane = QueueLane::Graphics; ///< lane of the consuming pass
        };

        // Derive all resource transition records from the current barrier plan
        // and access declarations. Returns an empty vector when no barriers are
        // planned (no declared reads/writes or passes not yet executed).
        // Must be called AFTER Execute() or after BuildFrameGraph() +
        // ComputeBarrierPlan() have run so that m_PlannedBarriers and
        // m_PassAccessDeclarations are populated.
        [[nodiscard]] std::vector<ResourceTransition> GetResourceTransitions() const;

        // -------------------------------------------------------------------
        // Phase G Slice 11 — Unified resource lifetime records
        // -------------------------------------------------------------------
        // One record per registered resource giving its full first-write /
        // last-read extent in pass-execution order.  Covers ALL resource kinds
        // (transient, imported, history, extracted) so that an explicit-barrier
        // backend (Vulkan / DX12) can schedule image-layout transitions and
        // memory acquire/release without additional bookkeeping.
        //
        //   IsImported  — resource entered via ImportTexture / ImportFramebuffer /
        //                 ImportBuffer; initial layout is driver-defined.
        //   IsExtracted — ExtractTexture / ExtractFramebuffer was called; the
        //                 backend must keep the resource live after the last pass.
        //   IsHistory   — resource is a temporal-history input (imported from
        //                 the previous frame via ImportHistory / ExtractHistoryTexture).
        //   IsTransient — resource is allocated by the transient pool; memory
        //                 can be freed at LastPassIndex+1.
        //
        //   FirstWritePassIndex / LastReadPassIndex are indices into GetPassOrder().
        //   When no write is found (import-only) FirstWritePassIndex == UINT32_MAX
        //   and FirstWritePass == "external".  When no read is found (write-only /
        //   extracted) LastReadPassIndex == UINT32_MAX and LastReadPass == "".
        struct ResourceLifetime
        {
            std::string ResourceName;
            bool IsImported = false;                                   ///< entered via ImportTexture/ImportFramebuffer/ImportBuffer
            bool IsExtracted = false;                                  ///< has a pending TextureExtract or FramebufferExtract
            bool IsHistory = false;                                    ///< temporal-history resource (ImportHistory)
            bool IsTransient = false;                                  ///< allocated by the transient pool
            u32 FirstWritePassIndex = std::numeric_limits<u32>::max(); ///< index in GetPassOrder(); UINT32_MAX when import-only
            u32 LastReadPassIndex = std::numeric_limits<u32>::max();   ///< index in GetPassOrder(); UINT32_MAX when write-only
            std::string FirstWritePass;                                ///< name of the first writing pass; "external" when import-only
            std::string LastReadPass;                                  ///< name of the last reading pass; "" when no reads declared
            RGWriteUsage FirstWriteUsage = RGWriteUsage::RenderTarget; ///< usage at first write
            RGReadUsage LastReadUsage = RGReadUsage::ShaderSample;     ///< usage at last read
        };

        // Returns one ResourceLifetime per registered resource, ordered to
        // match GetRegisteredResources().  Available after Execute() or after
        // UpdateDependencyGraph() + ComputeBarrierPlan() have run.
        [[nodiscard]] std::vector<ResourceLifetime> GetResourceLifetimes() const;

      private:
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
        [[nodiscard]] bool IsHistoryTextureResource(std::string_view resourceName) const;
        [[nodiscard]] bool IsResourceReachableForExtraction(std::string_view resourceName) const;

        // Get the list of passes that were culled in the last reachability
        // analysis. Useful for debugging and profiling.
        [[nodiscard]] const std::vector<std::string>& GetCulledPasses() const
        {
            return m_CulledPasses;
        }

        std::unordered_map<std::string, Ref<RenderPass>> m_PassLookup;
        std::unordered_map<std::string, std::vector<std::string>> m_Dependencies; // Execution ordering

        std::vector<std::string> m_InsertionOrder; // Pass names in AddPass() order (stable topo tie-break)
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
        BatchEventCallback m_BatchEventHook;

        // Execution-ready cache — rebuilt when m_DependencyGraphDirty is set.
        // Avoids per-frame hash lookups in Execute().
        std::vector<RenderPass*> m_CachedExecutionOrder;
        std::vector<SubmissionCommand> m_CachedSubmissionPlan; ///< Phase G Slice 6: IR cached after barrier planning

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
        struct HistoryTextureExtract
        {
            std::string HistoryResource;
            RGTextureHandle SourceHandle;
            std::function<void(u32)> Callback;
        };
        struct FramebufferExtract
        {
            RGFramebufferHandle Handle;
            std::function<void(Ref<Framebuffer>)> Callback;
        };
        std::vector<TextureExtract> m_TextureExtracts;
        std::vector<HistoryTextureExtract> m_HistoryTextureExtracts;
        std::vector<FramebufferExtract> m_FramebufferExtracts;
        std::vector<TemporalHistoryContract> m_TemporalHistoryContracts;

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
