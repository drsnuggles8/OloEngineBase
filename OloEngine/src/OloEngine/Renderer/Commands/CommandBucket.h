#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/Platform.h"
#include "CommandPacket.h"
#include "CommandAllocator.h"
#include <vector>
#include <unordered_map>
#include <atomic>
#include <functional>

#include "OloEngine/Threading/Mutex.h"

#include <optional>

namespace OloEngine
{
    // Forward declarations
    class RendererAPI;
    class FrameDataBuffer;

    // Key for identifying groups of draw calls that can be instanced together.
    // Commands sharing the same key render the same mesh with the same material
    // and render state — only the per-instance transform differs.
    struct InstanceGroupKey
    {
        AssetHandle meshHandle = 0;
        u16 materialDataIndex = 0;
        u16 renderStateIndex = 0;

        bool operator==(const InstanceGroupKey& other) const = default;
    };

    struct InstanceGroupKeyHash
    {
        sizet operator()(const InstanceGroupKey& key) const
        {
            sizet h = std::hash<u64>{}(static_cast<u64>(key.meshHandle));
            h ^= std::hash<u16>{}(key.materialDataIndex) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<u16>{}(key.renderStateIndex) + 0x9e3779b9 + (h << 6) + (h >> 2);
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
        bool EnableBatching = true; // Attempt to batch similar commands
        u32 MaxMeshInstances = 100; // Maximum instances for instanced mesh rendering
        u32 InitialCapacity = 1024; // Initial capacity for command arrays
    };

    // Cache-line padded slot for thread-local storage
    // Prevents false sharing between worker threads
    struct alignas(OLO_PLATFORM_CACHE_LINE_SIZE) TLSBucketSlot
    {
        u32 offset = 0;     // Current write offset in thread-local range
        u32 remaining = 0;  // Remaining slots in current batch
        u32 batchStart = 0; // Start offset of current batch in global array
        // Padding to fill cache line
        u8 _padding[OLO_PLATFORM_CACHE_LINE_SIZE - 3 * sizeof(u32)];
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

            CommandPacket* packet = allocator->CreateCommandPacket(commandData, metadata);
            if (packet)
            {
                m_Keys.push_back(metadata.m_SortKey.GetKey());
                m_Packets.push_back(packet);
                m_CommandCount++;
                m_Stats.TotalCommands++;

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
            return reinterpret_cast<T*>(packet->GetCommandData());
        }

        // Sort commands for optimal rendering (minimizes state changes)
        void SortCommands();

        // Batch compatible commands into instanced commands where possible
        void BatchCommands(CommandAllocator& allocator);

        void Execute(RendererAPI& rendererAPI);

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
        };

        // Get execution statistics
        Statistics GetStatistics() const
        {
            return m_Stats;
        }

        // Get command count
        sizet GetCommandCount() const
        {
            return m_CommandCount;
        }

        // Debugging/analysis methods to access commands
        const std::vector<CommandPacket*>& GetSortedCommands() const
        {
            return m_Packets;
        }
        const std::vector<CommandPacket*>& GetPackets() const
        {
            return m_Packets;
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

            m_Keys.push_back(packet->GetMetadata().m_SortKey.GetKey());
            m_Packets.push_back(packet);
            m_CommandCount++;
            m_Stats.TotalCommands++;

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
        void UseWorkerIndex(u32 workerIndex);

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

      private:
        static inline ViewStateReadFn s_ViewStateReader = nullptr;
        static inline ViewStateWriteFn s_ViewStateWriter = nullptr;
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
        std::vector<InstancedTransformBuffer> m_TransformBuffers;
        // Maps for tracking instanced command transform buffers
        std::unordered_map<CommandPacket*, u32> m_PacketToBufferIndex;

        // Try to merge compatible commands for batching (works on array indices)
        bool TryMergeCommands(sizet targetIdx, sizet sourceIdx, CommandAllocator& allocator);

        // Convert a DrawMeshCommand to DrawMeshInstancedCommand for batching
        CommandPacket* ConvertToInstanced(CommandPacket* meshPacket, CommandAllocator& allocator);

        // Internal sort implementation — caller must hold m_Mutex
        void SortCommandsInternal();

        // ——— Flat array storage (replaces linked list) ———
        // Keys and packets are stored in 1:1 correspondence.
        // Keys are pre-extracted once during AddCommand for cache-friendly sorting.
        std::vector<u64> m_Keys;
        std::vector<CommandPacket*> m_Packets;

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
        std::vector<CommandPacket*> m_ParallelCommands;

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
