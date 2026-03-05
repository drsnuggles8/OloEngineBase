#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"
#include "MockRendererAPI.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Commands/CommandAllocator.h"

#include <vector>
#include <algorithm>

using namespace OloEngine;          // NOLINT(google-build-using-namespace)
using namespace OloEngine::Testing; // NOLINT(google-build-using-namespace)

// =============================================================================
// Fixture
// =============================================================================

class FramePipelineTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Allocator = std::make_unique<CommandAllocator>();
        m_MockAPI = std::make_unique<MockRendererAPI>();
    }

    void TearDown() override
    {
        m_MockAPI.reset();
        m_Allocator.reset();
    }

    std::unique_ptr<CommandAllocator> m_Allocator;
    std::unique_ptr<MockRendererAPI> m_MockAPI;
};

// =============================================================================
// Opaque Before Transparent
// =============================================================================

TEST_F(FramePipelineTest, OpaqueBeforeTransparent)
{
    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = false;
    CommandBucket bucket(config);

    // Submit transparent commands
    for (u32 i = 0; i < 5; ++i)
    {
        auto cmd = MakeSyntheticDrawMeshCommand(1, 1, static_cast<f32>(i) * 0.1f, static_cast<i32>(i));
        PacketMetadata meta;
        meta.m_SortKey = MakeSyntheticTransparentKey(0, ViewLayerType::ThreeD, 1, 1, i * 100);
        bucket.Submit(cmd, meta, m_Allocator.get());
    }

    // Submit opaque commands
    for (u32 i = 0; i < 5; ++i)
    {
        auto cmd = MakeSyntheticDrawMeshCommand(2, 2, static_cast<f32>(i) * 0.1f, static_cast<i32>(10 + i));
        PacketMetadata meta;
        meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 2, 2, i * 100);
        bucket.Submit(cmd, meta, m_Allocator.get());
    }

    bucket.SortCommands();
    ASSERT_TRUE(bucket.IsSorted());

    const auto& sorted = bucket.GetSortedCommands();
    ASSERT_EQ(sorted.size(), 10u);

    // Verify all opaque (RenderMode::Opaque) come before transparent (RenderMode::Transparent)
    bool seenTransparent = false;
    for (const auto* pkt : sorted)
    {
        auto mode = pkt->GetMetadata().m_SortKey.GetRenderMode();
        if (mode == RenderMode::Transparent)
        {
            seenTransparent = true;
        }
        else if (mode == RenderMode::Opaque && seenTransparent)
        {
            FAIL() << "Opaque command found after transparent — sort order violation";
        }
    }
}

// =============================================================================
// All Submitted Commands Present After Sort
// =============================================================================

TEST_F(FramePipelineTest, AllSubmittedCommandsPresent)
{
    CommandBucket bucket;

    constexpr u32 N = 50;
    for (u32 i = 0; i < N; ++i)
    {
        auto cmd = MakeSyntheticDrawMeshCommand(i % 5 + 1, i % 3 + 1, static_cast<f32>(i) * 0.02f, static_cast<i32>(i));
        PacketMetadata meta;
        meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, i % 5 + 1, i % 3 + 1, i * 10);
        bucket.Submit(cmd, meta, m_Allocator.get());
    }

    bucket.SortCommands();

    const auto& sorted = bucket.GetSortedCommands();
    EXPECT_EQ(sorted.size(), N) << "Lost commands during sort";
}

// =============================================================================
// Sort Reduces Shader State Changes
// =============================================================================

TEST_F(FramePipelineTest, SortReducesShaderStateChanges)
{
    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = false;
    CommandBucket bucket(config);

    // Submit commands with random shader assignments
    auto rng = MakeTestRNG();
    std::uniform_int_distribution<u32> shaderDist(1, 5);

    std::vector<DrawKey> preKeys;
    for (u32 i = 0; i < 30; ++i)
    {
        u32 shader = shaderDist(rng);
        auto cmd = MakeSyntheticDrawMeshCommand(shader, 1, static_cast<f32>(i) * 0.03f, static_cast<i32>(i));
        PacketMetadata meta;
        meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, shader, 1, i * 10);
        bucket.Submit(cmd, meta, m_Allocator.get());
        preKeys.push_back(meta.m_SortKey);
    }

    u32 changesBefore = CountShaderChanges(preKeys);

    bucket.SortCommands();

    const auto& sorted = bucket.GetSortedCommands();
    std::vector<DrawKey> postKeys;
    for (const auto* pkt : sorted)
    {
        postKeys.push_back(pkt->GetMetadata().m_SortKey);
    }
    u32 changesAfter = CountShaderChanges(postKeys);

    EXPECT_LE(changesAfter, changesBefore)
        << "Sorting should not increase state changes";
    EXPECT_LE(changesAfter, 4u)
        << "With 5 unique shaders, should have at most 4 shader transitions";
}

// =============================================================================
// Isolated Buckets Don't Interfere
// =============================================================================

TEST_F(FramePipelineTest, IsolatedBuckets)
{
    CommandBucket bucketA;
    CommandBucket bucketB;

    for (u32 i = 0; i < 10; ++i)
    {
        auto cmdA = MakeSyntheticDrawMeshCommand(1, 1, 0.5f, static_cast<i32>(i));
        bucketA.Submit(cmdA, {}, m_Allocator.get());

        auto cmdB = MakeSyntheticDrawMeshCommand(2, 2, 0.5f, static_cast<i32>(100 + i));
        bucketB.Submit(cmdB, {}, m_Allocator.get());
    }

    EXPECT_EQ(bucketA.GetCommandCount(), 10u);
    EXPECT_EQ(bucketB.GetCommandCount(), 10u);

    bucketA.SortCommands();
    bucketB.SortCommands();

    // Verify bucket A only has shader 1, bucket B only has shader 2
    for (const auto* pkt : bucketA.GetSortedCommands())
    {
        const auto* data = pkt->GetCommandData<DrawMeshCommand>();
        EXPECT_EQ(data->shaderHandle, static_cast<AssetHandle>(1));
        EXPECT_EQ(data->materialDataIndex, static_cast<u16>(1));
    }
    for (const auto* pkt : bucketB.GetSortedCommands())
    {
        const auto* data = pkt->GetCommandData<DrawMeshCommand>();
        EXPECT_EQ(data->shaderHandle, static_cast<AssetHandle>(2));
        EXPECT_EQ(data->materialDataIndex, static_cast<u16>(2));
    }
}

// =============================================================================
// Multi-Frame Reset Cycle
// =============================================================================

TEST_F(FramePipelineTest, MultiFrameResetCycle)
{
    CommandBucket bucket;

    for (u32 frame = 0; frame < 10u; ++frame)
    {
        bucket.Clear();
        EXPECT_EQ(bucket.GetCommandCount(), 0u);

        for (u32 i = 0; i < 20; ++i)
        {
            auto cmd = MakeSyntheticDrawMeshCommand(1, 1, 0.5f, static_cast<i32>(i));
            bucket.Submit(cmd, {}, m_Allocator.get());
        }

        EXPECT_EQ(bucket.GetCommandCount(), 20u);
        bucket.SortCommands();
        EXPECT_TRUE(bucket.IsSorted());
        EXPECT_EQ(bucket.GetSortedCommands().size(), 20u);
    }
}

// =============================================================================
// ViewLayer Sorting Priority
// =============================================================================

TEST_F(FramePipelineTest, ViewLayerSortingPriority)
{
    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = false;
    CommandBucket bucket(config);

    // Submit commands from different view layers in reverse priority
    ViewLayerType layers[] = { ViewLayerType::UI, ViewLayerType::TwoD, ViewLayerType::Skybox, ViewLayerType::ThreeD };

    for (auto layer : layers)
    {
        auto cmd = MakeSyntheticDrawMeshCommand(1, 1, 0.5f);
        PacketMetadata meta;
        meta.m_SortKey = MakeSyntheticOpaqueKey(0, layer, 1, 1, 100);
        bucket.Submit(cmd, meta, m_Allocator.get());
    }

    bucket.SortCommands();

    const auto& sorted = bucket.GetSortedCommands();
    ASSERT_EQ(sorted.size(), 4u);

    // Higher view layer values have higher sort key bits — they sort first
    // Verify the ordering respects DrawKey semantics
    std::vector<DrawKey> keys;
    for (const auto* pkt : sorted)
    {
        keys.push_back(pkt->GetMetadata().m_SortKey);
    }
    ExpectCommandOrder(keys);
}
