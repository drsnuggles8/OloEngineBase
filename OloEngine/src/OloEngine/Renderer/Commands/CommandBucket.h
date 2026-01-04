#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/Platform.h"
#include "CommandPacket.h"
#include "CommandAllocator.h"
#include <vector>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <thread>

namespace OloEngine
{
    // Forward declarations
    class RendererAPI;
    class FrameDataBuffer;

    // Maximum number of worker threads for parallel command generation
    // This should match the maximum expected worker thread count
    static constexpr u32 MAX_RENDER_WORKERS = 16;

    // Batch size for thread-local slot claiming (reduces atomic operations)
    // Following Molecular Matters Version 7: claim 32 entries at a time
    static constexpr u32 TLS_BATCH_SIZE = 32;

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
        u32 offset = 0;         // Current write offset in thread-local range
        u32 remaining = 0;      // Remaining slots in current batch
        u32 batchStart = 0;     // Start offset of current batch in global array
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
            // Temporarily disable strict requirements for command data while we use object references
            // TODO: Restore this check when implementing a proper resource manager with handles
            // static_assert(std::is_trivially_copyable<T>::value && std::is_standard_layout<T>::value,
            // 	"Command data must be trivially copyable and have standard layout");
            std::lock_guard<std::mutex> lock(m_Mutex);

            CommandPacket* packet = allocator->CreateCommandPacket(commandData, metadata);
            if (packet)
            {
                // The rest of your code remains the same
                if (!m_Head)
                {
                    m_Head = packet;
                    m_Tail = packet;
                }
                else
                {
                    m_Tail->SetNext(packet);
                    m_Tail = packet;
                }

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

        // Debugging methods to access commands for analysis
        const std::vector<CommandPacket*>& GetSortedCommands() const
        {
            return m_SortedCommands;
        }
        CommandPacket* GetCommandHead() const
        {
            return m_Head;
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
            std::lock_guard<std::mutex> lock(m_Mutex);

            AddCommand(packet);
        }

        // Thread-safe packet submission for parallel command generation
        // Uses thread-local batching to minimize atomic operations
        // @param packet The command packet to submit
        // @param workerIndex The worker thread index (0 to MAX_RENDER_WORKERS-1)
        void SubmitPacketParallel(CommandPacket* packet, u32 workerIndex);

        // Register the current thread as a worker and get its index
        // Call this once per worker thread before submitting commands
        // @return Worker index for use with SubmitPacketParallel
        u32 RegisterWorkerThread();

        // Get the worker index for the current thread (returns -1 if not registered)
        i32 GetCurrentWorkerIndex() const;

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

      private:
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

        // Try to merge compatible commands for batching
        bool TryMergeCommands(CommandPacket* target, CommandPacket* source, CommandAllocator& allocator);

        // Convert a DrawMeshCommand to DrawMeshInstancedCommand for batching
        CommandPacket* ConvertToInstanced(CommandPacket* meshPacket, CommandAllocator& allocator);

        // Head of the linked list of commands
        CommandPacket* m_Head = nullptr;

        // Tail of the linked list for O(1) append
        CommandPacket* m_Tail = nullptr;

        // Count of commands in the bucket
        sizet m_CommandCount = 0;

        // Cached sorted array of commands (built during SortCommands)
        std::vector<CommandPacket*> m_SortedCommands;

        // Configuration
        CommandBucketConfig m_Config;

        // Flags for bucket state
        bool m_IsSorted = false;
        bool m_IsBatched = false;

        // Statistics
        Statistics m_Stats;

        // Allocator for command memory (must be set before use)
        CommandAllocator* m_Allocator = nullptr;

        mutable std::mutex m_Mutex;

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

        // Thread ID to worker index mapping
        mutable std::mutex m_ThreadMapMutex;
        std::unordered_map<std::thread::id, u32> m_ThreadToWorkerIndex;
        std::atomic<u32> m_NextWorkerIndex{ 0 };

        // Whether we're currently in parallel submission mode
        bool m_ParallelSubmissionActive = false;

        // Claim a batch of slots for a worker thread
        // Returns the start index of the claimed batch
        u32 ClaimBatch();
    };
} // namespace OloEngine
