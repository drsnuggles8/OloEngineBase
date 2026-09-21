#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Memory/Platform.h"
#include "CommandPacket.h"
#include "CommandAllocator.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/LinkedList.h"
#include <unordered_map>
#include <atomic>
#include <functional>
#include <span>

#include "OloEngine/Threading/Mutex.h"
// TUniqueLock — used by the parallel-submission paths below. Not transitively
// provided by Mutex.h: a non-PCH TU including this header (the Vulkan
// execution test was the first) fails on Linux/Clang without it.
#include "OloEngine/Threading/UniqueLock.h"

#include <optional>

namespace OloEngine
{
    // Forward declarations
    class RendererAPI;
    class FrameDataBuffer;

    // Key for identifying groups of draw calls that can be instanced together.
    // Commands sharing the same key render the same geometry with the same
    // material and render state — only the per-instance transform differs.
    //
    // Geometry identity is the GL-level triplet the draw actually binds —
    // vertex array + index range — NOT the asset handle. Runtime-imported
    // meshes (Model's assimp path) are never registered with the AssetManager,
    // so they all carry the default handle 0; keying on the handle merged any
    // two such meshes whose material data deduplicated to the same index, and
    // the batched draw then rendered the FIRST command's geometry at every
    // instance's transform. In practice: two Kenney GLBs share a palette
    // material, so distant cars drew the ship's hull (near the quay the boats
    // are frustum-culled, no cross-model group forms, and everything looks
    // right — which is what made this look like a distance-dependent LOD bug).
    struct InstanceGroupKey
    {
        // Identity, not driver name (issue #691): batching two
        // draws together because their VAOs share a recycled GL name would
        // render one mesh with the other's geometry. Two LIVE handles cannot
        // collide, so this is a correctness improvement, not a retype.
        RHI::ResourceHandle vertexArrayID{};
        u32 indexCount = 0;
        u32 baseIndex = 0;
        u16 materialDataIndex = 0;
        u16 renderStateIndex = 0;

        // Bone-palette partition id (issue #1031). 0 for a static draw. A
        // skinned draw may only batch with another skinned draw whose bone
        // palette — current AND previous pose — is byte-identical, because the
        // batched draw uploads exactly one palette for all N instances.
        //
        // This is a partition ORDINAL assigned by BatchCommands after it has
        // compared the candidate palettes byte for byte, NOT a content hash.
        // A hash here would make a collision render one actor in another's
        // pose, which is the kind of wrong image nobody reads as a bug; an
        // ordinal cannot collide by construction. Palettes are only compared
        // among draws that already share the geometry/material key above, so
        // the comparison never runs for a scene with one character.
        u64 bonePaletteID = 0;

        bool operator==(const InstanceGroupKey& other) const = default;
    };

    struct InstanceGroupKeyHash
    {
        sizet operator()(const InstanceGroupKey& key) const
        {
            sizet h = std::hash<u64>{}(RHI::HashKey(key.vertexArrayID));
            h ^= std::hash<u32>{}(key.indexCount) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<u32>{}(key.baseIndex) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<u16>{}(key.materialDataIndex) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<u16>{}(key.renderStateIndex) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<u64>{}(key.bonePaletteID) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    // Maximum number of worker threads for parallel command generation
    // This should match the maximum expected worker thread count
    static constexpr u32 MAX_RENDER_WORKERS = 16;

    // Batch size for thread-local slot claiming (reduces atomic operations)
    // Following Molecular Matters Version 7: claim 32 entries at a time
    static constexpr u32 TLS_BATCH_SIZE = 32;

    // Per-bucket view state — enables multiple cameras/viewpoints per frame
    // (split-screen, minimaps, reflection probes) without full BeginScene/EndScene cycles.
    struct BucketViewState
    {
        glm::mat4 ViewMatrix = glm::mat4(1.0f);
        glm::mat4 ProjectionMatrix = glm::mat4(1.0f);
        glm::mat4 ViewProjectionMatrix = glm::mat4(1.0f);
        glm::vec3 ViewPosition = glm::vec3(0.0f);
    };

    // Callback type for reading/writing view state without depending on CommandDispatch.
    // ReadFn fills a BucketViewState from the current global state.
    // WriteFn applies a BucketViewState to the global state.
    using ViewStateReadFn = void (*)(BucketViewState& out);
    using ViewStateWriteFn = void (*)(const BucketViewState& in);

    // Configuration for command bucket processing
    struct CommandBucketConfig
    {
        bool EnableSorting = true;  // Sort commands to minimize state changes
        bool EnableBatching = true; // Batch similar DrawMesh → DrawMeshInstanced (requires instanced shader support)
        // The cap the DISPATCHER can actually honour. CommandDispatch::
        // DrawMeshInstanced truncates to this and does not split the batch, so
        // a bucket configured above it collapses source draws that then never
        // render. It used to be spelled `CommandBucketConfig{}.MaxMeshInstances`
        // at the two dispatch-side sites -- i.e. the DEFAULT config rather than
        // the bucket's own -- which is why a raised cap silently dropped
        // instances instead of failing. Naming it once is what lets
        // ValidateConfig below reject a value the dispatcher cannot serve.
        static constexpr u32 kMaxDispatchableMeshInstances = 16384;

        u32 MaxMeshInstances = kMaxDispatchableMeshInstances; // Maximum instances per DrawMeshInstanced packet, for CommandBucket's CPU
                                                              // auto-batching of separate DrawMesh entities that share mesh/material/render
                                                              // state — a different path from the GPU-cull InstancedMeshComponent draw (see
                                                              // FrameDataBuffer::DEFAULT_ENTITY_ID_CAPACITY, 262144, for that one; issue
                                                              // #524's draws_unique/draws_instanced/anim_crowd stress scenes all route
                                                              // around this path — unique materials, single-InstancedMeshComponent, and
                                                              // animated-mesh-skips-batching, respectively — so there's no evidence this
                                                              // needs to scale with those). Well below FrameDataBuffer's EntityID/Color/
                                                              // Custom capacity (so an N-into-1 collapse never truncates per-source picking
                                                              // IDs) and below DEFAULT_TRANSFORM_CAPACITY (65536) with headroom — a group
                                                              // at this cap uses only 1/4 of the transform buffer for its current-transform
                                                              // allocation, leaving room for the prev-transform stream plus other groups in
                                                              // the same frame. Dispatcher uses a TLS heap scratch so raising this doesn't
                                                              // bloat the stack.
        u32 InitialCapacity = 1024;                           // Initial capacity for command arrays
    };

    // Cache-line padded slot for thread-local storage
    // Prevents false sharing between worker threads
    struct alignas(OLO_PLATFORM_CACHE_LINE_SIZE) TLSBucketSlot
    {
        u32 offset = 0;     // Current write offset in thread-local range
        u32 remaining = 0;  // Remaining slots in current batch
        u32 batchStart = 0; // Start offset of current batch in global array
        // Padding to fill cache line
        u8 Pad[OLO_PLATFORM_CACHE_LINE_SIZE - 3 * sizeof(u32)];
    };
    static_assert(sizeof(TLSBucketSlot) == OLO_PLATFORM_CACHE_LINE_SIZE,
                  "TLSBucketSlot must be exactly one cache line");

    // A bucket of command packets that can be sorted and executed together
    // Thread-safe for parallel command generation following Molecular Matters pattern
    class CommandBucket
    {
      public:
        CommandBucket(const CommandBucketConfig& config = CommandBucketConfig());
        ~CommandBucket();

        // Disallow copying
        CommandBucket(const CommandBucket&) = delete;
        CommandBucket& operator=(const CommandBucket&) = delete;

        // Allow moving
        CommandBucket(CommandBucket&& other) noexcept;
        CommandBucket& operator=(CommandBucket&& other) noexcept;

        // Add a command packet to the bucket
        void AddCommand(CommandPacket* packet);

        template<typename T>
        CommandPacket* Submit(const T& commandData, const PacketMetadata& metadata = {}, CommandAllocator* allocator = nullptr)
        {
            TUniqueLock<FMutex> lock(m_Mutex);

            CommandAllocator* alloc = allocator ? allocator : m_Allocator;
            OLO_CORE_ASSERT(alloc, "CommandBucket::Submit: No allocator available (neither passed nor set on bucket)!");
            CommandPacket* packet = alloc->CreateCommandPacket(commandData, metadata);
            if (packet)
            {
                m_Keys.Add(metadata.m_SortKey.GetKey());
                m_Packets.Add(packet);
                ++m_CommandCount;
                ++m_Stats.TotalCommands;

                // Adding a new command invalidates sorting and batching
                m_IsSorted = false;
                m_IsBatched = false;
            }

            return packet;
        }

        template<typename T>
        T* SubmitAndGetCommandPtr(const T& commandData, const PacketMetadata& metadata = {}, CommandAllocator* allocator = nullptr)
        {
            CommandPacket* packet = Submit(commandData, metadata, allocator);
            if (!packet)
                return nullptr;
            return reinterpret_cast<T*>(packet->template GetCommandData<T>());
        }

        // Sort commands for optimal rendering (minimizes state changes)
        void SortCommands();

        // Batch compatible commands into instanced commands where possible
        void BatchCommands(CommandAllocator& allocator);

        void Execute(RendererAPI& rendererAPI);

        // Prepared, self-contained draw packets are partitioned into contiguous
        // ranges. GPU execution retains packet order; queries/stateful packets
        // and small buckets replay inline. One owner publishes bucket timings.
        void ExecuteParallel(RendererAPI& rendererAPI, u32 minCommandsPerItem = 32u);

        // Execute with per-command GPU timing (used during capture)
        void ExecuteWithGPUTiming(RendererAPI& rendererAPI);

        // Clear the bucket (doesn't free memory, just resets)
        void Clear();

        // Reset the bucket and free all memory
        void Reset(CommandAllocator& allocator);

        // Statistics
        struct Statistics
        {
            u32 TotalCommands = 0;   // Total commands in the bucket
            u32 BatchedCommands = 0; // Commands that were successfully batched
            u32 DrawCalls = 0;       // Actual draw calls executed
            u32 StateChanges = 0;    // State changes performed

            // Skinned auto-batching (issue #1031). Kept apart from
            // BatchedCommands — which counts every collapsed source — because
            // the skinned figure answers a different question: how much of a
            // crowd actually shares a pose. A crowd of independently phased
            // animations legitimately reports zero here, and that is the
            // measurement, not a failure.
            u32 SkinnedBatchedCommands = 0; // Skinned sources collapsed into an instanced draw
            u32 SkinnedBatchGroups = 0;     // Distinct same-pose groups those came from
            // Skinned draws that were eligible on geometry and material but
            // could not be considered, because their bone offsets were still
            // worker-local at batch time. Non-zero means RemapBoneOffsets did
            // not run before this pass — a plumbing error, not a pose result.
            u32 SkinnedBatchUnremapped = 0;
        };

        // Immutable replay for an already prepared range. Concurrent replays of
        // this bucket keep statistics in their callers, never in the bucket.
        [[nodiscard]] Statistics ReplayRange(RendererAPI& rendererAPI, sizet begin, sizet end) const;
        struct ParallelReplayPlan
        {
            u32 ItemCount = 1;
            u32 InstanceCapacity = 1;
        };
        [[nodiscard]] static ParallelReplayPlan PlanParallelReplay(std::span<CommandPacket* const> packets,
                                                                   u32 minCommandsPerItem = 32u);
        [[nodiscard]] static Statistics RecordPackets(RendererAPI& rendererAPI, std::span<CommandPacket* const> packets,
                                                      const std::optional<BucketViewState>& view = {}, u32 minCommandsPerItem = 32u);

        // Get execution statistics
        Statistics GetStatistics() const
        {
            return m_Stats;
        }

        // Batching / sorting policy for this bucket. The constructor already
        // takes this struct; exposing it afterwards lets a caller A/B the
        // batcher against itself on one real frame, which is how the skinned
        // batching of issue #1031 is measured and how its output is proved
        // pixel-identical to the unbatched path. Takes effect on the next
        // BatchCommands call.
        [[nodiscard]] CommandBucketConfig GetConfig() const
        {
            TUniqueLock<FMutex> lock(m_Mutex);
            return m_Config;
        }

        void SetConfig(const CommandBucketConfig& config)
        {
            TUniqueLock<FMutex> lock(m_Mutex);
            m_Config = ValidateConfig(config);
        }

        // Get command count
        sizet GetCommandCount() const
        {
            return m_CommandCount;
        }

        // Debugging/analysis methods to access commands
        std::span<CommandPacket* const> GetSortedCommands() const
        {
            return { m_Packets.GetData(), static_cast<sizet>(m_Packets.Num()) };
        }
        std::span<CommandPacket* const> GetPackets() const
        {
            return { m_Packets.GetData(), static_cast<sizet>(m_Packets.Num()) };
        }
        bool IsSorted() const
        {
            return m_IsSorted;
        }
        bool IsBatched() const
        {
            return m_IsBatched;
        }

        template<typename T>
        CommandPacket* CreateDrawCall()
        {
            OLO_CORE_ASSERT(m_Allocator, "CommandBucket::CreateDrawCall: No allocator available!");
            PacketMetadata initialMetadata; // Default metadata, to be enhanced later
            return m_Allocator->AllocatePacketWithCommand<T>(initialMetadata);
        }

        void SubmitPacket(CommandPacket* packet)
        {
            OLO_CORE_ASSERT(packet, "CommandBucket::SubmitPacket: Null packet!");
            TUniqueLock<FMutex> lock(m_Mutex);

            m_Keys.Add(packet->GetMetadata().m_SortKey.GetKey());
            m_Packets.Add(packet);
            ++m_CommandCount;
            ++m_Stats.TotalCommands;

            m_IsSorted = false;
            m_IsBatched = false;
        }

        // Thread-safe packet submission for parallel command generation
        // Uses thread-local batching to minimize atomic operations
        // @param packet The command packet to submit
        // @param workerIndex The worker thread index (0 to MAX_RENDER_WORKERS-1)
        void SubmitPacketParallel(CommandPacket* packet, u32 workerIndex);

        // Use an explicit worker index (no thread ID lookup needed)
        // This is the optimized path when contextIndex is already known from ParallelFor.
        // @param workerIndex The worker index (typically from ParallelFor contextIndex)
        void UseWorkerIndex(u32 workerIndex) const;

        // Merge all thread-local command ranges into a contiguous array
        // Must be called on the main thread after all workers complete
        // Should be called before SortCommands()
        void MergeThreadLocalCommands();

        // Remap bone buffer offsets from worker-local to global
        // Must be called after MergeThreadLocalCommands() and FrameDataBuffer::MergeScratchBuffers()
        // @param frameDataBuffer Reference to the frame data buffer for offset remapping
        void RemapBoneOffsets(class FrameDataBuffer& frameDataBuffer);

        // Prepare the bucket for parallel submission
        // Resets thread-local state and prepares arrays
        // Call at the start of each frame (in BeginScene)
        void PrepareForParallelSubmission();

        void SetAllocator(CommandAllocator* allocator)
        {
            m_Allocator = allocator;
        }
        CommandAllocator* GetAllocator() const
        {
            return m_Allocator;
        }

        // Timing accessors (populated during SortCommands/BatchCommands/Execute)
        f64 GetLastSortTimeMs() const
        {
            return m_LastSortTimeMs;
        }
        f64 GetLastBatchTimeMs() const
        {
            return m_LastBatchTimeMs;
        }
        f64 GetLastExecuteTimeMs() const
        {
            return m_LastExecuteTimeMs;
        }

        // Per-bucket view state — if set, bound to CommandDispatch before execution.
        // Enables multiple cameras per frame (split-screen, minimaps, reflections).
        void SetViewState(const BucketViewState& viewState)
        {
            m_ViewState = viewState;
        }
        void ClearViewState()
        {
            m_ViewState.reset();
        }
        bool HasViewState() const
        {
            return m_ViewState.has_value();
        }
        const std::optional<BucketViewState>& GetViewState() const
        {
            return m_ViewState;
        }

        // Register callbacks so Execute() can save/bind/restore view state
        // without a compile-time dependency on CommandDispatch.
        static void SetViewStateCallbacks(ViewStateReadFn readFn, ViewStateWriteFn writeFn);
        // Returns the required instance capacity, or zero for a primary-only
        // packet. Installed by CommandDispatch to keep the queue independent
        // of renderer services and diagnostic-query state.
        using ParallelReplayClassifier = u32 (*)(const CommandPacket&);
        static void SetParallelReplayClassifier(ParallelReplayClassifier classifier)
        {
            s_ParallelReplayClassifier = classifier;
        }

      private:
        [[nodiscard]] static Statistics ReplayPackets(RendererAPI& rendererAPI, std::span<CommandPacket* const> packets,
                                                      const std::optional<BucketViewState>& view);
        static inline ViewStateReadFn s_ViewStateReader = nullptr;
        static inline ViewStateWriteFn s_ViewStateWriter = nullptr;
        static inline ParallelReplayClassifier s_ParallelReplayClassifier = nullptr;
        // Transform buffer for instanced rendering
        class InstancedTransformBuffer
        {
          public:
            InstancedTransformBuffer(u32 maxInstances)
                : m_Capacity(maxInstances)
            {
                m_Buffer = new glm::mat4[m_Capacity];
            }

            ~InstancedTransformBuffer()
            {
                delete[] m_Buffer;
            }

            // Disallow copying
            InstancedTransformBuffer(const InstancedTransformBuffer&) = delete;
            InstancedTransformBuffer& operator=(const InstancedTransformBuffer&) = delete;

            // Allow moving
            InstancedTransformBuffer(InstancedTransformBuffer&& other) noexcept
                : m_Buffer(other.m_Buffer), m_Capacity(other.m_Capacity)
            {
                other.m_Buffer = nullptr;
                other.m_Capacity = 0;
            }

            InstancedTransformBuffer& operator=(InstancedTransformBuffer&& other) noexcept
            {
                if (this != &other)
                {
                    delete[] m_Buffer;
                    m_Buffer = other.m_Buffer;
                    m_Capacity = other.m_Capacity;
                    other.m_Buffer = nullptr;
                    other.m_Capacity = 0;
                }
                return *this;
            }

            glm::mat4* Allocate(u32 count)
            {
                if (m_Capacity < count)
                    return nullptr;
                return m_Buffer;
            }

            glm::mat4* GetBuffer()
            {
                return m_Buffer;
            }
            u32 GetCapacity() const
            {
                return m_Capacity;
            }

          private:
            glm::mat4* m_Buffer = nullptr;
            u32 m_Capacity = 0;
        };

        // Instance transform buffer management
        // Nodes retain the owning buffer object at a stable address.
        TDoubleLinkedList<InstancedTransformBuffer> m_TransformBuffers;
        // Maps for tracking instanced command transform buffers
        std::unordered_map<CommandPacket*, u32> m_PacketToBufferIndex;

        // Try to merge compatible commands for batching (works on array indices)
        bool TryMergeCommands(sizet targetIdx, sizet sourceIdx, CommandAllocator& allocator);

        // Clamp a config into what the rest of the pipeline can serve, loudly.
        // Zero would make BatchCommands emit an instanced command of zero
        // instances; anything above the dispatch cap would make it collapse
        // source draws the dispatcher then truncates away. Both are silent
        // wrong-image failures, so the clamp reports rather than just clamps.
        [[nodiscard]] static CommandBucketConfig ValidateConfig(const CommandBucketConfig& config);

        // Convert a DrawMeshCommand to DrawMeshInstancedCommand for batching
        CommandPacket* ConvertToInstanced(CommandPacket* meshPacket, CommandAllocator& allocator);

        // Split skinned batching candidates by pose and feed the same-pose
        // subsets into `groups` under distinct bonePaletteID ordinals
        // (issue #1031). Lives on the bucket because it reads m_Packets and
        // reports into m_Stats.
        using InstanceGroupMap = std::unordered_map<InstanceGroupKey, TArray64<sizet>, InstanceGroupKeyHash>;
        void PartitionSkinnedGroups(const InstanceGroupMap& candidates, InstanceGroupMap& groups);

        // Internal sort implementation — caller must hold m_Mutex
        void SortCommandsInternal();

        // ——— Flat array storage (replaces linked list) ———
        // Keys and packets are stored in 1:1 correspondence.
        // Keys are pre-extracted once during AddCommand for cache-friendly sorting.
        TArray64<u64> m_Keys;
        TArray64<CommandPacket*> m_Packets;

        // Count of commands in the bucket
        sizet m_CommandCount = 0;

        // Configuration
        CommandBucketConfig m_Config;

        // Flags for bucket state
        bool m_IsSorted = false;
        bool m_IsBatched = false;

        // Statistics
        Statistics m_Stats;

        // Allocator for command memory (must be set before use)
        CommandAllocator* m_Allocator = nullptr;

        // Timing data
        f64 m_LastSortTimeMs = 0.0;
        f64 m_LastBatchTimeMs = 0.0;
        f64 m_LastExecuteTimeMs = 0.0;

        // Optional per-bucket view state for multi-camera rendering
        std::optional<BucketViewState> m_ViewState;

        mutable FMutex m_Mutex;

        // ====================================================================
        // Thread-Local Storage for Parallel Command Generation
        // Following Molecular Matters Version 7 pattern
        // ====================================================================

        // Cache-line-aligned per-thread slots to prevent false sharing
        TLSBucketSlot m_TLSSlots[MAX_RENDER_WORKERS];

        // Global command packet array for parallel submission
        // Workers claim batches of TLS_BATCH_SIZE slots atomically
        TArray64<CommandPacket*> m_ParallelCommands;

        // Atomic counter for claiming batches of slots
        std::atomic<u32> m_NextBatchStart{ 0 };

        // Total commands submitted across all workers (for statistics)
        std::atomic<u32> m_ParallelCommandCount{ 0 };

        // Whether we're currently in parallel submission mode
        bool m_ParallelSubmissionActive = false;

        // Claim a batch of slots for a worker thread
        // Returns the start index of the claimed batch
        u32 ClaimBatch();
    };
} // namespace OloEngine
