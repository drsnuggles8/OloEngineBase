#include "OloEnginePCH.h"
#include "CommandBucket.h"
#include "FrameDataBuffer.h"
#include "RenderCommand.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneDrawLink.h"
#include "OloEngine/Renderer/Debug/GPUTimerQueryPool.h"
#include "OloEngine/Task/ParallelFor.h"
#include "OloEngine/Threading/UniqueLock.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <unordered_set>

namespace OloEngine
{
    // ── Skinned auto-batching (issue #1031) ──────────────────────────────
    //
    // A batched draw uploads exactly ONE bone palette for all N instances
    // (CommandDispatch::UploadBoneMatrices), so skinned draws may only group
    // when their palettes are identical. That is the whole reason animated
    // draws were excluded from batching until now.
    //
    // Identity is decided on CONTENT, not on an authored archetype id: two
    // actors playing the same clip at the same phase produce byte-identical
    // palettes without anyone declaring that they should, and actors that drift
    // apart stop sharing the moment they do. Both the current and the previous
    // pose must match — sharing on the current pose alone would give the whole
    // group one actor's skeletal velocity, which reads as ghosting under TAA
    // rather than as a batching bug.
    //
    // Hash first, then memcmp. The hash only chooses who is worth comparing;
    // the memcmp decides. A hash used as the decision would render one actor in
    // another's pose on a collision, and there is no test that notices that.
    namespace
    {
        namespace SkinnedBatching
        {
            // The palette pair a single skinned draw would upload, resolved to
            // pointers into this frame's FrameDataBuffer.
            struct PalettePair
            {
                sizet m_PacketIndex = 0;
                const glm::mat4* m_Current = nullptr;
                const glm::mat4* m_Prev = nullptr;
                u32 m_BoneCount = 0;
            };

            [[nodiscard]] u64 HashPalettes(const PalettePair& pair)
            {
                // FNV-1a over both palettes, seeded with the bone count so two
                // poses that share a prefix but differ in length never collide.
                constexpr u64 kOffsetBasis = 14695981039346656037ull;
                constexpr u64 kPrime = 1099511628211ull;

                // Consumed a WORD at a time rather than a byte: a palette is
                // 6.4 KB at the 100-bone cap and a crowd hashes two of them per
                // actor per frame, so the eightfold difference is the difference
                // between a rounding error and a visible cost. A mat4 is 16 floats,
                // so the byte count is always a multiple of 8 and there is no tail.
                static_assert(sizeof(glm::mat4) % sizeof(u64) == 0, "bone palette is not word-sized");

                u64 hash = kOffsetBasis ^ static_cast<u64>(pair.m_BoneCount);
                const sizet words = static_cast<sizet>(pair.m_BoneCount) * (sizeof(glm::mat4) / sizeof(u64));
                for (const glm::mat4* palette : { pair.m_Current, pair.m_Prev })
                {
                    for (sizet w = 0; w < words; ++w)
                    {
                        u64 word = 0;
                        std::memcpy(&word, reinterpret_cast<const u8*>(palette) + w * sizeof(u64), sizeof(u64));
                        hash ^= word;
                        hash *= kPrime;
                    }
                }
                return hash;
            }

            [[nodiscard]] bool SamePose(const PalettePair& lhs, const PalettePair& rhs)
            {
                if (lhs.m_BoneCount != rhs.m_BoneCount)
                    return false;
                const sizet bytes = static_cast<sizet>(lhs.m_BoneCount) * sizeof(glm::mat4);
                return std::memcmp(lhs.m_Current, rhs.m_Current, bytes) == 0 &&
                       std::memcmp(lhs.m_Prev, rhs.m_Prev, bytes) == 0;
            }
        } // namespace SkinnedBatching
    } // namespace

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
                    ++histogram[digit];
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
                                ++hist[digit];
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

    CommandBucketConfig CommandBucket::ValidateConfig(const CommandBucketConfig& config)
    {
        CommandBucketConfig validated = config;
        if (validated.MaxMeshInstances == 0)
        {
            OLO_CORE_WARN("CommandBucket: MaxMeshInstances of 0 cannot batch anything; using 1.");
            validated.MaxMeshInstances = 1;
        }
        else if (validated.MaxMeshInstances > CommandBucketConfig::kMaxDispatchableMeshInstances)
        {
            // Not a preference: CommandDispatch::DrawMeshInstanced truncates to
            // this and never splits, so the excess draws would be collapsed here
            // and then dropped there -- objects missing from the frame, with
            // only a per-draw warning at the far end to say so.
            OLO_CORE_WARN("CommandBucket: MaxMeshInstances {} exceeds what the dispatcher can draw ({}); "
                          "clamping, or the surplus instances would be batched here and dropped at dispatch.",
                          validated.MaxMeshInstances, CommandBucketConfig::kMaxDispatchableMeshInstances);
            validated.MaxMeshInstances = CommandBucketConfig::kMaxDispatchableMeshInstances;
        }
        return validated;
    }

    CommandBucket::CommandBucket(const CommandBucketConfig& config)
        : m_Config(ValidateConfig(config))
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

        ++m_CommandCount;
        ++m_Stats.TotalCommands;

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
            for (const auto& [start, end] : groupRanges)
            {
                if (sizet groupSize = end - start; groupSize <= 1)
                    continue;

                // Create temporary sub-arrays for the group
                std::vector<u64> groupKeys(m_Keys.begin() + start, m_Keys.begin() + end);
                std::vector<CommandPacket*> groupPackets(m_Packets.begin() + start, m_Packets.begin() + end);

                ParallelRadixSort64(groupKeys, groupPackets);

                // Copy sorted results back
                std::ranges::copy(groupKeys, m_Keys.begin() + start);
                std::ranges::copy(groupPackets, m_Packets.begin() + start);
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
        instancedCmd->baseIndex = meshCmd->baseIndex; // submesh index range, was dropped (drew from 0)

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
        instancedCmd->prevBoneBufferOffset = meshCmd->prevBoneBufferOffset;

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

            if (const glm::mat4* existingTransforms = frameBuffer.GetTransformPtr(instancedCmd->transformBufferOffset))
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

            if (const glm::mat4* existingTransforms = frameBuffer.GetTransformPtr(instancedCmd->transformBufferOffset))
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

        // Skinned draws are collected separately and partitioned by pose
        // (issue #1031) before they join `groups` — see PartitionSkinnedGroups.
        const u32 unremappedBefore = m_Stats.SkinnedBatchUnremapped;
        std::unordered_map<InstanceGroupKey, std::vector<sizet>, InstanceGroupKeyHash> skinnedCandidates;

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
            InstanceGroupKey key{ cmd->vertexArrayID, cmd->indexCount, cmd->baseIndex,
                                  cmd->materialDataIndex, cmd->renderStateIndex, 0 };
            if (cmd->isAnimatedMesh)
            {
                // Skinned draws carry a bone palette each and the batched draw
                // uploads one. Park them here; PartitionSkinnedGroups below
                // splits them by pose and feeds the same-pose subsets back into
                // `groups`. Parking rather than grouping directly is what keeps
                // a single-character scene from ever hashing a palette.
                if (cmd->boneCount == 0)
                {
                    // UploadBoneMatrices no-ops on a zero count, so a batched
                    // draw would render against whatever palette the previous
                    // draw left bound. Leave it as its own DrawMesh.
                    continue;
                }
                if (cmd->needsBoneOffsetRemap)
                {
                    // The offset is still worker-local, so GetBoneMatrixPtr
                    // would read some other worker's palette. RemapBoneOffsets
                    // runs in EndParallelSubmission, before any pass executes,
                    // so this is a plumbing error rather than a pose result —
                    // counted, and reported once per frame below.
                    ++m_Stats.SkinnedBatchUnremapped;
                    continue;
                }
                skinnedCandidates[key].push_back(i);
                continue;
            }
            groups[key].push_back(i);
        }

        PartitionSkinnedGroups(skinnedCandidates, groups);

        // The counter is cumulative for the bucket's lifetime, like
        // BatchedCommands; warn on what THIS pass found so a scene that hits it
        // once does not log every frame afterwards.
        if (const u32 unremappedThisPass = m_Stats.SkinnedBatchUnremapped - unremappedBefore; unremappedThisPass > 0)
        {
            OLO_CORE_WARN("CommandBucket::BatchCommands: {} skinned draw(s) still carried worker-local bone "
                          "offsets and were excluded from batching. RemapBoneOffsets must run before the pass.",
                          unremappedThisPass);
        }

        // ── Phase 2: Merge groups with count > 1 ──────────────────────
        FrameDataBuffer& frameBuffer = FrameDataBufferManager::Get();

        // Once-per-BatchCommands-call throttles (this function runs once per
        // frame, so a local flag here is equivalent to the once-per-frame
        // throttle used by FrameDataBuffer's own internal overflow logs —
        // without it, a stress scene with many overflowing groups logs once
        // per group instead of once per frame).
        bool truncationLogged = false;
        bool transformOverflowLogged = false;
        bool prevTransformOverflowLogged = false;
        bool entityIDOverflowLogged = false;
        bool colorOverflowLogged = false;
        bool customOverflowLogged = false;
        bool gpuSceneRefOverflowLogged = false;
        bool lightmapRegionOverflowLogged = false;

        for (auto& [key, indices] : groups)
        {
            if (indices.size() <= 1)
                continue;

            if (indices.size() > m_Config.MaxMeshInstances && !truncationLogged)
            {
                OLO_CORE_WARN("CommandBucket::BatchCommands: Instance group of {} exceeds MaxMeshInstances ({}); "
                              "truncating to the cap. Subsequent truncations this frame will be silent.",
                              indices.size(), m_Config.MaxMeshInstances);
                truncationLogged = true;
            }

            u32 totalInstances = static_cast<u32>(
                std::min(indices.size(), static_cast<sizet>(m_Config.MaxMeshInstances)));

            // A group that survives the cap with fewer than two members is not a
            // batch: emitting an instanced command for it would draw zero or one
            // instance through the batched path for no reason, and the skinned
            // counter below would underflow on `totalInstances - 1`. Reachable
            // only through a MaxMeshInstances of 1, which ValidateConfig allows
            // because it is a legitimate "never batch" setting.
            if (totalInstances <= 1)
                continue;

            // Allocate three parallel per-instance streams in FrameDataBuffer:
            //   • transforms      — current-frame world transforms (always present)
            //   • prevTransforms  — previous-frame transforms for TAA/motion-vector velocity
            //   • entityIDs       — per-source EntityIDs for editor picking
            // All three indexed by the same instance slot (0..totalInstances-1).
            u32 transformOffset = frameBuffer.AllocateTransforms(totalInstances);
            if (transformOffset == UINT32_MAX)
            {
                if (!transformOverflowLogged)
                {
                    OLO_CORE_ERROR("CommandBucket::BatchCommands: Failed to allocate {} transforms in FrameDataBuffer. "
                                   "Subsequent failures this frame will be silent.",
                                   totalInstances);
                    transformOverflowLogged = true;
                }
                continue;
            }
            u32 prevTransformOffset = frameBuffer.AllocateTransforms(totalInstances);
            if (prevTransformOffset == UINT32_MAX)
            {
                if (!prevTransformOverflowLogged)
                {
                    OLO_CORE_WARN("CommandBucket::BatchCommands: Failed to allocate {} prev-transforms; batched motion vectors will alias current frame. "
                                  "Subsequent failures this frame will be silent.",
                                  totalInstances);
                    prevTransformOverflowLogged = true;
                }
                // Continue without prev-transform stream — dispatcher falls back
                // to prevTransforms = transforms, producing zero velocity for
                // this draw (same behaviour as a brand-new entity in frame 0).
            }
            u32 entityIDOffset = frameBuffer.AllocateEntityIDs(totalInstances);
            if (entityIDOffset == UINT32_MAX)
            {
                if (!entityIDOverflowLogged)
                {
                    OLO_CORE_WARN("CommandBucket::BatchCommands: Failed to allocate {} entity IDs; batched picking will return -1. "
                                  "Subsequent failures this frame will be silent.",
                                  totalInstances);
                    entityIDOverflowLogged = true;
                }
            }

            // Only allocate Color / Custom streams when at least one source has
            // a non-default value — common case is no per-entity tinting set,
            // in which case the dispatcher falls back to the InstanceData
            // defaults (1,1,1,1) / 0.0f. Saves N*20 B of per-frame FrameDataBuffer
            // pressure on the typical scene.
            bool anyNonDefaultColor = false;
            bool anyNonDefaultCustom = false;
            bool anyNonDefaultLightmapRegion = false;
            bool anyGPUSceneLink = false;
            constexpr glm::vec4 defaultColor{ 1.0f };
            constexpr f32 defaultCustom = 0.0f;
            constexpr glm::vec4 defaultLightmapRegion{ 0.0f };
            for (u32 t = 0; t < totalInstances; ++t)
            {
                auto const* meshCmd = m_Packets[indices[t]]->GetCommandData<DrawMeshCommand>();
                // Bit-exact comparison to detect any per-entity override — instance defaults
                // are bit-exactly 1.0f / 0.0f, so BitwiseEqual catches anything else (see cpp-coding-quality §2a).
                if (!Math::BitwiseEqual(meshCmd->color, defaultColor))
                    anyNonDefaultColor = true;
                if (!Math::BitwiseEqual(meshCmd->custom, defaultCustom))
                    anyNonDefaultCustom = true;
                if (!Math::BitwiseEqual(meshCmd->lightmapScaleOffset, defaultLightmapRegion))
                    anyNonDefaultLightmapRegion = true;
                if (meshCmd->gpuSceneDrawLink != GPUSceneDrawLinkNone)
                    anyGPUSceneLink = true;
                if (anyNonDefaultColor && anyNonDefaultCustom && anyNonDefaultLightmapRegion)
                    break;
            }

            u32 colorOffset = UINT32_MAX;
            u32 customOffset = UINT32_MAX;
            u32 gpuSceneRefOffset = UINT32_MAX;
            if (anyGPUSceneLink)
            {
                gpuSceneRefOffset = frameBuffer.AllocateGPUSceneRefs(totalInstances);
                if (gpuSceneRefOffset == UINT32_MAX && !gpuSceneRefOverflowLogged)
                {
                    OLO_CORE_WARN("CommandBucket::BatchCommands: Failed to allocate {} GPU Scene references; linked draws will fall back. Subsequent failures this frame will be silent.", totalInstances);
                    gpuSceneRefOverflowLogged = true;
                }
            }
            if (anyNonDefaultColor)
            {
                colorOffset = frameBuffer.AllocateColors(totalInstances);
                if (colorOffset == UINT32_MAX && !colorOverflowLogged)
                {
                    OLO_CORE_WARN("CommandBucket::BatchCommands: Failed to allocate {} colors; per-entity tint lost in batched draw. "
                                  "Subsequent failures this frame will be silent.",
                                  totalInstances);
                    colorOverflowLogged = true;
                }
            }
            if (anyNonDefaultCustom)
            {
                customOffset = frameBuffer.AllocateCustoms(totalInstances);
                if (customOffset == UINT32_MAX && !customOverflowLogged)
                {
                    OLO_CORE_WARN("CommandBucket::BatchCommands: Failed to allocate {} customs; per-entity Custom lost in batched draw. "
                                  "Subsequent failures this frame will be silent.",
                                  totalInstances);
                    customOverflowLogged = true;
                }
            }
            // Lightmap regions ride the generic vec4 (Colors) stream under their
            // own offset (issue #439) — a second vec4 stream would duplicate the
            // allocator for no isolation benefit.
            u32 lightmapRegionOffset = UINT32_MAX;
            if (anyNonDefaultLightmapRegion)
            {
                lightmapRegionOffset = frameBuffer.AllocateColors(totalInstances);
                // Own flag: sharing colorOverflowLogged would let an earlier
                // tint-overflow warning swallow the first (and only) mention
                // that baked lighting was dropped this frame.
                if (lightmapRegionOffset == UINT32_MAX && !lightmapRegionOverflowLogged)
                {
                    OLO_CORE_WARN("CommandBucket::BatchCommands: Failed to allocate {} lightmap regions; baked lighting lost in batched draw. "
                                  "Subsequent failures this frame will be silent.",
                                  totalInstances);
                    lightmapRegionOverflowLogged = true;
                }
            }

            // Write each source command's per-instance data contiguously.
            for (u32 t = 0; t < totalInstances; ++t)
            {
                auto const* meshCmd = m_Packets[indices[t]]->GetCommandData<DrawMeshCommand>();
                frameBuffer.WriteTransforms(transformOffset + t, &meshCmd->transform, 1);
                if (prevTransformOffset != UINT32_MAX)
                    frameBuffer.WriteTransforms(prevTransformOffset + t, &meshCmd->prevTransform, 1);
                if (entityIDOffset != UINT32_MAX)
                    frameBuffer.WriteEntityIDs(entityIDOffset + t, &meshCmd->entityID, 1);
                if (colorOffset != UINT32_MAX)
                    frameBuffer.WriteColors(colorOffset + t, &meshCmd->color, 1);
                if (customOffset != UINT32_MAX)
                    frameBuffer.WriteCustoms(customOffset + t, &meshCmd->custom, 1);
                if (lightmapRegionOffset != UINT32_MAX)
                    frameBuffer.WriteColors(lightmapRegionOffset + t, &meshCmd->lightmapScaleOffset, 1);
                if (gpuSceneRefOffset != UINT32_MAX)
                {
                    glm::uvec4 reference{ GPUSceneDrawRefUnlinked };
                    if (const GPUSceneDrawLink* link = Renderer3D::GetGPUSceneDrawLink(meshCmd->gpuSceneDrawLink); link && link->m_Resolved)
                    {
                        reference = link->Ref();
                        frameBuffer.WriteTransforms(transformOffset + t, &link->m_CurrentTransform, 1);
                        if (prevTransformOffset != UINT32_MAX)
                            frameBuffer.WriteTransforms(prevTransformOffset + t, &link->m_PreviousTransform, 1);
                    }
                    frameBuffer.WriteGPUSceneRefs(gpuSceneRefOffset + t, &reference, 1);
                }
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
            // baseIndex was never copied here, so a batched submesh with a
            // non-zero base drew the wrong index range. Safe now that the
            // group key includes it (all group members share the same value).
            icmd->baseIndex = firstCmd->baseIndex;
            icmd->instanceCount = totalInstances;
            icmd->transformBufferOffset = transformOffset;
            icmd->transformCount = totalInstances;
            icmd->prevTransformBufferOffset = prevTransformOffset;   // UINT32_MAX on alloc failure -> dispatcher aliases current
            icmd->entityIDBufferOffset = entityIDOffset;             // UINT32_MAX on alloc failure -> dispatcher writes -1
            icmd->colorBufferOffset = colorOffset;                   // UINT32_MAX when all sources had identity tint
            icmd->customBufferOffset = customOffset;                 // UINT32_MAX when all sources had Custom == 0
            icmd->gpuSceneRefBufferOffset = gpuSceneRefOffset;
            icmd->lightmapRegionBufferOffset = lightmapRegionOffset; // UINT32_MAX when no source carried a lightmap region
            icmd->shaderHandle = firstCmd->shaderHandle;
            icmd->materialDataIndex = firstCmd->materialDataIndex;
            icmd->renderStateIndex = firstCmd->renderStateIndex;
            icmd->isAnimatedMesh = firstCmd->isAnimatedMesh;
            icmd->boneBufferOffset = firstCmd->boneBufferOffset;
            icmd->boneCountPerInstance = firstCmd->boneCount;
            // Every member of a skinned group was proved byte-identical to
            // this one in BOTH palettes (PartitionSkinnedGroups), so taking the
            // first command's offsets uploads the pose the whole group is in.
            icmd->prevBoneBufferOffset = firstCmd->prevBoneBufferOffset;

            instancedPacket->SetCommandType(icmd->header.type);

            // Replace first packet with the instanced command, null out the rest
            m_Packets[firstIdx] = instancedPacket;
            m_Keys[firstIdx] = instancedPacket->GetMetadata().m_SortKey.GetKey();

            for (u32 t = 1; t < totalInstances; ++t)
            {
                m_Packets[indices[t]] = nullptr;
                ++m_Stats.BatchedCommands;
            }

            // Counted here rather than in PartitionSkinnedGroups so the figure
            // reflects what actually collapsed, after the MaxMeshInstances
            // truncation above.
            if (key.bonePaletteID != 0)
            {
                ++m_Stats.SkinnedBatchGroups;
                m_Stats.SkinnedBatchedCommands += totalInstances - 1;
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

    CommandBucket::Statistics CommandBucket::ReplayRange(RendererAPI& rendererAPI, sizet begin, sizet end) const
    {
        OLO_CORE_ASSERT(begin <= end && end <= m_Packets.size(), "Invalid replay range");
        return ReplayPackets(rendererAPI, std::span<CommandPacket* const>(m_Packets).subspan(begin, end - begin), m_ViewState);
    }

    CommandBucket::Statistics CommandBucket::ReplayPackets(RendererAPI& rendererAPI, std::span<CommandPacket* const> packets, const std::optional<BucketViewState>& view)
    {
        OLO_PROFILE_FUNCTION();

        Statistics stats;

        // Bind per-bucket view state if set, saving the previous state for restoration
        BucketViewState savedState;
        bool restoreState = false;
        if (view.has_value() && s_ViewStateReader && s_ViewStateWriter)
        {
            s_ViewStateReader(savedState);
            restoreState = true;
            s_ViewStateWriter(*view);
        }

        // Execute all commands in order from the flat array
        for (const auto* packet : packets)
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
                ++stats.DrawCalls;
            }
            else if (type != CommandType::Invalid)
            {
                ++stats.StateChanges;
            }
            else
            {
                // No additional handling required.
            }

            packet->Execute(rendererAPI);
        }

        // Restore previous view state if we changed it
        if (restoreState)
        {
            s_ViewStateWriter(savedState);
        }

        return stats;
    }

    void CommandBucket::Execute(RendererAPI& rendererAPI)
    {
        OLO_PROFILE_FUNCTION();
        const auto start = std::chrono::steady_clock::now();
        const auto stats = ReplayRange(rendererAPI, 0, m_Packets.size());
        m_Stats.DrawCalls = stats.DrawCalls;
        m_Stats.StateChanges = stats.StateChanges;
        m_LastExecuteTimeMs = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    CommandBucket::ParallelReplayPlan CommandBucket::PlanParallelReplay(std::span<CommandPacket* const> packets, u32 minCommandsPerItem)
    {
        ParallelReplayPlan plan;
        if (!s_ParallelReplayClassifier)
            return plan;
        plan.ItemCount = static_cast<u32>(std::clamp<sizet>(packets.size() / std::max(1u, minCommandsPerItem), 1u, MAX_RENDER_WORKERS));
        if (plan.ItemCount < 2u)
            return plan;
        for (const auto* packet : packets)
        {
            if (!packet)
                continue;
            const u32 capacity = s_ParallelReplayClassifier(*packet);
            if (capacity == 0u)
                return {};
            plan.InstanceCapacity = std::max(plan.InstanceCapacity, capacity);
        }
        return plan;
    }

    CommandBucket::Statistics CommandBucket::RecordPackets(RendererAPI& rendererAPI, std::span<CommandPacket* const> packets,
                                                           const std::optional<BucketViewState>& view, u32 minCommandsPerItem)
    {
        if (packets.empty())
            return {};
        const auto plan = PlanParallelReplay(packets, minCommandsPerItem);
        std::array<Statistics, MAX_RENDER_WORKERS> results;
        rendererAPI.RecordParallel(plan.ItemCount, [&](u32 item)
                                   {
            const sizet begin = packets.size() * item / plan.ItemCount;
            const sizet end = packets.size() * (item + 1u) / plan.ItemCount;
            results[item] = ReplayPackets(rendererAPI, packets.subspan(begin, end - begin), view); }, plan.InstanceCapacity);
        Statistics stats;
        for (u32 item = 0; item < plan.ItemCount; ++item)
        {
            stats.DrawCalls += results[item].DrawCalls;
            stats.StateChanges += results[item].StateChanges;
        }
        return stats;
    }

    void CommandBucket::ExecuteParallel(RendererAPI& rendererAPI, u32 minCommandsPerItem)
    {
        OLO_PROFILE_FUNCTION();
        const auto start = std::chrono::steady_clock::now();
        const auto stats = RecordPackets(rendererAPI, m_Packets, m_ViewState, minCommandsPerItem);
        m_Stats.DrawCalls = stats.DrawCalls;
        m_Stats.StateChanges = stats.StateChanges;
        m_LastExecuteTimeMs = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    void CommandBucket::ExecuteWithGPUTiming(RendererAPI& rendererAPI)
    {
        OLO_PROFILE_FUNCTION();

        // Timestamp pools are primary-only. A recording item remains a legal
        // replay, with CPU item timing reported by the recording context.
        if (rendererAPI.IsRecordingParallelItem())
        {
            Execute(rendererAPI);
            return;
        }

        auto& gpuTimer = GPUTimerQueryPool::GetInstance();

        if (!gpuTimer.IsInitialized())
            gpuTimer.Initialize();

        gpuTimer.BeginFrame();

        auto execStart = std::chrono::high_resolution_clock::now();

        // Timestamp recording and this bucket are owned by the primary thread here.
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
                ++m_Stats.DrawCalls;
            }
            else if (type != CommandType::Invalid)
            {
                ++m_Stats.StateChanges;
            }
            else
            {
                // No additional handling required.
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

            ++cmdIndex;
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

    void CommandBucket::UseWorkerIndex(u32 workerIndex) const
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
        if (u32 requiredCapacity = batchStart + TLS_BATCH_SIZE; requiredCapacity > m_ParallelCommands.size())
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
        ++slot.offset;
        --slot.remaining;

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

        if (u32 totalCommands = m_ParallelCommandCount.load(std::memory_order_acquire); totalCommands == 0)
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
                ++m_CommandCount;
            }
        }

        // Reset parallel submission state
        m_ParallelSubmissionActive = false;

        // Invalidate sorting and batching since we have new commands
        m_IsSorted = false;
        m_IsBatched = false;
    }

    void CommandBucket::PartitionSkinnedGroups(const InstanceGroupMap& candidates, InstanceGroupMap& groups)
    {
        OLO_PROFILE_FUNCTION();

        if (candidates.empty())
            return;

        const FrameDataBuffer& frameBuffer = FrameDataBufferManager::Get();

        // Partition ordinals start at 1 so they never collide with the 0 every
        // static group carries.
        u64 nextPaletteID = 1;

        std::vector<SkinnedBatching::PalettePair> pairs;
        std::unordered_map<u64, std::vector<sizet>> byHash; // palette hash -> indices into `pairs`
        std::vector<std::vector<sizet>> partitions;         // exact-match partitions, indices into `pairs`

        for (auto const& [geometryKey, indices] : candidates)
        {
            // One actor of this mesh and material has nobody to share a pose
            // with. Leaving early here is what makes the single-character case
            // free: no palette is ever read, let alone hashed.
            if (indices.size() <= 1)
                continue;

            pairs.clear();
            pairs.reserve(indices.size());
            for (sizet packetIndex : indices)
            {
                auto const* cmd = m_Packets[packetIndex]->GetCommandData<DrawMeshCommand>();
                const glm::mat4* current = frameBuffer.GetBoneMatrixRange(cmd->boneBufferOffset, cmd->boneCount);
                if (!current)
                    continue; // no palette to compare; the draw stays its own DrawMesh

                // UINT32_MAX is UploadBoneMatrices' alias-current sentinel.
                // Resolving it to the current palette here is deliberate: a
                // first-frame actor and a settled one whose previous pose
                // equals its current upload the same bytes, so they may share
                // a draw. Every other case compares the real previous pose,
                // which is what keeps skeletal velocity per-group correct.
                const glm::mat4* prev = cmd->prevBoneBufferOffset == UINT32_MAX
                                            ? current
                                            : frameBuffer.GetBoneMatrixRange(cmd->prevBoneBufferOffset, cmd->boneCount);
                if (!prev)
                    prev = current;

                pairs.push_back({ packetIndex, current, prev, cmd->boneCount });
            }

            if (pairs.size() <= 1)
                continue;

            byHash.clear();
            for (sizet p = 0; p < pairs.size(); ++p)
                byHash[SkinnedBatching::HashPalettes(pairs[p])].push_back(p);

            for (auto const& [hash, bucket] : byHash)
            {
                if (bucket.size() <= 1)
                    continue;

                // Inside one hash bucket, confirm by comparison. Collisions are
                // rare enough that this is normally a single partition, but it
                // is the comparison and not the hash that decides.
                partitions.clear();
                for (sizet p : bucket)
                {
                    bool placed = false;
                    for (auto& partition : partitions)
                    {
                        if (SkinnedBatching::SamePose(pairs[partition.front()], pairs[p]))
                        {
                            partition.push_back(p);
                            placed = true;
                            break;
                        }
                    }
                    if (!placed)
                        partitions.push_back({ p });
                }

                for (auto const& partition : partitions)
                {
                    if (partition.size() <= 1)
                        continue;

                    InstanceGroupKey poseKey = geometryKey;
                    poseKey.bonePaletteID = nextPaletteID++;

                    std::vector<sizet>& target = groups[poseKey];
                    target.reserve(partition.size());
                    for (sizet p : partition)
                        target.push_back(pairs[p].m_PacketIndex);

                    // Merge phase 2 walks `indices` in order and keeps the
                    // first as the surviving packet. Submission order is the
                    // order the scene produced, and the hash/partition walk
                    // above does not preserve it, so restore it — a batch that
                    // reorders its own instances would move per-instance entity
                    // IDs relative to the transforms they belong to across
                    // frames for no reason, and makes any capture diff noise.
                    std::sort(target.begin(), target.end());
                }
            }
        }
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
                    // Also remap the previous-frame bone offset when present.
                    // UINT32_MAX is the alias-current sentinel and must be left
                    // untouched so CommandDispatch::UploadBoneMatrices uploads
                    // the current palette into both current and prev slots.
                    if (cmd->prevBoneBufferOffset != UINT32_MAX)
                    {
                        cmd->prevBoneBufferOffset = frameDataBuffer.GetGlobalBoneOffset(cmd->workerIndex, cmd->prevBoneBufferOffset);
                    }
                    cmd->needsBoneOffsetRemap = false;
                    ++remappedCount;
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
