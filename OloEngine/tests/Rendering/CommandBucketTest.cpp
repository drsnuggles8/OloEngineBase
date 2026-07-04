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
    void SubmitNDrawMeshCommands(CommandBucket& bucket, u32 count, bool randomOrder = false) const
    {
        std::vector<u32> ids(count);
        std::iota(ids.begin(), ids.end(), 1);
        if (randomOrder)
        {
            std::shuffle(ids.begin(), ids.end(), MakeTestRNG());
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
    std::shuffle(shaderIds.begin(), shaderIds.end(), MakeTestRNG());

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
                ++changesBefore;
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
        // Reuse a manager an earlier renderer test already brought up rather
        // than re-Init()-ing it (which asserts). Only own/shutdown what we
        // initialise — see FrameDataBufferFixture for the full rationale.
        m_OwnsFrameData = !FrameDataBufferManager::IsInitialized();
        if (m_OwnsFrameData)
            FrameDataBufferManager::Init();
        FrameDataBufferManager::Get().Reset();
    }

    void TearDown() override
    {
        if (m_OwnsFrameData)
            FrameDataBufferManager::Shutdown();
        CommandBucketTest::TearDown();
    }

  private:
    bool m_OwnsFrameData = false;
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

TEST_F(CommandBucketBatchTest, AnimatedMeshesAreNotBatched)
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

    // Animated meshes have per-instance bone data and must not be merged
    const auto& sorted = bucket.GetSortedCommands();
    for (const auto* packet : sorted)
    {
        EXPECT_NE(packet->GetCommandType(), CommandType::DrawMeshInstanced)
            << "Animated meshes should not be batched into instanced commands";
    }
    EXPECT_EQ(sorted.size(), 2u) << "Both original commands should remain";
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

// =============================================================================
// 10k-Instance Stress: GPU Instancing acceptance criterion
// =============================================================================
// Issue #173 acceptance: "10,000+ instances rendered in 1-2 draw calls". The
// default CommandBucketConfig::MaxMeshInstances is 16384 (well below the
// FrameDataBuffer EntityID/Color/Custom capacity of 262144 raised for the
// GPU-cull InstancedMeshComponent path by issue #524 — a different subsystem;
// see CommandBucket.h), so 10k same-mesh draws collapse into a single
// DrawMeshInstanced packet without raising the cap in the test. Dispatcher
// uses a TLS heap scratch buffer, so the 10k * 224 B = 2.24 MB of per-instance
// data lives off-stack.

TEST_F(CommandBucketBatchTest, TenThousandInstancesCollapseToSinglePacket)
{
    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = true;
    // Uses the default MaxMeshInstances (16384) — no override needed.
    CommandBucket bucket(config);

    constexpr u32 kCount = 10000;
    for (u32 i = 0; i < kCount; ++i)
    {
        auto cmd = MakeSyntheticDrawMeshCommand(1, 1, 0.0f, static_cast<i32>(i));
        cmd.vertexArrayID = 100;
        cmd.renderStateIndex = 0;
        cmd.materialDataIndex = 0;
        cmd.transform = glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<f32>(i) * 0.01f, 0.0f, 0.0f));
        PacketMetadata meta;
        meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 1, 1, i);
        bucket.Submit(cmd, meta, m_Allocator.get());
    }

    EXPECT_EQ(bucket.GetCommandCount(), kCount);

    bucket.BatchCommands(*m_Allocator);

    // Acceptance: 10k identical draws collapse into a single DrawMeshInstanced
    // packet. If this regresses, the auto-batching key matching or the
    // FrameDataBuffer transform allocator has broken.
    ASSERT_EQ(bucket.GetSortedCommands().size(), 1u);
    const auto* packet = bucket.GetSortedCommands()[0];
    ASSERT_EQ(packet->GetCommandType(), CommandType::DrawMeshInstanced);
    auto const* icmd = static_cast<const DrawMeshInstancedCommand*>(packet->GetRawCommandData());
    EXPECT_EQ(icmd->instanceCount, kCount);
    EXPECT_EQ(icmd->transformCount, kCount);
}

// =============================================================================
// Per-Source EntityID + PrevTransform Survive Batch Collapse
// =============================================================================
// Regression: before this test, BatchCommands captured only `transform` per
// instance and dropped each source's `entityID` and `prevTransform`. The
// dispatcher then wrote -1 / aliased prev=current, silently breaking editor
// picking and TAA velocity on every auto-batched draw. The two new
// FrameDataBuffer streams (EntityIDs + the second transform allocation for
// prevs) plug both holes — this test pins the contract end-to-end.

TEST_F(CommandBucketBatchTest, BatchedEntityIDAndPrevTransformSurviveCollapse)
{
    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = true;
    CommandBucket bucket(config);

    constexpr u32 kCount = 8;
    i32 expectedEntityIDs[kCount];
    glm::mat4 expectedPrev[kCount];
    for (u32 i = 0; i < kCount; ++i)
    {
        auto cmd = MakeSyntheticDrawMeshCommand(1, 1, 0.0f, static_cast<i32>(100 + i));
        cmd.vertexArrayID = 100;
        cmd.renderStateIndex = 0;
        cmd.materialDataIndex = 0;
        cmd.transform = glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<f32>(i), 0.0f, 0.0f));
        // Distinct prev-transform per source so aliasing prev=current would be
        // immediately visible.
        cmd.prevTransform = glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<f32>(i) - 0.5f, 0.0f, 0.0f));
        expectedEntityIDs[i] = static_cast<i32>(100 + i);
        expectedPrev[i] = cmd.prevTransform;

        PacketMetadata meta;
        meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 1, 1, i);
        bucket.Submit(cmd, meta, m_Allocator.get());
    }

    bucket.BatchCommands(*m_Allocator);

    ASSERT_EQ(bucket.GetSortedCommands().size(), 1u);
    const auto* packet = bucket.GetSortedCommands()[0];
    ASSERT_EQ(packet->GetCommandType(), CommandType::DrawMeshInstanced);
    auto const* icmd = static_cast<const DrawMeshInstancedCommand*>(packet->GetRawCommandData());
    EXPECT_EQ(icmd->instanceCount, kCount);

    // Both auxiliary streams must be allocated.
    ASSERT_NE(icmd->entityIDBufferOffset, UINT32_MAX) << "EntityID stream must be allocated for batched draws";
    ASSERT_NE(icmd->prevTransformBufferOffset, UINT32_MAX) << "PrevTransform stream must be allocated for batched draws";

    FrameDataBuffer& fb = FrameDataBufferManager::Get();
    const i32* storedIDs = fb.GetEntityIDPtr(icmd->entityIDBufferOffset);
    const glm::mat4* storedPrev = fb.GetTransformPtr(icmd->prevTransformBufferOffset);
    ASSERT_NE(storedIDs, nullptr);
    ASSERT_NE(storedPrev, nullptr);

    for (u32 i = 0; i < kCount; ++i)
    {
        EXPECT_EQ(storedIDs[i], expectedEntityIDs[i])
            << "EntityID " << i << " lost across batch collapse";
        EXPECT_EQ(storedPrev[i], expectedPrev[i])
            << "PrevTransform " << i << " lost across batch collapse";
    }
}

TEST_F(CommandBucketBatchTest, IdentityColorAndCustomSkipParallelStreamAllocation)
{
    // Most scenes don't set per-entity Color / Custom on DrawMeshCommand, so
    // the batcher must NOT allocate those streams in that case — saving the
    // per-frame FrameDataBuffer pressure that the existing entityID stream
    // already pays.
    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = true;
    CommandBucket bucket(config);

    constexpr u32 kCount = 4;
    for (u32 i = 0; i < kCount; ++i)
    {
        auto cmd = MakeSyntheticDrawMeshCommand(1, 1, 0.0f, static_cast<i32>(i));
        cmd.vertexArrayID = 100;
        cmd.renderStateIndex = 0;
        cmd.materialDataIndex = 0;
        // Default color (1,1,1,1) and default custom (0.0) on every source.
        PacketMetadata meta;
        meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 1, 1, i);
        bucket.Submit(cmd, meta, m_Allocator.get());
    }

    bucket.BatchCommands(*m_Allocator);

    ASSERT_EQ(bucket.GetSortedCommands().size(), 1u);
    auto const* icmd = static_cast<const DrawMeshInstancedCommand*>(
        bucket.GetSortedCommands()[0]->GetRawCommandData());
    EXPECT_EQ(icmd->instanceCount, kCount);
    EXPECT_EQ(icmd->colorBufferOffset, UINT32_MAX)
        << "All-identity colors should skip stream allocation";
    EXPECT_EQ(icmd->customBufferOffset, UINT32_MAX)
        << "All-zero customs should skip stream allocation";
}

TEST_F(CommandBucketBatchTest, SameLODBatchesAndDifferentLODsStaySeparate)
{
    // LOD selection runs before DrawMeshCommand is constructed, so
    // `cmd.meshHandle` already names the LOD-resolved mesh. The auto-batcher
    // groups by meshHandle, so same-LOD instances collapse to one draw and
    // different-LOD instances stay separate (one draw per LOD). This pins the
    // current behaviour — relaxing it to a single multi-LOD draw needs
    // glMultiDrawElementsIndirect (see §4 of GPU_INSTANCING_FUTURE_IMPROVEMENTS).
    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = true;
    CommandBucket bucket(config);

    // 6 entities: 4 at LOD-0 (meshHandle 100), 2 at LOD-1 (meshHandle 101).
    constexpr u32 kLOD0Count = 4;
    constexpr u32 kLOD1Count = 2;
    constexpr u32 kLOD0Handle = 100;
    constexpr u32 kLOD1Handle = 101;
    u32 nextEntity = 0;
    for (u32 i = 0; i < kLOD0Count; ++i)
    {
        auto cmd = MakeSyntheticDrawMeshCommand(1, 1, 0.0f, static_cast<i32>(nextEntity));
        cmd.meshHandle = UUID(kLOD0Handle); // LOD-resolved mesh handle differs by LOD level.
        cmd.vertexArrayID = kLOD0Handle;
        cmd.renderStateIndex = 0;
        cmd.materialDataIndex = 0;
        PacketMetadata meta;
        meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, kLOD0Handle, 1, nextEntity);
        bucket.Submit(cmd, meta, m_Allocator.get());
        ++nextEntity;
    }
    for (u32 i = 0; i < kLOD1Count; ++i)
    {
        auto cmd = MakeSyntheticDrawMeshCommand(1, 1, 0.0f, static_cast<i32>(nextEntity));
        cmd.meshHandle = UUID(kLOD1Handle);
        cmd.vertexArrayID = kLOD1Handle;
        cmd.renderStateIndex = 0;
        cmd.materialDataIndex = 0;
        PacketMetadata meta;
        meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, kLOD1Handle, 1, nextEntity);
        bucket.Submit(cmd, meta, m_Allocator.get());
        ++nextEntity;
    }

    bucket.BatchCommands(*m_Allocator);

    // Expect two DrawMeshInstanced packets — one per LOD — each holding
    // their respective instance counts.
    auto const& sorted = bucket.GetSortedCommands();
    u32 lod0InstanceCount = 0;
    u32 lod1InstanceCount = 0;
    u32 instancedPacketCount = 0;
    for (const auto* pkt : sorted)
    {
        if (!pkt || pkt->GetCommandType() != CommandType::DrawMeshInstanced)
            continue;
        auto const* icmd = static_cast<const DrawMeshInstancedCommand*>(pkt->GetRawCommandData());
        ++instancedPacketCount;
        if (icmd->meshHandle == kLOD0Handle)
            lod0InstanceCount = icmd->instanceCount;
        else if (icmd->meshHandle == kLOD1Handle)
            lod1InstanceCount = icmd->instanceCount;
        else
        {
            // No additional handling required.
        }
    }
    EXPECT_EQ(instancedPacketCount, 2u) << "Two LOD levels should produce two instanced packets";
    EXPECT_EQ(lod0InstanceCount, kLOD0Count);
    EXPECT_EQ(lod1InstanceCount, kLOD1Count);
}

TEST_F(CommandBucketBatchTest, NonDefaultColorAndCustomSurviveCollapse)
{
    // When at least one source sets a non-identity Color or non-zero Custom,
    // the batcher must allocate the parallel stream and preserve every
    // source's value — the dispatcher reads these into InstanceData per slot.
    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = true;
    CommandBucket bucket(config);

    constexpr u32 kCount = 4;
    glm::vec4 expectedColors[kCount];
    f32 expectedCustoms[kCount];
    for (u32 i = 0; i < kCount; ++i)
    {
        auto cmd = MakeSyntheticDrawMeshCommand(1, 1, 0.0f, static_cast<i32>(i));
        cmd.vertexArrayID = 100;
        cmd.renderStateIndex = 0;
        cmd.materialDataIndex = 0;
        cmd.color = glm::vec4(0.1f * static_cast<f32>(i), 0.5f, 0.25f, 1.0f);
        cmd.custom = static_cast<f32>(i) * 2.5f;
        expectedColors[i] = cmd.color;
        expectedCustoms[i] = cmd.custom;

        PacketMetadata meta;
        meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 1, 1, i);
        bucket.Submit(cmd, meta, m_Allocator.get());
    }

    bucket.BatchCommands(*m_Allocator);

    ASSERT_EQ(bucket.GetSortedCommands().size(), 1u);
    auto const* icmd = static_cast<const DrawMeshInstancedCommand*>(
        bucket.GetSortedCommands()[0]->GetRawCommandData());
    EXPECT_EQ(icmd->instanceCount, kCount);
    ASSERT_NE(icmd->colorBufferOffset, UINT32_MAX) << "Non-default colors must allocate stream";
    ASSERT_NE(icmd->customBufferOffset, UINT32_MAX) << "Non-default customs must allocate stream";

    FrameDataBuffer& fb = FrameDataBufferManager::Get();
    const glm::vec4* storedColors = fb.GetColorPtr(icmd->colorBufferOffset);
    const f32* storedCustoms = fb.GetCustomPtr(icmd->customBufferOffset);
    ASSERT_NE(storedColors, nullptr);
    ASSERT_NE(storedCustoms, nullptr);

    for (u32 i = 0; i < kCount; ++i)
    {
        EXPECT_EQ(storedColors[i], expectedColors[i])
            << "Color " << i << " lost across batch collapse";
        EXPECT_EQ(storedCustoms[i], expectedCustoms[i])
            << "Custom " << i << " lost across batch collapse";
    }
}
