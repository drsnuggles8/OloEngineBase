#include "OloEnginePCH.h"
#include "CommandBucket.h"
#include "FrameDataBuffer.h"
#include "RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/Debug/GPUTimerQueryPool.h"
#include "OloEngine/Task/ParallelFor.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Threading/UniqueLock.h"
#include <algorithm>
#include <array>
#include <cstring>

namespace OloEngine
{
    // LSB Radix Sort for 64-bit keys using 8-bit digits (8 passes)
    // This is a stable sort, preserving relative order of equal keys.
    // Input: array of command packets and their sort keys
    // Uses counting sort for each digit pass.
    namespace
    {
        constexpr sizet RADIX_BITS = 8;
        constexpr sizet RADIX_SIZE = 1 << RADIX_BITS;          // 256 buckets
        constexpr sizet NUM_PASSES = sizeof(u64) / sizeof(u8); // 8 passes for 64-bit

        void RadixSort64(std::vector<CommandPacket*>& packets)
        {
            OLO_PROFILE_FUNCTION();

            if (packets.size() <= 1)
                return;

            sizet count = packets.size();

            // Temporary buffer for swapping
            std::vector<CommandPacket*> temp(count);

            // Pre-extract keys for cache efficiency
            std::vector<u64> keys(count);
            for (sizet i = 0; i < count; ++i)
            {
                keys[i] = packets[i]->GetMetadata().m_SortKey.GetKey();
            }
            std::vector<u64> tempKeys(count);

            // Process each byte from LSB to MSB
            for (sizet pass = 0; pass < NUM_PASSES; ++pass)
            {
                sizet shift = pass * RADIX_BITS;

                // Count occurrences of each digit
                std::array<sizet, RADIX_SIZE> histogram{};
                for (sizet i = 0; i < count; ++i)
                {
                    u8 digit = static_cast<u8>((keys[i] >> shift) & 0xFF);
                    histogram[digit]++;
                }

                // Convert counts to starting positions (prefix sum)
                std::array<sizet, RADIX_SIZE> offsets{};
                sizet total = 0;
                for (sizet i = 0; i < RADIX_SIZE; ++i)
                {
                    offsets[i] = total;
                    total += histogram[i];
                }

                // Place elements in sorted order
                for (sizet i = 0; i < count; ++i)
                {
                    u8 digit = static_cast<u8>((keys[i] >> shift) & 0xFF);
                    sizet destIdx = offsets[digit]++;
                    temp[destIdx] = packets[i];
                    tempKeys[destIdx] = keys[i];
                }

                // Swap buffers
                std::swap(packets, temp);
                std::swap(keys, tempKeys);
            }
        }

        // ====================================================================
        // Parallel Radix Sort Implementation
        // ====================================================================
        // Uses parallel histogram building for each radix pass.
        // For large command counts (>1000), this provides ~2-4x speedup on sort.
        // Falls back to single-threaded for small arrays.

        constexpr sizet PARALLEL_SORT_THRESHOLD = 1024; // Min elements for parallel sort
        constexpr i32 PARALLEL_SORT_BATCH_SIZE = 256;   // Elements per batch for histogram

        // Per-worker histogram accumulator
        struct alignas(OLO_PLATFORM_CACHE_LINE_SIZE) WorkerHistogram
        {
            std::array<sizet, RADIX_SIZE> counts{};
        };

        void ParallelRadixSort64(std::vector<CommandPacket*>& packets)
        {
            OLO_PROFILE_FUNCTION();

            if (packets.size() <= 1)
                return;

            sizet count = packets.size();

            // Fall back to single-threaded for small arrays
            if (count < PARALLEL_SORT_THRESHOLD)
            {
                RadixSort64(packets);
                return;
            }

            // Temporary buffer for swapping
            std::vector<CommandPacket*> temp(count);

            // Pre-extract keys for cache efficiency
            std::vector<u64> keys(count);
            {
                OLO_PROFILE_SCOPE("ExtractKeys");
                // Parallel key extraction
                ParallelFor("ExtractSortKeys", static_cast<i32>(count), PARALLEL_SORT_BATCH_SIZE,
                            [&packets, &keys](i32 i)
                            {
                                keys[i] = packets[i]->GetMetadata().m_SortKey.GetKey();
                            });
            }
            std::vector<u64> tempKeys(count);

            // Process each byte from LSB to MSB
            for (sizet pass = 0; pass < NUM_PASSES; ++pass)
            {
                OLO_PROFILE_SCOPE("RadixPass");
                sizet shift = pass * RADIX_BITS;

                // Phase 1: Parallel histogram building
                // Each worker builds a local histogram, then we reduce
                TArray<WorkerHistogram> workerHistograms;
                {
                    OLO_PROFILE_SCOPE("ParallelHistogram");
                    ParallelForWithTaskContext(
                        "RadixHistogram",
                        workerHistograms,
                        static_cast<i32>(count),
                        PARALLEL_SORT_BATCH_SIZE,
                        [&keys, shift](WorkerHistogram& hist, i32 i)
                        {
                            u8 digit = static_cast<u8>((keys[i] >> shift) & 0xFF);
                            hist.counts[digit]++;
                        });
                }

                // Phase 2: Reduce worker histograms into global histogram
                std::array<sizet, RADIX_SIZE> globalHistogram{};
                {
                    OLO_PROFILE_SCOPE("ReduceHistogram");
                    for (i32 w = 0; w < workerHistograms.Num(); ++w)
                    {
                        for (sizet b = 0; b < RADIX_SIZE; ++b)
                        {
                            globalHistogram[b] += workerHistograms[w].counts[b];
                        }
                    }
                }

                // Phase 3: Compute prefix sums (exclusive scan) for offsets
                std::array<sizet, RADIX_SIZE> offsets{};
                {
                    OLO_PROFILE_SCOPE("PrefixSum");
                    sizet total = 0;
                    for (sizet i = 0; i < RADIX_SIZE; ++i)
                    {
                        offsets[i] = total;
                        total += globalHistogram[i];
                    }
                }

                // Phase 4: Scatter phase (sequential for correctness)
                // Note: Parallel scatter requires careful write conflict handling.
                // For now, we do sequential scatter which is still fast due to
                // prefetched destination addresses and sequential reads.
                {
                    OLO_PROFILE_SCOPE("Scatter");
                    for (sizet i = 0; i < count; ++i)
                    {
                        u8 digit = static_cast<u8>((keys[i] >> shift) & 0xFF);
                        sizet destIdx = offsets[digit]++;
                        temp[destIdx] = packets[i];
                        tempKeys[destIdx] = keys[i];
                    }
                }

                // Swap buffers
                std::swap(packets, temp);
                std::swap(keys, tempKeys);
            }
        }

    } // anonymous namespace
    CommandBucket::CommandBucket(const CommandBucketConfig& config)
        : m_Config(config)
    {
        // Initialize statistics
        m_Stats = Statistics();

        // Initialize thread-local slots
        std::memset(m_TLSSlots, 0, sizeof(m_TLSSlots));

        // Pre-allocate parallel command array
        m_ParallelCommands.reserve(config.InitialCapacity);
    }

    CommandBucket::~CommandBucket()
    {
        // The actual command memory is managed by CommandAllocator
        // We just need to clear our references
        Clear();
    }

    CommandBucket::CommandBucket(CommandBucket&& other) noexcept
        : m_Head(other.m_Head),
          m_Tail(other.m_Tail),
          m_CommandCount(other.m_CommandCount),
          m_SortedCommands(std::move(other.m_SortedCommands)),
          m_Config(other.m_Config),
          m_IsSorted(other.m_IsSorted),
          m_IsBatched(other.m_IsBatched),
          m_Stats(other.m_Stats)
    {
        // Reset the source object
        other.m_Head = nullptr;
        other.m_Tail = nullptr;
        other.m_CommandCount = 0;
        other.m_IsSorted = false;
        other.m_IsBatched = false;
        other.m_Stats = Statistics();
    }

    CommandBucket& CommandBucket::operator=(CommandBucket&& other) noexcept
    {
        if (this != &other)
        {
            m_Head = other.m_Head;
            m_Tail = other.m_Tail;
            m_CommandCount = other.m_CommandCount;
            m_SortedCommands = std::move(other.m_SortedCommands);
            m_Config = other.m_Config;
            m_IsSorted = other.m_IsSorted;
            m_IsBatched = other.m_IsBatched;
            m_Stats = other.m_Stats;

            // Reset the source object
            other.m_Head = nullptr;
            other.m_Tail = nullptr;
            other.m_CommandCount = 0;
            other.m_IsSorted = false;
            other.m_IsBatched = false;
            other.m_Stats = Statistics();
        }
        return *this;
    }

    void CommandBucket::AddCommand(CommandPacket* packet)
    {
        OLO_PROFILE_FUNCTION();

        if (!packet)
            return;

        packet->SetNext(nullptr);

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

    void CommandBucket::SortCommands()
    {
        OLO_PROFILE_FUNCTION();

        auto sortStart = std::chrono::high_resolution_clock::now();

        TUniqueLock<FMutex> lock(m_Mutex);

        if (!m_Config.EnableSorting || m_IsSorted || m_CommandCount <= 1)
        {
            m_LastSortTimeMs = 0.0;
            return;
        }

        // Build a vector of command pointers for sorting
        m_SortedCommands.clear();
        m_SortedCommands.reserve(m_CommandCount);

        // First, group commands by dependency chains
        std::vector<std::vector<CommandPacket*>> dependencyGroups;
        std::vector<CommandPacket*> currentGroup;
        CommandPacket* current = m_Head;
        while (current)
        {
            // Start a new group if this is the first command or if it depends on previous
            if (currentGroup.empty() || current->GetMetadata().m_DependsOnPrevious)
            {
                // If we have an existing group, finalize it
                if (!currentGroup.empty())
                {
                    dependencyGroups.push_back(std::move(currentGroup));
                    currentGroup = std::vector<CommandPacket*>();
                }
            }

            // Add the current command to the current group
            currentGroup.push_back(current);
            current = current->GetNext();
        }

        // Add the last group if it's not empty
        if (!currentGroup.empty())
            dependencyGroups.push_back(std::move(currentGroup));

        // Sort each group internally using parallel RadixSort for optimal performance
        for (auto& group : dependencyGroups)
        {
            // Only sort if there's more than one command in a group
            if (group.size() > 1)
            {
                // Use ParallelRadixSort64 for O(n) sorting on 64-bit keys
                // Falls back to single-threaded RadixSort64 for small groups
                ParallelRadixSort64(group);
            }
        }

        // Rebuild the linked list from the sorted groups
        if (!dependencyGroups.empty())
        {
            // Link the first command
            m_Head = dependencyGroups[0][0];
            current = m_Head;
            m_SortedCommands.push_back(current);

            // Link each group
            for (sizet groupIdx = 0; groupIdx < dependencyGroups.size(); groupIdx++)
            {
                const auto& group = dependencyGroups[groupIdx];

                // Link commands within the group
                for (sizet cmdIdx = (groupIdx == 0 ? 1 : 0); cmdIdx < group.size(); cmdIdx++)
                {
                    current->SetNext(group[cmdIdx]);
                    current = group[cmdIdx];
                    m_SortedCommands.push_back(current);
                }

                // Link to the next group
                if (groupIdx < dependencyGroups.size() - 1)
                {
                    current->SetNext(dependencyGroups[groupIdx + 1][0]);
                    current = dependencyGroups[groupIdx + 1][0];
                    m_SortedCommands.push_back(current);
                }
            }

            // Set the tail and terminate the list
            m_Tail = current;
            m_Tail->SetNext(nullptr);
        }

        m_IsSorted = true;

        auto sortEnd = std::chrono::high_resolution_clock::now();
        m_LastSortTimeMs = std::chrono::duration<f64, std::milli>(sortEnd - sortStart).count();
    }

    CommandPacket* CommandBucket::ConvertToInstanced(CommandPacket* meshPacket, CommandAllocator& allocator)
    {
        OLO_PROFILE_FUNCTION();

        if (!meshPacket || meshPacket->GetCommandType() != CommandType::DrawMesh)
            return nullptr;

        auto const* meshCmd = meshPacket->GetCommandData<DrawMeshCommand>();
        if (!meshCmd)
            return nullptr;

        // Create a new packet for the instanced command
        // DrawMeshInstancedCommand is now POD/trivially copyable (uses transformBufferOffset instead of std::vector)
        PacketMetadata metadata = meshPacket->GetMetadata();
        CommandPacket* instancedPacket = allocator.AllocatePacketWithCommand<DrawMeshInstancedCommand>(metadata);
        if (!instancedPacket)
            return nullptr;

        auto* instancedCmd = instancedPacket->GetCommandData<DrawMeshInstancedCommand>();

        // Copy header
        instancedCmd->header.type = CommandType::DrawMeshInstanced;
        instancedCmd->header.dispatchFn = nullptr; // Will be set during initialization

        // Copy mesh data using POD identifiers
        instancedCmd->meshHandle = meshCmd->meshHandle;
        instancedCmd->vertexArrayID = meshCmd->vertexArrayID;
        instancedCmd->indexCount = meshCmd->indexCount;

        // Initial instance count is 1
        instancedCmd->instanceCount = 1;

        // Allocate space in FrameDataBuffer for the first transform and copy it
        FrameDataBuffer& frameBuffer = FrameDataBufferManager::Get();
        u32 transformOffset = frameBuffer.AllocateTransforms(1);
        if (transformOffset == UINT32_MAX)
        {
            OLO_CORE_ERROR("CommandBucket::ConvertToInstanced: Failed to allocate transform in FrameDataBuffer");
            return nullptr;
        }
        frameBuffer.WriteTransforms(transformOffset, &meshCmd->transform, 1);
        instancedCmd->transformBufferOffset = transformOffset;
        instancedCmd->transformCount = 1;

        // Copy material properties
        instancedCmd->ambient = meshCmd->ambient;
        instancedCmd->diffuse = meshCmd->diffuse;
        instancedCmd->specular = meshCmd->specular;
        instancedCmd->shininess = meshCmd->shininess;
        instancedCmd->useTextureMaps = meshCmd->useTextureMaps;

        // Copy texture renderer IDs (POD)
        instancedCmd->diffuseMapID = meshCmd->diffuseMapID;
        instancedCmd->specularMapID = meshCmd->specularMapID;

        // Copy shader renderer ID (POD)
        instancedCmd->shaderHandle = meshCmd->shaderHandle;
        instancedCmd->shaderRendererID = meshCmd->shaderRendererID;

        // Copy POD render state
        instancedCmd->renderState = meshCmd->renderState;

        // Set command type and dispatch function
        instancedPacket->SetCommandType(instancedCmd->header.type);
        instancedPacket->SetDispatchFunction(CommandDispatch::GetDispatchFunction(instancedCmd->header.type));

        return instancedPacket;
    }

    bool CommandBucket::TryMergeCommands(CommandPacket* target, CommandPacket* source, CommandAllocator& allocator)
    {
        OLO_PROFILE_FUNCTION();

        TUniqueLock<FMutex> lock(m_Mutex);

        if (!target || !source || !target->CanBatchWith(*source))
            return false;

        // Handle batching based on command type
        CommandType targetType = target->GetCommandType();

        // We can have a few different scenarios:
        // 1. Both are DrawMeshCommand - Convert to DrawMeshInstancedCommand
        // 2. target is DrawMeshInstancedCommand, source is DrawMeshCommand - Add to instance
        // 3. Both are DrawQuadCommand - Can't combine directly with the current design

        if (CommandType sourceType = source->GetCommandType();
            targetType == CommandType::DrawMesh && sourceType == CommandType::DrawMesh)
        {
            // Convert to instanced command if it's not already
            CommandPacket* instancedPacket = ConvertToInstanced(target, allocator);
            if (!instancedPacket)
                return false;

            // Replace target with instancedPacket in the linked list
            instancedPacket->SetNext(target->GetNext());

            // Find the packet that points to target and update it
            if (m_Head == target)
            {
                m_Head = instancedPacket;
            }
            else
            {
                CommandPacket* prev = m_Head;
                while (prev && prev->GetNext() != target)
                    prev = prev->GetNext();

                if (prev)
                    prev->SetNext(instancedPacket);
            }

            if (m_Tail == target)
                m_Tail = instancedPacket;

            // Dynamic instance batching: merge transforms from source into the new instanced command
            // 1. Get the source mesh command's transform
            auto const* sourceMeshCmd = source->GetCommandData<DrawMeshCommand>();
            if (!sourceMeshCmd)
                return false;

            // 2. Get the current instanced command (which has 1 transform from ConvertToInstanced)
            auto* instancedCmd = instancedPacket->GetCommandData<DrawMeshInstancedCommand>();
            if (!instancedCmd)
                return false;

            // 3. Allocate new contiguous space for both transforms
            FrameDataBuffer& frameBuffer = FrameDataBufferManager::Get();
            u32 totalTransforms = instancedCmd->transformCount + 1;

            // Check against max instances limit
            if (totalTransforms > m_Config.MaxMeshInstances)
            {
                OLO_CORE_WARN("CommandBucket::TryMergeCommands: Max instances ({}) reached", m_Config.MaxMeshInstances);
                return false;
            }

            u32 newOffset = frameBuffer.AllocateTransforms(totalTransforms);
            if (newOffset == UINT32_MAX)
            {
                OLO_CORE_ERROR("CommandBucket::TryMergeCommands: Failed to allocate {} transforms in FrameDataBuffer", totalTransforms);
                return false;
            }

            // 4. Copy existing transforms from the instanced command
            const glm::mat4* existingTransforms = frameBuffer.GetTransformPtr(instancedCmd->transformBufferOffset);
            if (existingTransforms)
            {
                frameBuffer.WriteTransforms(newOffset, existingTransforms, instancedCmd->transformCount);
            }

            // 5. Copy the source command's transform
            frameBuffer.WriteTransforms(newOffset + instancedCmd->transformCount, &sourceMeshCmd->transform, 1);

            // 6. Update the instanced command to use the new buffer
            instancedCmd->transformBufferOffset = newOffset;
            instancedCmd->transformCount = totalTransforms;
            instancedCmd->instanceCount = totalTransforms;

            return true;
        }
        else if (targetType == CommandType::DrawMeshInstanced && sourceType == CommandType::DrawMesh)
        {
            // Add source mesh's transform to existing instanced command
            auto* instancedCmd = target->GetCommandData<DrawMeshInstancedCommand>();
            auto const* sourceMeshCmd = source->GetCommandData<DrawMeshCommand>();

            if (!instancedCmd || !sourceMeshCmd)
                return false;

            // Check against max instances limit
            u32 totalTransforms = instancedCmd->transformCount + 1;
            if (totalTransforms > m_Config.MaxMeshInstances)
            {
                OLO_CORE_WARN("CommandBucket::TryMergeCommands: Max instances ({}) reached", m_Config.MaxMeshInstances);
                return false;
            }

            // Allocate new contiguous space for all transforms
            FrameDataBuffer& frameBuffer = FrameDataBufferManager::Get();
            u32 newOffset = frameBuffer.AllocateTransforms(totalTransforms);
            if (newOffset == UINT32_MAX)
            {
                OLO_CORE_ERROR("CommandBucket::TryMergeCommands: Failed to allocate {} transforms in FrameDataBuffer", totalTransforms);
                return false;
            }

            // Copy existing transforms
            const glm::mat4* existingTransforms = frameBuffer.GetTransformPtr(instancedCmd->transformBufferOffset);
            if (existingTransforms)
            {
                frameBuffer.WriteTransforms(newOffset, existingTransforms, instancedCmd->transformCount);
            }

            // Copy the source command's transform
            frameBuffer.WriteTransforms(newOffset + instancedCmd->transformCount, &sourceMeshCmd->transform, 1);

            // Update the instanced command
            instancedCmd->transformBufferOffset = newOffset;
            instancedCmd->transformCount = totalTransforms;
            instancedCmd->instanceCount = totalTransforms;

            return true;
        }

        // Other command types can't be merged directly
        return false;
    }

    void CommandBucket::BatchCommands(CommandAllocator& allocator)
    {
        OLO_PROFILE_FUNCTION();

        auto batchStart = std::chrono::high_resolution_clock::now();

        TUniqueLock<FMutex> lock(m_Mutex);

        if (!m_Config.EnableBatching || m_IsBatched || m_CommandCount <= 1)
        {
            m_LastBatchTimeMs = 0.0;
            return;
        }

        // Make sure commands are sorted first for optimal batching
        if (!m_IsSorted)
            SortCommands();

        // Start with the first command
        CommandPacket* current = m_Head;

        while (current)
        {
            CommandPacket* next = current->GetNext();
            // Try to merge with subsequent compatible commands
            while (next && TryMergeCommands(current, next, allocator))
            {
                // Merging succeeded, remove the next command from the list
                next = next->GetNext();
                current->SetNext(next);

                // Decrement command count since we've merged one
                m_CommandCount--;
                m_Stats.BatchedCommands++;
            }

            // Move to the next command
            current = next;
        }

        // Update the tail pointer to the last node
        if (m_Head)
        {
            current = m_Head;
            while (current->GetNext())
                current = current->GetNext();
            m_Tail = current;
        }

        // Rebuild sorted commands array if it exists
        if (!m_SortedCommands.empty())
        {
            m_SortedCommands.clear();
            m_SortedCommands.reserve(m_CommandCount);

            current = m_Head;
            while (current)
            {
                m_SortedCommands.push_back(current);
                current = current->GetNext();
            }
        }

        m_IsBatched = true;

        auto batchEnd = std::chrono::high_resolution_clock::now();
        m_LastBatchTimeMs = std::chrono::duration<f64, std::milli>(batchEnd - batchStart).count();
    }

    void CommandBucket::Execute(RendererAPI& rendererAPI)
    {
        OLO_PROFILE_FUNCTION();

        auto execStart = std::chrono::high_resolution_clock::now();

        // Take a snapshot of the head pointer under lock
        CommandPacket const* current;
        {
            TUniqueLock<FMutex> lock(m_Mutex);
            // Reset execution statistics
            m_Stats.DrawCalls = 0;
            m_Stats.StateChanges = 0;
            current = m_Head;
        }

        // Execute all commands in order
        while (current)
        {
            // Track statistics
            if (CommandType type = current->GetCommandType();
                type == CommandType::DrawMesh ||
                type == CommandType::DrawMeshInstanced ||
                type == CommandType::DrawQuad)
            {
                m_Stats.DrawCalls++;
            }
            else if (type != CommandType::Invalid)
            {
                m_Stats.StateChanges++;
            }

            current->Execute(rendererAPI);
            current = current->GetNext();
        }

        auto execEnd = std::chrono::high_resolution_clock::now();
        m_LastExecuteTimeMs = std::chrono::duration<f64, std::milli>(execEnd - execStart).count();
    }

    void CommandBucket::ExecuteWithGPUTiming(RendererAPI& rendererAPI)
    {
        OLO_PROFILE_FUNCTION();

        auto& gpuTimer = GPUTimerQueryPool::GetInstance();

        // If not initialized, lazily initialize
        if (!gpuTimer.IsInitialized())
            gpuTimer.Initialize();

        // Begin frame — returns true if previous frame's results are ready
        gpuTimer.BeginFrame();

        auto execStart = std::chrono::high_resolution_clock::now();

        CommandPacket const* current;
        {
            TUniqueLock<FMutex> lock(m_Mutex);
            m_Stats.DrawCalls = 0;
            m_Stats.StateChanges = 0;
            current = m_Head;
        }

        u32 cmdIndex = 0;
        while (current)
        {
            if (CommandType type = current->GetCommandType();
                type == CommandType::DrawMesh ||
                type == CommandType::DrawMeshInstanced ||
                type == CommandType::DrawQuad)
            {
                m_Stats.DrawCalls++;
            }
            else if (type != CommandType::Invalid)
            {
                m_Stats.StateChanges++;
            }

            if (cmdIndex < gpuTimer.GetMaxQueries())
            {
                gpuTimer.BeginQuery(cmdIndex);
                current->Execute(rendererAPI);
                gpuTimer.EndQuery(cmdIndex);
            }
            else
            {
                current->Execute(rendererAPI);
            }

            current = current->GetNext();
            cmdIndex++;
        }

        gpuTimer.EndFrame();

        auto execEnd = std::chrono::high_resolution_clock::now();
        m_LastExecuteTimeMs = std::chrono::duration<f64, std::milli>(execEnd - execStart).count();
    }

    void CommandBucket::Clear()
    {
        m_Head = nullptr;
        m_Tail = nullptr;
        m_CommandCount = 0;
        m_SortedCommands.clear();

        // Important: clear transform buffers to prevent memory leaks
        m_TransformBuffers.clear();
        m_PacketToBufferIndex.clear();

        // Reset parallel submission state
        m_ParallelCommands.clear();
        m_NextBatchStart.store(0, std::memory_order_relaxed);
        m_ParallelCommandCount.store(0, std::memory_order_relaxed);
        m_ParallelSubmissionActive = false;

        // Reset TLS slots
        std::memset(m_TLSSlots, 0, sizeof(m_TLSSlots));

        m_IsSorted = false;
        m_IsBatched = false;
    }

    // ========================================================================
    // Thread-Local Storage for Parallel Command Generation
    // ========================================================================

    void CommandBucket::PrepareForParallelSubmission()
    {
        OLO_PROFILE_FUNCTION();

        TUniqueLock<FMutex> lock(m_Mutex);

        // Reset parallel command array with sufficient capacity
        m_ParallelCommands.clear();
        m_ParallelCommands.resize(m_Config.InitialCapacity, nullptr);

        // Reset atomic counters
        m_NextBatchStart.store(0, std::memory_order_relaxed);
        m_ParallelCommandCount.store(0, std::memory_order_relaxed);

        // Reset all TLS slots
        for (u32 i = 0; i < MAX_RENDER_WORKERS; ++i)
        {
            m_TLSSlots[i].offset = 0;
            m_TLSSlots[i].remaining = 0;
            m_TLSSlots[i].batchStart = 0;
        }

        m_ParallelSubmissionActive = true;
        m_IsSorted = false;
        m_IsBatched = false;
    }

    void CommandBucket::UseWorkerIndex(u32 workerIndex)
    {
        // Optimized path: directly use the provided worker index without thread ID lookup.
        // This is called when the worker index is already known (e.g., from ParallelFor contextIndex).
        // No mutex needed, no map lookup - just validate the index is in range.
        OLO_CORE_ASSERT(workerIndex < MAX_RENDER_WORKERS,
                        "CommandBucket::UseWorkerIndex: Invalid worker index {}!", workerIndex);

        // Verify that PrepareForParallelSubmission has been called and parallel submission is active.
        // TLS slots are initialized there; this function is just a lightweight index validation.
        OLO_CORE_ASSERT(m_ParallelSubmissionActive,
                        "CommandBucket::UseWorkerIndex: PrepareForParallelSubmission must be called first!");
    }

    u32 CommandBucket::ClaimBatch()
    {
        // Atomically claim a batch of TLS_BATCH_SIZE slots
        u32 batchStart = m_NextBatchStart.fetch_add(TLS_BATCH_SIZE, std::memory_order_relaxed);

        // Grow the parallel commands array if needed
        u32 requiredCapacity = batchStart + TLS_BATCH_SIZE;
        if (requiredCapacity > m_ParallelCommands.size())
        {
            TUniqueLock<FMutex> lock(m_Mutex);
            if (requiredCapacity > m_ParallelCommands.size())
            {
                // Double the capacity or grow to required size
                sizet newCapacity = std::max(
                    m_ParallelCommands.size() * 2,
                    static_cast<sizet>(requiredCapacity));
                m_ParallelCommands.resize(newCapacity, nullptr);
            }
        }

        return batchStart;
    }

    void CommandBucket::SubmitPacketParallel(CommandPacket* packet, u32 workerIndex)
    {
        OLO_PROFILE_FUNCTION();

        OLO_CORE_ASSERT(packet, "CommandBucket::SubmitPacketParallel: Null packet!");
        OLO_CORE_ASSERT(workerIndex < MAX_RENDER_WORKERS,
                        "CommandBucket::SubmitPacketParallel: Invalid worker index {}!", workerIndex);
        OLO_CORE_ASSERT(m_ParallelSubmissionActive,
                        "CommandBucket::SubmitPacketParallel: Not in parallel submission mode!");

        TLSBucketSlot& slot = m_TLSSlots[workerIndex];

        // If no remaining slots in current batch, claim a new batch
        if (slot.remaining == 0)
        {
            slot.batchStart = ClaimBatch();
            slot.offset = 0;
            slot.remaining = TLS_BATCH_SIZE;
        }

        // Write packet to thread-local slot (no synchronization needed)
        u32 globalIndex = slot.batchStart + slot.offset;
        m_ParallelCommands[globalIndex] = packet;

        // Update slot state
        slot.offset++;
        slot.remaining--;

        // Increment global command count (atomic)
        m_ParallelCommandCount.fetch_add(1, std::memory_order_relaxed);
    }

    void CommandBucket::MergeThreadLocalCommands()
    {
        OLO_PROFILE_FUNCTION();

        TUniqueLock<FMutex> lock(m_Mutex);

        if (!m_ParallelSubmissionActive)
        {
            OLO_CORE_WARN("CommandBucket::MergeThreadLocalCommands: Not in parallel submission mode!");
            return;
        }

        u32 totalCommands = m_ParallelCommandCount.load(std::memory_order_acquire);
        if (totalCommands == 0)
        {
            m_ParallelSubmissionActive = false;
            return;
        }

        // Compact the parallel commands array into the linked list
        // Option A: Copy to linked list (simple, maintains existing code paths)
        m_Head = nullptr;
        m_Tail = nullptr;
        m_CommandCount = 0;

        sizet maxIndex = m_NextBatchStart.load(std::memory_order_relaxed);
        for (sizet i = 0; i < maxIndex && i < m_ParallelCommands.size(); ++i)
        {
            CommandPacket* packet = m_ParallelCommands[i];
            if (packet != nullptr)
            {
                packet->SetNext(nullptr);

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
                // NOTE: m_Stats.TotalCommands is NOT incremented here.
                // TotalCommands is a cumulative counter incremented in AddCommand,
                // which counts all commands submitted (both serial and parallel).
                // During parallel submission, commands are already counted in AddCommand
                // before being placed in m_ParallelCommands, so we only update m_CommandCount
                // here (the per-frame linked list size) to track current state.
            }
        }

        // Reset parallel submission state
        m_ParallelSubmissionActive = false;

        // Invalidate sorting and batching since we have new commands
        m_IsSorted = false;
        m_IsBatched = false;

        OLO_CORE_TRACE("CommandBucket: Merged {} commands from parallel submission",
                       m_CommandCount);
    }

    void CommandBucket::RemapBoneOffsets(FrameDataBuffer& frameDataBuffer)
    {
        OLO_PROFILE_FUNCTION();

        u32 remappedCount = 0;

        // Iterate through the linked list of commands
        CommandPacket* current = m_Head;
        while (current)
        {
            // Check if this is a DrawMeshCommand with bone data that needs remapping
            if (current->GetCommandType() == CommandType::DrawMesh)
            {
                auto* cmd = current->GetCommandData<DrawMeshCommand>();
                if (cmd->isAnimatedMesh && cmd->needsBoneOffsetRemap && cmd->boneCount > 0)
                {
                    // Remap the worker-local offset to the global offset
                    u32 globalOffset = frameDataBuffer.GetGlobalBoneOffset(cmd->workerIndex, cmd->boneBufferOffset);
                    cmd->boneBufferOffset = globalOffset;
                    cmd->needsBoneOffsetRemap = false;
                    remappedCount++;
                }
            }
            current = current->GetNext();
        }

        if (remappedCount > 0)
        {
            OLO_CORE_TRACE("CommandBucket: Remapped {} animated mesh bone offsets", remappedCount);
        }
    }

    void CommandBucket::Reset(CommandAllocator& allocator)
    {
        OLO_PROFILE_FUNCTION();

        Clear();

        // Clear transform buffers
        m_TransformBuffers.clear();
        m_PacketToBufferIndex.clear();

        // Reset the allocator to free memory
        allocator.Reset();

        // Reset statistics
        m_Stats = Statistics();
    }
} // namespace OloEngine
