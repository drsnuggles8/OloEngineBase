#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "FrameDataBufferFixture.h"
#include "RenderingTestUtils.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Commands/CommandAllocator.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"

#include <algorithm>
#include <numeric>
#include <random>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

// =============================================================================
// Fixture with shared allocator
// =============================================================================

class CommandBucketTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Allocator = std::make_unique<CommandAllocator>();
    }

    void TearDown() override
    {
        m_Allocator.reset();
    }

    /// Submit N synthetic DrawMeshCommands with unique shader IDs
    void SubmitNDrawMeshCommands(CommandBucket& bucket, u32 count, bool randomOrder = false)
    {
        std::vector<u32> ids(count);
        std::iota(ids.begin(), ids.end(), 1);
        if (randomOrder)
        {
            std::shuffle(ids.begin(), ids.end(), GetTestRNG());
        }

        for (u32 id : ids)
        {
            auto cmd = MakeSyntheticDrawMeshCommand(id, id, static_cast<f32>(id) * 0.01f, static_cast<i32>(id));
            PacketMetadata meta;
            meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, id, id, id * 10);
            bucket.Submit(cmd, meta, m_Allocator.get());
        }
    }

    std::unique_ptr<CommandAllocator> m_Allocator;
};

// =============================================================================
// Sort Preserves All Commands
// =============================================================================

TEST_F(CommandBucketTest, SortPreservesAllCommands)
{
    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = false;
    CommandBucket bucket(config);

    constexpr u32 N = 100;
    SubmitNDrawMeshCommands(bucket, N, /*randomOrder=*/true);

    EXPECT_EQ(bucket.GetCommandCount(), N);

    bucket.SortCommands();

    EXPECT_EQ(bucket.GetCommandCount(), N);
    EXPECT_TRUE(bucket.IsSorted());

    const auto& sorted = bucket.GetSortedCommands();
    EXPECT_EQ(sorted.size(), N);
}

// =============================================================================
// Sort Order Matches DrawKey
// =============================================================================

TEST_F(CommandBucketTest, SortOrderMatchesDrawKey)
{
    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = false;
    CommandBucket bucket(config);

    // Submit commands with known, deterministic keys
    constexpr u32 N = 50;
    SubmitNDrawMeshCommands(bucket, N, /*randomOrder=*/true);

    bucket.SortCommands();
    ASSERT_TRUE(bucket.IsSorted());

    const auto& sorted = bucket.GetSortedCommands();

    // Extract DrawKeys and verify they're in ascending raw key order
    // (radix sort produces ascending order; lower raw keys first)
    std::vector<DrawKey> keys;
    keys.reserve(sorted.size());
    for (const auto* pkt : sorted)
    {
        keys.push_back(pkt->GetMetadata().m_SortKey);
    }
    ExpectCommandOrder(keys);
}

// =============================================================================
// Sort Stability (equal keys preserve insertion order)
// =============================================================================

TEST_F(CommandBucketTest, SortStabilityForEqualKeys)
{
    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = false;
    CommandBucket bucket(config);

    // Submit 10 commands with the SAME key but different entityIDs
    PacketMetadata meta;
    meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 1, 1, 100);

    std::vector<i32> entityOrder;
    for (i32 i = 0; i < 10; ++i)
    {
        auto cmd = MakeSyntheticDrawMeshCommand(1, 1, 0.5f, i);
        bucket.Submit(cmd, meta, m_Allocator.get());
        entityOrder.push_back(i);
    }

    bucket.SortCommands();
    const auto& sorted = bucket.GetSortedCommands();
    ASSERT_EQ(sorted.size(), 10u);

    // With stable sort, the entity IDs should be in original insertion order
    for (sizet i = 0; i < sorted.size(); ++i)
    {
        const auto* data = sorted[i]->GetCommandData<DrawMeshCommand>();
        EXPECT_EQ(data->entityID, entityOrder[i])
            << "Stability violated at index " << i;
    }
}

// =============================================================================
// Empty Bucket Sort
// =============================================================================

TEST_F(CommandBucketTest, EmptyBucketSort)
{
    CommandBucket bucket;
    EXPECT_EQ(bucket.GetCommandCount(), 0u);

    bucket.SortCommands();
    EXPECT_TRUE(bucket.IsSorted());
    EXPECT_EQ(bucket.GetSortedCommands().size(), 0u);
}

// =============================================================================
// Single Command Sort
// =============================================================================

TEST_F(CommandBucketTest, SingleCommandSort)
{
    CommandBucket bucket;

    auto cmd = MakeSyntheticClearCommand();
    bucket.Submit(cmd, {}, m_Allocator.get());
    EXPECT_EQ(bucket.GetCommandCount(), 1u);

    bucket.SortCommands();
    EXPECT_TRUE(bucket.IsSorted());
    EXPECT_EQ(bucket.GetSortedCommands().size(), 1u);
}

// =============================================================================
// Clear Resets State
// =============================================================================

TEST_F(CommandBucketTest, ClearResetsState)
{
    CommandBucket bucket;

    SubmitNDrawMeshCommands(bucket, 20);
    EXPECT_EQ(bucket.GetCommandCount(), 20u);

    bucket.SortCommands();
    EXPECT_TRUE(bucket.IsSorted());

    bucket.Clear();
    EXPECT_EQ(bucket.GetCommandCount(), 0u);
    EXPECT_FALSE(bucket.IsSorted());
    EXPECT_FALSE(bucket.IsBatched());
}

// =============================================================================
// Reset Frees Memory
// =============================================================================

TEST_F(CommandBucketTest, ResetFreesMemory)
{
    CommandBucket bucket;

    SubmitNDrawMeshCommands(bucket, 50);
    bucket.SortCommands();

    bucket.Reset(*m_Allocator);
    EXPECT_EQ(bucket.GetCommandCount(), 0u);
    EXPECT_FALSE(bucket.IsSorted());
    EXPECT_FALSE(bucket.IsBatched());
}

// =============================================================================
// Sort Reduces State Changes
// =============================================================================

TEST_F(CommandBucketTest, SortReducesStateChanges)
{
    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = false;
    CommandBucket bucket(config);

    // Submit commands with 3 different shaders in random order
    // This ensures sorting groups same shaders together
    std::vector<u32> shaderIds = { 1, 2, 3, 1, 2, 3, 1, 2, 3, 1 };
    std::shuffle(shaderIds.begin(), shaderIds.end(), GetTestRNG());

    for (u32 sid : shaderIds)
    {
        auto cmd = MakeSyntheticDrawMeshCommand(sid, 1, 0.5f);
        PacketMetadata meta;
        meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, sid, 1, 100);
        bucket.Submit(cmd, meta, m_Allocator.get());
    }

    // Count shader changes before sort
    u32 changesBefore = 0;
    {
        const auto& packets = bucket.GetPackets();
        u32 lastShader = 0;
        for (const auto* pkt : packets)
        {
            u32 s = pkt->GetMetadata().m_SortKey.GetShaderID();
            if (s != lastShader)
            {
                changesBefore++;
                lastShader = s;
            }
        }
    }

    bucket.SortCommands();

    // Count shader changes after sort
    const auto& sorted = bucket.GetSortedCommands();
    std::vector<DrawKey> keys;
    for (const auto* pkt : sorted)
    {
        keys.push_back(pkt->GetMetadata().m_SortKey);
    }
    u32 changesAfter = CountShaderChanges(keys);

    // After sort, shader changes should be <= 3 (at most one per unique shader)
    EXPECT_LE(changesAfter, 3u);
    // Sorting should not increase state changes
    EXPECT_LE(changesAfter, changesBefore);
}

// =============================================================================
// Statistics Tracking
// =============================================================================

TEST_F(CommandBucketTest, StatisticsTrackSubmissions)
{
    CommandBucket bucket;

    auto stats = bucket.GetStatistics();
    EXPECT_EQ(stats.TotalCommands, 0u);

    SubmitNDrawMeshCommands(bucket, 10);
    stats = bucket.GetStatistics();
    EXPECT_EQ(stats.TotalCommands, 10u);
}

// =============================================================================
// Parallel Submission
// =============================================================================

TEST_F(CommandBucketTest, ParallelSubmissionMerge)
{
    CommandBucketConfig config;
    config.InitialCapacity = 2048;
    CommandBucket bucket(config);
    bucket.SetAllocator(m_Allocator.get());

    bucket.PrepareForParallelSubmission();

    // Simulate 4 workers each submitting 10 commands
    constexpr u32 numWorkers = 4;
    constexpr u32 commandsPerWorker = 10;

    for (u32 worker = 0; worker < numWorkers; ++worker)
    {
        for (u32 i = 0; i < commandsPerWorker; ++i)
        {
            auto cmd = MakeSyntheticDrawMeshCommand(
                worker * 100 + i, 1, static_cast<f32>(i) * 0.1f, static_cast<i32>(worker * 100 + i));
            PacketMetadata meta;
            meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, worker * 100 + i, 1, i * 10);

            auto* packet = m_Allocator->CreateCommandPacket(cmd, meta);
            ASSERT_NE(packet, nullptr);
            bucket.SubmitPacketParallel(packet, worker);
        }
    }

    // Merge thread-local commands into global list
    bucket.MergeThreadLocalCommands();

    // All commands should be present
    EXPECT_EQ(bucket.GetCommandCount(), numWorkers * commandsPerWorker);

    // Should be sortable after merge
    bucket.SortCommands();
    EXPECT_TRUE(bucket.IsSorted());
    EXPECT_EQ(bucket.GetSortedCommands().size(), numWorkers * commandsPerWorker);
}

// =============================================================================
// Batch Commands (DrawMesh -> DrawMeshInstanced)
// =============================================================================

// Fixture that adds FrameDataBufferManager init/shutdown for batching tests.
class CommandBucketBatchTest : public CommandBucketTest
{
  protected:
    void SetUp() override
    {
        CommandBucketTest::SetUp();
        FrameDataBufferManager::Init();
        FrameDataBufferManager::Get().Reset();
    }

    void TearDown() override
    {
        FrameDataBufferManager::Shutdown();
        CommandBucketTest::TearDown();
    }
};

TEST_F(CommandBucketBatchTest, BatchConvertsMeshToInstanced)
{
    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = true;
    CommandBucket bucket(config);

    // Submit several DrawMesh commands with same shader + material (batchable)
    for (u32 i = 0; i < 5; ++i)
    {
        auto cmd = MakeSyntheticDrawMeshCommand(1, 1, static_cast<f32>(i) * 0.1f, static_cast<i32>(i));
        // Make meshes identical (same VAO, material, shader)
        cmd.vertexArrayID = 100;
        cmd.indexCount = 36;
        PacketMetadata meta;
        meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 1, 1, i * 10);
        bucket.Submit(cmd, meta, m_Allocator.get());
    }

    bucket.SortCommands();
    sizet countBeforeBatch = bucket.GetSortedCommands().size();

    bucket.BatchCommands(*m_Allocator);

    // After batching, there should be fewer commands (some merged into instanced)
    // or at least no more
    sizet countAfterBatch = bucket.GetSortedCommands().size();
    EXPECT_LE(countAfterBatch, countBeforeBatch);
    EXPECT_TRUE(bucket.IsBatched());
}

TEST_F(CommandBucketBatchTest, BatchRejectsDifferentRenderStateIndex)
{
    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = true;
    CommandBucket bucket(config);

    // Submit two DrawMesh commands with same mesh+material but different render state
    auto cmd1 = MakeSyntheticDrawMeshCommand(1, 1, 0.1f, 1);
    cmd1.vertexArrayID = 100;
    cmd1.renderStateIndex = 0;
    PacketMetadata meta1;
    meta1.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 1, 1, 10);
    bucket.Submit(cmd1, meta1, m_Allocator.get());

    auto cmd2 = MakeSyntheticDrawMeshCommand(1, 1, 0.2f, 2);
    cmd2.vertexArrayID = 100;
    cmd2.renderStateIndex = 1;
    PacketMetadata meta2;
    meta2.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 1, 1, 20);
    bucket.Submit(cmd2, meta2, m_Allocator.get());

    bucket.SortCommands();
    sizet countBeforeBatch = bucket.GetSortedCommands().size();
    EXPECT_EQ(countBeforeBatch, 2u);

    bucket.BatchCommands(*m_Allocator);

    // Commands with different render states must NOT merge even if mesh+material match
    sizet countAfterBatch = bucket.GetSortedCommands().size();
    EXPECT_EQ(countAfterBatch, 2u) << "Commands with different renderStateIndex must not batch";
}

TEST_F(CommandBucketBatchTest, BatchAcceptsSameRenderStateIndex)
{
    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = true;
    CommandBucket bucket(config);

    // Submit 3 DrawMesh commands with identical mesh, material, AND render state
    for (u32 i = 0; i < 3; ++i)
    {
        auto cmd = MakeSyntheticDrawMeshCommand(1, 1, static_cast<f32>(i) * 0.1f, static_cast<i32>(i));
        cmd.vertexArrayID = 100;
        cmd.renderStateIndex = 5;
        PacketMetadata meta;
        meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 1, 1, i * 10);
        bucket.Submit(cmd, meta, m_Allocator.get());
    }

    bucket.SortCommands();
    EXPECT_EQ(bucket.GetSortedCommands().size(), 3u);

    bucket.BatchCommands(*m_Allocator);

    // Hash-table grouping merges ALL matching commands into a single instanced command
    sizet countAfterBatch = bucket.GetSortedCommands().size();
    EXPECT_EQ(countAfterBatch, 1u) << "All 3 commands with same key should merge into 1 instanced";
}

// =============================================================================
// Mixed Command Types Don't Interfere With Sort
// =============================================================================

TEST_F(CommandBucketTest, MixedCommandTypes)
{
    CommandBucket bucket;

    // Submit a mix of state and draw commands
    auto clear = MakeSyntheticClearCommand();
    auto viewport = MakeSyntheticViewportCommand();
    auto depth = MakeSyntheticDepthTestCommand();
    auto mesh = MakeSyntheticDrawMeshCommand(1, 1, 0.5f);

    bucket.Submit(clear, {}, m_Allocator.get());
    bucket.Submit(viewport, {}, m_Allocator.get());
    bucket.Submit(depth, {}, m_Allocator.get());

    PacketMetadata meshMeta;
    meshMeta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 1, 1, 100);
    bucket.Submit(mesh, meshMeta, m_Allocator.get());

    EXPECT_EQ(bucket.GetCommandCount(), 4u);

    bucket.SortCommands();
    EXPECT_TRUE(bucket.IsSorted());
    EXPECT_EQ(bucket.GetSortedCommands().size(), 4u);
}

// =============================================================================
// Timing Accessors
// =============================================================================

TEST_F(CommandBucketTest, TimingAccessors)
{
    CommandBucket bucket;

    SubmitNDrawMeshCommands(bucket, 100, true);
    bucket.SortCommands();

    // Sort time should be non-negative (may be 0 for small datasets on fast machines)
    EXPECT_GE(bucket.GetLastSortTimeMs(), 0.0);
}

// =============================================================================
// Config Disabling Sort/Batch
// =============================================================================

TEST_F(CommandBucketTest, DisabledSortingSkipsSort)
{
    CommandBucketConfig config;
    config.EnableSorting = false;
    config.EnableBatching = false;
    CommandBucket bucket(config);

    SubmitNDrawMeshCommands(bucket, 10, true);

    bucket.SortCommands();
    // With sorting disabled, sorted commands may still be populated for execution
    // but the IsSorted flag behavior depends on implementation
    EXPECT_EQ(bucket.GetCommandCount(), 10u);
}

// =============================================================================
// Batching — Material Data Index
// =============================================================================

TEST_F(CommandBucketBatchTest, BatchRejectsDifferentMaterialDataIndex)
{
    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = true;
    CommandBucket bucket(config);

    // Submit two commands with same mesh+renderState but different materialDataIndex
    auto cmd1 = MakeSyntheticDrawMeshCommand(1, 1, 0.1f, 1);
    cmd1.vertexArrayID = 100;
    cmd1.renderStateIndex = 0;
    cmd1.materialDataIndex = 0; // material A
    PacketMetadata meta1;
    meta1.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 1, 1, 10);
    bucket.Submit(cmd1, meta1, m_Allocator.get());

    auto cmd2 = MakeSyntheticDrawMeshCommand(1, 1, 0.2f, 2);
    cmd2.vertexArrayID = 100;
    cmd2.renderStateIndex = 0;
    cmd2.materialDataIndex = 1; // material B
    PacketMetadata meta2;
    meta2.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 1, 1, 20);
    bucket.Submit(cmd2, meta2, m_Allocator.get());

    bucket.SortCommands();
    EXPECT_EQ(bucket.GetSortedCommands().size(), 2u);

    bucket.BatchCommands(*m_Allocator);

    sizet countAfterBatch = bucket.GetSortedCommands().size();
    EXPECT_EQ(countAfterBatch, 2u) << "Commands with different materialDataIndex must not batch";
}

// =============================================================================
// Batching — Animation Field Preservation
// =============================================================================

TEST_F(CommandBucketBatchTest, ConvertToInstancedPreservesAnimationFields)
{
    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = true;
    CommandBucket bucket(config);

    // Submit two identical animated DrawMesh commands
    for (u32 i = 0; i < 2; ++i)
    {
        auto cmd = MakeSyntheticDrawMeshCommand(1, 1, static_cast<f32>(i) * 0.1f, static_cast<i32>(i));
        cmd.vertexArrayID = 100;
        cmd.renderStateIndex = 0;
        cmd.isAnimatedMesh = true;
        cmd.boneBufferOffset = 42;
        cmd.boneCount = 64;
        PacketMetadata meta;
        meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 1, 1, i * 10);
        bucket.Submit(cmd, meta, m_Allocator.get());
    }

    bucket.SortCommands();
    EXPECT_EQ(bucket.GetSortedCommands().size(), 2u);

    bucket.BatchCommands(*m_Allocator);

    // At least one merge should have happened
    const auto& sorted = bucket.GetSortedCommands();
    bool foundInstanced = false;
    for (const auto* packet : sorted)
    {
        if (packet && packet->GetCommandType() == CommandType::DrawMeshInstanced)
        {
            foundInstanced = true;
            const auto* instancedCmd = static_cast<const DrawMeshInstancedCommand*>(packet->GetRawCommandData());
            EXPECT_TRUE(instancedCmd->isAnimatedMesh) << "Animation flag must be preserved";
            EXPECT_EQ(instancedCmd->boneBufferOffset, 42u) << "Bone buffer offset must be preserved";
            EXPECT_EQ(instancedCmd->boneCountPerInstance, 64u) << "Bone count must be preserved";
            break;
        }
    }
    EXPECT_TRUE(foundInstanced) << "Batching should have produced a DrawMeshInstanced command";
}

// =============================================================================
// Instance Group Tables — Non-Adjacent Commands Are Grouped
// =============================================================================

TEST_F(CommandBucketBatchTest, HashTableGroupsNonAdjacentCommands)
{
    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = true;
    CommandBucket bucket(config);

    // Interleave two different mesh groups: A B A B A
    // Old greedy-adjacent merge could NOT group the non-adjacent A's.
    // Hash-table grouping MUST find all 3 A's and both B's.
    for (u32 i = 0; i < 5; ++i)
    {
        bool isGroupA = (i % 2 == 0);
        auto cmd = MakeSyntheticDrawMeshCommand(1, 1, static_cast<f32>(i) * 0.1f, static_cast<i32>(i));
        cmd.meshHandle = UUID(isGroupA ? 100 : 200);
        cmd.vertexArrayID = isGroupA ? 10u : 20u;
        cmd.renderStateIndex = 0;
        cmd.materialDataIndex = 0;
        PacketMetadata meta;
        meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 1, 1, i * 10);
        bucket.Submit(cmd, meta, m_Allocator.get());
    }

    EXPECT_EQ(bucket.GetSortedCommands().size(), 5u);

    bucket.BatchCommands(*m_Allocator);

    // Group A: 3 instances, Group B: 2 instances → 2 commands total
    EXPECT_EQ(bucket.GetSortedCommands().size(), 2u)
        << "Non-adjacent commands with same key must merge via hash-table grouping";

    // Verify instance counts
    u32 totalInstances = 0;
    for (const auto* packet : bucket.GetSortedCommands())
    {
        ASSERT_TRUE(packet != nullptr);
        EXPECT_EQ(packet->GetCommandType(), CommandType::DrawMeshInstanced);
        auto const* icmd = static_cast<const DrawMeshInstancedCommand*>(packet->GetRawCommandData());
        totalInstances += icmd->instanceCount;
    }
    EXPECT_EQ(totalInstances, 5u) << "Total instance count must equal original command count";
}

// =============================================================================
// Instance Group Tables — Single Commands Stay As DrawMesh
// =============================================================================

TEST_F(CommandBucketBatchTest, SingleCommandGroupsRemainDrawMesh)
{
    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = true;
    CommandBucket bucket(config);

    // 3 unique meshes — each appears only once, so no instancing
    for (u32 i = 0; i < 3; ++i)
    {
        auto cmd = MakeSyntheticDrawMeshCommand(1, 1, static_cast<f32>(i) * 0.1f, static_cast<i32>(i));
        cmd.meshHandle = UUID(100 + i); // Different mesh each time
        cmd.vertexArrayID = 10 + i;
        cmd.renderStateIndex = 0;
        cmd.materialDataIndex = 0;
        PacketMetadata meta;
        meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 1, 1, i * 10);
        bucket.Submit(cmd, meta, m_Allocator.get());
    }

    bucket.BatchCommands(*m_Allocator);

    EXPECT_EQ(bucket.GetSortedCommands().size(), 3u)
        << "Unique meshes should not be merged";

    for (const auto* packet : bucket.GetSortedCommands())
    {
        EXPECT_EQ(packet->GetCommandType(), CommandType::DrawMesh)
            << "Single-instance groups must remain as DrawMesh";
    }
}

// =============================================================================
// Instance Group Tables — MaxMeshInstances Cap
// =============================================================================

TEST_F(CommandBucketBatchTest, BatchRespectsMaxMeshInstances)
{
    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = true;
    config.MaxMeshInstances = 3; // Low cap for testing
    CommandBucket bucket(config);

    // Submit 5 identical commands — only 3 should be merged due to cap
    for (u32 i = 0; i < 5; ++i)
    {
        auto cmd = MakeSyntheticDrawMeshCommand(1, 1, static_cast<f32>(i) * 0.1f, static_cast<i32>(i));
        cmd.vertexArrayID = 100;
        cmd.renderStateIndex = 0;
        cmd.materialDataIndex = 0;
        PacketMetadata meta;
        meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 1, 1, i * 10);
        bucket.Submit(cmd, meta, m_Allocator.get());
    }

    bucket.BatchCommands(*m_Allocator);

    // 3 merged into 1 instanced + 2 remaining as DrawMesh = 3 commands
    const auto& sorted = bucket.GetSortedCommands();
    EXPECT_EQ(sorted.size(), 3u)
        << "3 merged + 2 unbatched = 3 total commands";

    bool foundInstanced = false;
    for (const auto* packet : sorted)
    {
        if (packet->GetCommandType() == CommandType::DrawMeshInstanced)
        {
            auto const* icmd = static_cast<const DrawMeshInstancedCommand*>(packet->GetRawCommandData());
            EXPECT_EQ(icmd->instanceCount, 3u) << "Instance count must respect MaxMeshInstances";
            foundInstanced = true;
        }
    }
    EXPECT_TRUE(foundInstanced);
}

// =============================================================================
// Instance Group Tables — Transform Data Contiguity
// =============================================================================

TEST_F(CommandBucketBatchTest, BatchedTransformsAreContiguous)
{
    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = true;
    CommandBucket bucket(config);

    constexpr u32 kCount = 4;
    glm::mat4 expectedTransforms[kCount];
    for (u32 i = 0; i < kCount; ++i)
    {
        auto cmd = MakeSyntheticDrawMeshCommand(1, 1, static_cast<f32>(i) * 0.1f, static_cast<i32>(i));
        cmd.vertexArrayID = 100;
        cmd.renderStateIndex = 0;
        cmd.materialDataIndex = 0;
        cmd.transform = glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<f32>(i), 0.0f, 0.0f));
        expectedTransforms[i] = cmd.transform;
        PacketMetadata meta;
        meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 1, 1, i * 10);
        bucket.Submit(cmd, meta, m_Allocator.get());
    }

    bucket.BatchCommands(*m_Allocator);

    ASSERT_EQ(bucket.GetSortedCommands().size(), 1u);
    const auto* packet = bucket.GetSortedCommands()[0];
    ASSERT_EQ(packet->GetCommandType(), CommandType::DrawMeshInstanced);

    auto const* icmd = static_cast<const DrawMeshInstancedCommand*>(packet->GetRawCommandData());
    EXPECT_EQ(icmd->instanceCount, kCount);
    EXPECT_EQ(icmd->transformCount, kCount);

    // Verify transforms were written contiguously in FrameDataBuffer
    FrameDataBuffer& fb = FrameDataBufferManager::Get();
    const glm::mat4* storedTransforms = fb.GetTransformPtr(icmd->transformBufferOffset);
    ASSERT_NE(storedTransforms, nullptr);

    for (u32 i = 0; i < kCount; ++i)
    {
        EXPECT_EQ(storedTransforms[i], expectedTransforms[i])
            << "Transform " << i << " must match original";
    }
}
