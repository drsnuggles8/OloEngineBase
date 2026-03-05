#include "OloEnginePCH.h"
#include "CommandBucket.h"
#include "FrameDataBuffer.h"
#include "RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/Debug/GPUTimerQueryPool.h"
#include "OloEngine/Task/ParallelFor.h"
#include "OloEngine/Threading/UniqueLock.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <unordered_set>

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

        void RadixSort64(std::vector<u64>& keys, std::vector<CommandPacket*>& packets)
        {
            OLO_PROFILE_FUNCTION();

            if (keys.size() <= 1)
                return;

            sizet count = keys.size();

            // Temporary buffers for swapping
            std::vector<CommandPacket*> tempPackets(count);
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
                    tempPackets[destIdx] = packets[i];
                    tempKeys[destIdx] = keys[i];
                }

                // Swap buffers
                std::swap(packets, tempPackets);
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
        constexpr i32 PARALLEL_SORT_BATCH_SIZE = 256;   // Elements per chunk for parallel radix sort

        void ParallelRadixSort64(std::vector<u64>& keys, std::vector<CommandPacket*>& packets)
        {
            OLO_PROFILE_FUNCTION();

            if (keys.size() <= 1)
                return;

            sizet count = keys.size();

            // Fall back to single-threaded for small arrays
            if (count < PARALLEL_SORT_THRESHOLD)
            {
                RadixSort64(keys, packets);
                return;
            }

            // Divide input into deterministic fixed-size chunks.
            // Each chunk has a fixed range of input elements, enabling
            // per-chunk histograms and parallel scatter with no conflicts.
            i32 numChunks = static_cast<i32>((count + PARALLEL_SORT_BATCH_SIZE - 1) / PARALLEL_SORT_BATCH_SIZE);

            // Temporary buffers for swapping
            std::vector<CommandPacket*> tempPackets(count);
            std::vector<u64> tempKeys(count);

            // Per-chunk histograms (indexed by chunk ID, deterministic)
            std::vector<std::array<sizet, RADIX_SIZE>> chunkHistograms(numChunks);

            // Per-chunk scatter offsets
            std::vector<std::array<sizet, RADIX_SIZE>> chunkOffsets(numChunks);

            // Process each byte from LSB to MSB
            for (sizet pass = 0; pass < NUM_PASSES; ++pass)
            {
                OLO_PROFILE_SCOPE("RadixPass");
                sizet shift = pass * RADIX_BITS;

                // Phase 1: Build per-chunk histograms in parallel.
                // Each chunk is a deterministic range of input elements.
                {
                    OLO_PROFILE_SCOPE("ParallelHistogram");
                    ParallelFor(
                        "RadixHistogram",
                        numChunks,
                        1,
                        [&keys, &chunkHistograms, shift, count](i32 c)
                        {
                            auto& hist = chunkHistograms[c];
                            hist = {};
                            sizet start = static_cast<sizet>(c) * PARALLEL_SORT_BATCH_SIZE;
                            sizet end = std::min(start + PARALLEL_SORT_BATCH_SIZE, count);
                            for (sizet i = start; i < end; ++i)
                            {
                                u8 digit = static_cast<u8>((keys[i] >> shift) & 0xFF);
                                hist[digit]++;
                            }
                        });
                }

                // Phase 2: Compute per-chunk scatter offsets.
                // chunkOffsets[c][d] = globalPrefixSum[d] + sum of chunkHistograms[0..c-1][d]
                // Each chunk writes to non-overlapping destination ranges per digit.
                {
                    OLO_PROFILE_SCOPE("ChunkOffsets");
                    // Reduce into global histogram and compute prefix sums
                    std::array<sizet, RADIX_SIZE> globalHistogram{};
                    for (i32 c = 0; c < numChunks; ++c)
                    {
                        for (sizet d = 0; d < RADIX_SIZE; ++d)
                        {
                            globalHistogram[d] += chunkHistograms[c][d];
                        }
                    }

                    std::array<sizet, RADIX_SIZE> prefixSum{};
                    sizet total = 0;
                    for (sizet d = 0; d < RADIX_SIZE; ++d)
                    {
                        prefixSum[d] = total;
                        total += globalHistogram[d];
                    }

                    // Compute per-chunk offsets
                    for (sizet d = 0; d < RADIX_SIZE; ++d)
                    {
                        sizet running = prefixSum[d];
                        for (i32 c = 0; c < numChunks; ++c)
                        {
                            chunkOffsets[c][d] = running;
                            running += chunkHistograms[c][d];
                        }
                    }
                }

                // Phase 3: Parallel scatter.
                // Each chunk scatters its deterministic range of elements using
                // its own offset table. Writes are non-overlapping across chunks.
                {
                    OLO_PROFILE_SCOPE("ParallelScatter");
                    ParallelFor(
                        "RadixScatter",
                        numChunks,
                        1,
                        [&keys, &packets, &tempKeys, &tempPackets, &chunkOffsets, shift, count](i32 c)
                        {
                            auto offsets = chunkOffsets[c]; // Local copy to avoid false sharing
                            sizet start = static_cast<sizet>(c) * PARALLEL_SORT_BATCH_SIZE;
                            sizet end = std::min(start + PARALLEL_SORT_BATCH_SIZE, count);
                            for (sizet i = start; i < end; ++i)
                            {
                                u8 digit = static_cast<u8>((keys[i] >> shift) & 0xFF);
                                sizet destIdx = offsets[digit]++;
                                tempPackets[destIdx] = packets[i];
                                tempKeys[destIdx] = keys[i];
                            }
                        });
                }

                // Swap buffers
                std::swap(packets, tempPackets);
                std::swap(keys, tempKeys);
            }
        }

    } // anonymous namespace

    void CommandBucket::SetViewStateCallbacks(ViewStateReadFn readFn, ViewStateWriteFn writeFn)
    {
        s_ViewStateReader = readFn;
        s_ViewStateWriter = writeFn;
    }

    CommandBucket::CommandBucket(const CommandBucketConfig& config)
        : m_Config(config)
    {
        // Initialize statistics
        m_Stats = Statistics();

        // Initialize thread-local slots
        std::memset(m_TLSSlots, 0, sizeof(m_TLSSlots));

        // Pre-allocate flat arrays and parallel command array
        m_Keys.reserve(config.InitialCapacity);
        m_Packets.reserve(config.InitialCapacity);
        m_ParallelCommands.reserve(config.InitialCapacity);
    }

    CommandBucket::~CommandBucket()
    {
        // The actual command memory is managed by CommandAllocator
        // We just need to clear our references
        Clear();
    }

    CommandBucket::CommandBucket(CommandBucket&& other) noexcept
        : m_Keys(std::move(other.m_Keys)),
          m_Packets(std::move(other.m_Packets)),
          m_CommandCount(other.m_CommandCount),
          m_Config(other.m_Config),
          m_IsSorted(other.m_IsSorted),
          m_IsBatched(other.m_IsBatched),
          m_Stats(other.m_Stats)
    {
        other.m_CommandCount = 0;
        other.m_IsSorted = false;
        other.m_IsBatched = false;
        other.m_Stats = Statistics();
    }

    CommandBucket& CommandBucket::operator=(CommandBucket&& other) noexcept
    {
        if (this != &other)
        {
            m_Keys = std::move(other.m_Keys);
            m_Packets = std::move(other.m_Packets);
            m_CommandCount = other.m_CommandCount;
            m_Config = other.m_Config;
            m_IsSorted = other.m_IsSorted;
            m_IsBatched = other.m_IsBatched;
            m_Stats = other.m_Stats;

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

        TUniqueLock<FMutex> lock(m_Mutex);

        m_Keys.push_back(packet->GetMetadata().m_SortKey.GetKey());
        m_Packets.push_back(packet);

        m_CommandCount++;
        m_Stats.TotalCommands++;

        // Adding a new command invalidates sorting and batching
        m_IsSorted = false;
        m_IsBatched = false;
    }

    void CommandBucket::SortCommands()
    {
        OLO_PROFILE_FUNCTION();

        TUniqueLock<FMutex> lock(m_Mutex);
        SortCommandsInternal();
    }

    void CommandBucket::SortCommandsInternal()
    {
        // Internal sort implementation — caller must hold m_Mutex
        OLO_PROFILE_FUNCTION();

        if (!m_Config.EnableSorting || m_IsSorted)
        {
            m_LastSortTimeMs = 0.0;
            return;
        }

        if (m_CommandCount <= 1)
        {
            m_LastSortTimeMs = 0.0;
            m_IsSorted = true;
            return;
        }

        auto sortStart = std::chrono::high_resolution_clock::now();

        // Check if any commands have dependency ordering
        bool hasDependencies = false;
        for (sizet i = 0; i < m_Packets.size(); ++i)
        {
            if (m_Packets[i]->GetMetadata().m_DependsOnPrevious)
            {
                hasDependencies = true;
                break;
            }
        }

        if (!hasDependencies)
        {
            // Fast path: no dependencies, sort the flat key+packet arrays directly
            ParallelRadixSort64(m_Keys, m_Packets);
        }
        else
        {
            // Dependency-aware path: split into groups, sort each group
            std::vector<std::pair<sizet, sizet>> groupRanges; // [start, end) ranges
            sizet groupStart = 0;
            for (sizet i = 1; i < m_Packets.size(); ++i)
            {
                if (m_Packets[i]->GetMetadata().m_DependsOnPrevious)
                {
                    groupRanges.push_back({ groupStart, i });
                    groupStart = i;
                }
            }
            groupRanges.push_back({ groupStart, m_Packets.size() });

            // Sort each group independently
            for (auto& [start, end] : groupRanges)
            {
                sizet groupSize = end - start;
                if (groupSize <= 1)
                    continue;

                // Create temporary sub-arrays for the group
                std::vector<u64> groupKeys(m_Keys.begin() + start, m_Keys.begin() + end);
                std::vector<CommandPacket*> groupPackets(m_Packets.begin() + start, m_Packets.begin() + end);

                ParallelRadixSort64(groupKeys, groupPackets);

                // Copy sorted results back
                std::copy(groupKeys.begin(), groupKeys.end(), m_Keys.begin() + start);
                std::copy(groupPackets.begin(), groupPackets.end(), m_Packets.begin() + start);
            }
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
        instancedCmd->materialDataIndex = meshCmd->materialDataIndex;

        // Copy shader handle
        instancedCmd->shaderHandle = meshCmd->shaderHandle;

        // Copy POD render state index
        instancedCmd->renderStateIndex = meshCmd->renderStateIndex;

        // Copy animation fields
        instancedCmd->isAnimatedMesh = meshCmd->isAnimatedMesh;
        instancedCmd->boneBufferOffset = meshCmd->boneBufferOffset;
        instancedCmd->boneCountPerInstance = meshCmd->boneCount;

        // Set command type and dispatch function (via runtime resolver)
        instancedPacket->SetCommandType(instancedCmd->header.type);

        return instancedPacket;
    }

    bool CommandBucket::TryMergeCommands(sizet targetIdx, sizet sourceIdx, CommandAllocator& allocator)
    {
        OLO_PROFILE_FUNCTION();

        // Caller must hold m_Mutex
        CommandPacket* target = m_Packets[targetIdx];
        CommandPacket* source = m_Packets[sourceIdx];

        if (!target || !source || !target->CanBatchWith(*source))
            return false;

        // Handle batching based on command type
        CommandType targetType = target->GetCommandType();

        if (CommandType sourceType = source->GetCommandType();
            targetType == CommandType::DrawMesh && sourceType == CommandType::DrawMesh)
        {
            // Convert to instanced command
            CommandPacket* instancedPacket = ConvertToInstanced(target, allocator);
            if (!instancedPacket)
                return false;

            // Replace target in the flat array
            m_Packets[targetIdx] = instancedPacket;
            m_Keys[targetIdx] = instancedPacket->GetMetadata().m_SortKey.GetKey();

            // Dynamic instance batching: merge transforms from source
            auto const* sourceMeshCmd = source->GetCommandData<DrawMeshCommand>();
            if (!sourceMeshCmd)
                return false;

            auto* instancedCmd = instancedPacket->GetCommandData<DrawMeshInstancedCommand>();
            if (!instancedCmd)
                return false;

            // Allocate new contiguous space for both transforms
            FrameDataBuffer& frameBuffer = FrameDataBufferManager::Get();
            u32 totalTransforms = instancedCmd->transformCount + 1;

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

            const glm::mat4* existingTransforms = frameBuffer.GetTransformPtr(instancedCmd->transformBufferOffset);
            if (existingTransforms)
            {
                frameBuffer.WriteTransforms(newOffset, existingTransforms, instancedCmd->transformCount);
            }

            frameBuffer.WriteTransforms(newOffset + instancedCmd->transformCount, &sourceMeshCmd->transform, 1);

            instancedCmd->transformBufferOffset = newOffset;
            instancedCmd->transformCount = totalTransforms;
            instancedCmd->instanceCount = totalTransforms;

            // Mark the source as merged (will be compacted later)
            m_Packets[sourceIdx] = nullptr;

            return true;
        }
        else if (targetType == CommandType::DrawMeshInstanced && sourceType == CommandType::DrawMesh)
        {
            auto* instancedCmd = target->GetCommandData<DrawMeshInstancedCommand>();
            auto const* sourceMeshCmd = source->GetCommandData<DrawMeshCommand>();

            if (!instancedCmd || !sourceMeshCmd)
                return false;

            u32 totalTransforms = instancedCmd->transformCount + 1;
            if (totalTransforms > m_Config.MaxMeshInstances)
            {
                OLO_CORE_WARN("CommandBucket::TryMergeCommands: Max instances ({}) reached", m_Config.MaxMeshInstances);
                return false;
            }

            FrameDataBuffer& frameBuffer = FrameDataBufferManager::Get();
            u32 newOffset = frameBuffer.AllocateTransforms(totalTransforms);
            if (newOffset == UINT32_MAX)
            {
                OLO_CORE_ERROR("CommandBucket::TryMergeCommands: Failed to allocate {} transforms in FrameDataBuffer", totalTransforms);
                return false;
            }

            const glm::mat4* existingTransforms = frameBuffer.GetTransformPtr(instancedCmd->transformBufferOffset);
            if (existingTransforms)
            {
                frameBuffer.WriteTransforms(newOffset, existingTransforms, instancedCmd->transformCount);
            }

            frameBuffer.WriteTransforms(newOffset + instancedCmd->transformCount, &sourceMeshCmd->transform, 1);

            instancedCmd->transformBufferOffset = newOffset;
            instancedCmd->transformCount = totalTransforms;
            instancedCmd->instanceCount = totalTransforms;

            // Mark the source as merged
            m_Packets[sourceIdx] = nullptr;

            return true;
        }

        return false;
    }

    void CommandBucket::BatchCommands(CommandAllocator& allocator)
    {
        OLO_PROFILE_FUNCTION();

        TUniqueLock<FMutex> lock(m_Mutex);

        if (!m_Config.EnableBatching || m_IsBatched || m_CommandCount <= 1)
        {
            m_LastBatchTimeMs = 0.0;
            return;
        }

        auto batchStart = std::chrono::high_resolution_clock::now();

        // ── Phase 1: Build instance groups via hash table ──────────────
        // O(n) scan — groups ALL matching DrawMesh commands, not just adjacent.
        std::unordered_map<InstanceGroupKey, std::vector<sizet>, InstanceGroupKeyHash> groups;

        // Collect predecessors of dependency-constrained packets so they are
        // not merged/nulled during instancing — their dependent follower
        // relies on them staying in place.
        std::unordered_set<sizet> protectedPredecessors;
        for (sizet i = 1; i < m_Packets.size(); ++i)
        {
            if (m_Packets[i] && m_Packets[i]->GetMetadata().m_DependsOnPrevious)
            {
                protectedPredecessors.insert(i - 1);
            }
        }

        for (sizet i = 0; i < m_Packets.size(); ++i)
        {
            if (!m_Packets[i])
                continue;
            if (m_Packets[i]->GetCommandType() != CommandType::DrawMesh)
                continue;
            // Commands with ordering constraints must not be reordered into groups
            if (m_Packets[i]->GetMetadata().m_DependsOnPrevious)
                continue;
            // Predecessors of constrained packets must not be merged/nulled
            if (protectedPredecessors.count(i))
                continue;

            auto const* cmd = m_Packets[i]->GetCommandData<DrawMeshCommand>();
            // Skip animated/skinned meshes — they have per-instance bone data
            if (cmd->isAnimatedMesh)
                continue;
            InstanceGroupKey key{ cmd->meshHandle, cmd->materialDataIndex, cmd->renderStateIndex };
            groups[key].push_back(i);
        }

        // ── Phase 2: Merge groups with count > 1 ──────────────────────
        FrameDataBuffer& frameBuffer = FrameDataBufferManager::Get();

        for (auto& [key, indices] : groups)
        {
            if (indices.size() <= 1)
                continue;

            u32 totalInstances = static_cast<u32>(
                std::min(indices.size(), static_cast<sizet>(m_Config.MaxMeshInstances)));

            // Allocate contiguous transform block for all instances at once
            u32 transformOffset = frameBuffer.AllocateTransforms(totalInstances);
            if (transformOffset == UINT32_MAX)
            {
                OLO_CORE_ERROR("CommandBucket::BatchCommands: Failed to allocate {} transforms in FrameDataBuffer", totalInstances);
                continue;
            }

            // Write all transforms contiguously
            for (u32 t = 0; t < totalInstances; ++t)
            {
                auto const* meshCmd = m_Packets[indices[t]]->GetCommandData<DrawMeshCommand>();
                frameBuffer.WriteTransforms(transformOffset + t, &meshCmd->transform, 1);
            }

            // Build the instanced command from the first DrawMeshCommand
            sizet firstIdx = indices[0];
            auto const* firstCmd = m_Packets[firstIdx]->GetCommandData<DrawMeshCommand>();
            PacketMetadata metadata = m_Packets[firstIdx]->GetMetadata();

            CommandPacket* instancedPacket = allocator.AllocatePacketWithCommand<DrawMeshInstancedCommand>(metadata);
            if (!instancedPacket)
                continue;

            auto* icmd = instancedPacket->GetCommandData<DrawMeshInstancedCommand>();
            icmd->header.type = CommandType::DrawMeshInstanced;
            icmd->header.dispatchFn = nullptr;
            icmd->meshHandle = firstCmd->meshHandle;
            icmd->vertexArrayID = firstCmd->vertexArrayID;
            icmd->indexCount = firstCmd->indexCount;
            icmd->instanceCount = totalInstances;
            icmd->transformBufferOffset = transformOffset;
            icmd->transformCount = totalInstances;
            icmd->shaderHandle = firstCmd->shaderHandle;
            icmd->materialDataIndex = firstCmd->materialDataIndex;
            icmd->renderStateIndex = firstCmd->renderStateIndex;
            icmd->isAnimatedMesh = firstCmd->isAnimatedMesh;
            icmd->boneBufferOffset = firstCmd->boneBufferOffset;
            icmd->boneCountPerInstance = firstCmd->boneCount;

            instancedPacket->SetCommandType(icmd->header.type);

            // Replace first packet with the instanced command, null out the rest
            m_Packets[firstIdx] = instancedPacket;
            m_Keys[firstIdx] = instancedPacket->GetMetadata().m_SortKey.GetKey();

            for (u32 t = 1; t < totalInstances; ++t)
            {
                m_Packets[indices[t]] = nullptr;
                m_Stats.BatchedCommands++;
            }
        }

        // ── Phase 3: Compact — remove null entries ────────────────────
        sizet write = 0;
        for (sizet read = 0; read < m_Packets.size(); ++read)
        {
            if (m_Packets[read] != nullptr)
            {
                if (write != read)
                {
                    m_Packets[write] = m_Packets[read];
                    m_Keys[write] = m_Keys[read];
                }
                ++write;
            }
        }
        m_Packets.resize(write);
        m_Keys.resize(write);
        m_CommandCount = write;

        m_IsBatched = true;

        // ── Phase 4: Sort for optimal execution order ─────────────────
        // Batching invalidated sort order (packets were removed/replaced).
        // Always re-sort to minimize state changes during execution.
        m_IsSorted = false;
        SortCommandsInternal();

        auto batchEnd = std::chrono::high_resolution_clock::now();
        m_LastBatchTimeMs = std::chrono::duration<f64, std::milli>(batchEnd - batchStart).count();
    }

    void CommandBucket::Execute(RendererAPI& rendererAPI)
    {
        OLO_PROFILE_FUNCTION();

        auto execStart = std::chrono::high_resolution_clock::now();

        // No lock needed — Execute runs exclusively on the main thread
        // during EndScene, after all submission is complete.
        m_Stats.DrawCalls = 0;
        m_Stats.StateChanges = 0;

        // Bind per-bucket view state if set, saving the previous state for restoration
        BucketViewState savedState;
        bool restoreState = false;
        if (m_ViewState.has_value() && s_ViewStateReader && s_ViewStateWriter)
        {
            s_ViewStateReader(savedState);
            restoreState = true;
            s_ViewStateWriter(*m_ViewState);
        }

        // Execute all commands in order from the flat array
        for (const auto* packet : m_Packets)
        {
            if (!packet)
                continue;

            if (CommandType type = packet->GetCommandType();
                type == CommandType::DrawMesh ||
                type == CommandType::DrawMeshInstanced ||
                type == CommandType::DrawQuad ||
                type == CommandType::DrawDecal ||
                type == CommandType::DrawFoliageLayer ||
                type == CommandType::DrawTerrainPatch ||
                type == CommandType::DrawVoxelMesh ||
                type == CommandType::DrawSkybox ||
                type == CommandType::DrawInfiniteGrid ||
                type == CommandType::DrawArrays ||
                type == CommandType::DrawIndexed ||
                type == CommandType::DrawIndexedInstanced ||
                type == CommandType::DrawLines)
            {
                m_Stats.DrawCalls++;
            }
            else if (type != CommandType::Invalid)
            {
                m_Stats.StateChanges++;
            }

            packet->Execute(rendererAPI);
        }

        // Restore previous view state if we changed it
        if (restoreState)
        {
            s_ViewStateWriter(savedState);
        }

        auto execEnd = std::chrono::high_resolution_clock::now();
        m_LastExecuteTimeMs = std::chrono::duration<f64, std::milli>(execEnd - execStart).count();
    }

    void CommandBucket::ExecuteWithGPUTiming(RendererAPI& rendererAPI)
    {
        OLO_PROFILE_FUNCTION();

        auto& gpuTimer = GPUTimerQueryPool::GetInstance();

        if (!gpuTimer.IsInitialized())
            gpuTimer.Initialize();

        gpuTimer.BeginFrame();

        auto execStart = std::chrono::high_resolution_clock::now();

        // No lock needed — ExecuteWithGPUTiming runs exclusively on the main thread
        m_Stats.DrawCalls = 0;
        m_Stats.StateChanges = 0;

        // Bind per-bucket view state if set
        BucketViewState savedState;
        bool restoreState = false;
        if (m_ViewState.has_value() && s_ViewStateReader && s_ViewStateWriter)
        {
            s_ViewStateReader(savedState);
            restoreState = true;
            s_ViewStateWriter(*m_ViewState);
        }

        u32 cmdIndex = 0;
        for (const auto* packet : m_Packets)
        {
            if (!packet)
                continue;

            if (CommandType type = packet->GetCommandType();
                type == CommandType::DrawMesh ||
                type == CommandType::DrawMeshInstanced ||
                type == CommandType::DrawQuad ||
                type == CommandType::DrawDecal ||
                type == CommandType::DrawFoliageLayer ||
                type == CommandType::DrawTerrainPatch ||
                type == CommandType::DrawVoxelMesh ||
                type == CommandType::DrawSkybox ||
                type == CommandType::DrawInfiniteGrid ||
                type == CommandType::DrawArrays ||
                type == CommandType::DrawIndexed ||
                type == CommandType::DrawIndexedInstanced ||
                type == CommandType::DrawLines)
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
                packet->Execute(rendererAPI);
                gpuTimer.EndQuery(cmdIndex);
            }
            else
            {
                packet->Execute(rendererAPI);
            }

            cmdIndex++;
        }

        // Restore previous view state if we changed it
        if (restoreState)
        {
            s_ViewStateWriter(savedState);
        }

        gpuTimer.EndFrame();

        auto execEnd = std::chrono::high_resolution_clock::now();
        m_LastExecuteTimeMs = std::chrono::duration<f64, std::milli>(execEnd - execStart).count();
    }

    void CommandBucket::Clear()
    {
        m_Keys.clear();
        m_Packets.clear();
        m_CommandCount = 0;

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

        // Compact the parallel commands array into the flat arrays
        m_Keys.clear();
        m_Packets.clear();
        m_CommandCount = 0;

        sizet maxIndex = m_NextBatchStart.load(std::memory_order_relaxed);
        for (sizet i = 0; i < maxIndex && i < m_ParallelCommands.size(); ++i)
        {
            CommandPacket* packet = m_ParallelCommands[i];
            if (packet != nullptr)
            {
                m_Keys.push_back(packet->GetMetadata().m_SortKey.GetKey());
                m_Packets.push_back(packet);
                m_CommandCount++;
            }
        }

        // Reset parallel submission state
        m_ParallelSubmissionActive = false;

        // Invalidate sorting and batching since we have new commands
        m_IsSorted = false;
        m_IsBatched = false;
    }

    void CommandBucket::RemapBoneOffsets(FrameDataBuffer& frameDataBuffer)
    {
        OLO_PROFILE_FUNCTION();

        u32 remappedCount = 0;

        // Iterate through the flat array of commands
        for (auto* packet : m_Packets)
        {
            if (!packet)
                continue;

            if (packet->GetCommandType() == CommandType::DrawMesh)
            {
                auto* cmd = packet->GetCommandData<DrawMeshCommand>();
                if (cmd->isAnimatedMesh && cmd->needsBoneOffsetRemap && cmd->boneCount > 0)
                {
                    u32 globalOffset = frameDataBuffer.GetGlobalBoneOffset(cmd->workerIndex, cmd->boneBufferOffset);
                    cmd->boneBufferOffset = globalOffset;
                    cmd->needsBoneOffsetRemap = false;
                    remappedCount++;
                }
            }
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
