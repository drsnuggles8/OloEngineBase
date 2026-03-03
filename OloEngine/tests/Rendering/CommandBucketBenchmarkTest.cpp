#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Commands/CommandAllocator.h"

#include <chrono>
#include <cstdlib>
#include <numeric>
#include <random>
#include <string>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

// =============================================================================
// Utilities
// =============================================================================

/// Returns true if the OLOENGINE_BENCH_ASSERT environment variable is set.
static bool BenchAssertEnabled()
{
    const char* env = std::getenv("OLOENGINE_BENCH_ASSERT"); // NOLINT(concurrency-mt-unsafe)
    return env && std::string(env) == "1";
}

/// Populate a bucket with N random DrawMeshCommands using different shaders/materials.
static void PopulateBucket(CommandBucket& bucket, CommandAllocator& allocator, u32 count, std::mt19937& rng)
{
    std::uniform_int_distribution<u32> shaderDist(1, 64);
    std::uniform_int_distribution<u32> matDist(1, 256);
    std::uniform_int_distribution<u32> depthDist(0, 0xFFFFFF);

    for (u32 i = 0; i < count; ++i)
    {
        u32 shader = shaderDist(rng);
        u32 mat = matDist(rng);
        u32 depth = depthDist(rng);

        auto cmd = MakeSyntheticDrawMeshCommand(shader, mat, static_cast<f32>(depth) / static_cast<f32>(0xFFFFFF), static_cast<i32>(i));
        PacketMetadata meta;
        meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, shader, mat, depth);
        bucket.Submit(cmd, meta, &allocator);
    }
}

using Clock = std::chrono::high_resolution_clock;

// =============================================================================
// Sort Scaling (1K, 10K, 100K)
// =============================================================================

class SortScalingTest : public ::testing::TestWithParam<u32>
{
};

TEST_P(SortScalingTest, SortCompletesAndOrderIsValid)
{
    const u32 N = GetParam();
    CommandAllocator allocator;
    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = false;
    CommandBucket bucket(config);

    auto rng = GetTestRNG();
    PopulateBucket(bucket, allocator, N, rng);
    ASSERT_EQ(bucket.GetCommandCount(), N);

    auto start = Clock::now();
    bucket.SortCommands();
    auto end = Clock::now();

    ASSERT_TRUE(bucket.IsSorted());
    ASSERT_EQ(bucket.GetSortedCommands().size(), N);

    // Verify order
    const auto& sorted = bucket.GetSortedCommands();
    std::vector<DrawKey> keys;
    keys.reserve(sorted.size());
    for (const auto* pkt : sorted)
    {
        keys.push_back(pkt->GetMetadata().m_SortKey);
    }
    ExpectCommandOrder(keys);

    auto durationMs = std::chrono::duration<double, std::milli>(end - start).count();
    std::cout << "[BENCHMARK] Sort " << N << " commands: " << durationMs << " ms\n";

    if (BenchAssertEnabled())
    {
        // Generous upper bounds — these are correctness guards, not perf targets
        if (N <= 1000)
            EXPECT_LT(durationMs, 50.0) << "Sort of 1K commands too slow";
        else if (N <= 10000)
            EXPECT_LT(durationMs, 500.0) << "Sort of 10K commands too slow";
        else
            EXPECT_LT(durationMs, 5000.0) << "Sort of 100K commands too slow";
    }
}

INSTANTIATE_TEST_SUITE_P(
    SortScaling,
    SortScalingTest,
    ::testing::Values(1000u, 10000u, 100000u),
    [](const ::testing::TestParamInfo<u32>& info)
    {
        return "N" + std::to_string(info.param);
    });

// =============================================================================
// Parallel Submit Scaling
// =============================================================================

TEST(CommandBucketBenchmark, ParallelSubmitAndMerge)
{
    constexpr u32 N = 10000;
    constexpr u32 numWorkers = 4;
    constexpr u32 perWorker = N / numWorkers;

    CommandAllocator allocator;
    CommandBucketConfig config;
    config.InitialCapacity = N + 1024;
    config.EnableSorting = true;
    config.EnableBatching = false;
    CommandBucket bucket(config);
    bucket.SetAllocator(&allocator);
    bucket.PrepareForParallelSubmission();

    auto rng = GetTestRNG();
    std::uniform_int_distribution<u32> shaderDist(1, 32);
    std::uniform_int_distribution<u32> depthDist(0, 0xFFFFFF);

    auto start = Clock::now();

    for (u32 w = 0; w < numWorkers; ++w)
    {
        for (u32 i = 0; i < perWorker; ++i)
        {
            u32 shader = shaderDist(rng);
            u32 depth = depthDist(rng);
            auto cmd = MakeSyntheticDrawMeshCommand(shader, 1, static_cast<f32>(depth) / static_cast<f32>(0xFFFFFF));

            PacketMetadata meta;
            meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, shader, 1, depth);
            auto* packet = allocator.CreateCommandPacket(cmd, meta);
            ASSERT_NE(packet, nullptr);
            bucket.SubmitPacketParallel(packet, w);
        }
    }

    bucket.MergeThreadLocalCommands();
    auto mergeTime = Clock::now();

    bucket.SortCommands();
    auto sortTime = Clock::now();

    auto mergeMs = std::chrono::duration<double, std::milli>(mergeTime - start).count();
    auto sortMs = std::chrono::duration<double, std::milli>(sortTime - mergeTime).count();

    std::cout << "[BENCHMARK] Parallel submit+merge " << N << " commands: " << mergeMs << " ms\n";
    std::cout << "[BENCHMARK] Post-merge sort: " << sortMs << " ms\n";

    EXPECT_EQ(bucket.GetCommandCount(), N);
    EXPECT_TRUE(bucket.IsSorted());

    const auto& sorted = bucket.GetSortedCommands();
    std::vector<DrawKey> keys;
    for (const auto* pkt : sorted)
    {
        keys.push_back(pkt->GetMetadata().m_SortKey);
    }
    ExpectCommandOrder(keys);
}

// =============================================================================
// Allocator Reset Stability Over 1000 Frames
// =============================================================================

TEST(CommandBucketBenchmark, AllocatorResetStability)
{
    CommandAllocator allocator;
    constexpr u32 framesCount = 1000;
    constexpr u32 commandsPerFrame = 100;

    auto rng = GetTestRNG();

    auto start = Clock::now();

    for (u32 frame = 0; frame < framesCount; ++frame)
    {
        allocator.Reset();

        CommandBucket bucket;
        PopulateBucket(bucket, allocator, commandsPerFrame, rng);
        bucket.SortCommands();

        ASSERT_EQ(bucket.GetSortedCommands().size(), commandsPerFrame)
            << "Frame " << frame << " lost commands";
    }

    auto end = Clock::now();
    auto totalMs = std::chrono::duration<double, std::milli>(end - start).count();

    std::cout << "[BENCHMARK] " << framesCount << " frames x " << commandsPerFrame
              << " commands: " << totalMs << " ms total ("
              << (totalMs / framesCount) << " ms/frame)\n";

    if (BenchAssertEnabled())
    {
        EXPECT_LT(totalMs / framesCount, 10.0) << "Per-frame budget exceeded";
    }
}

// =============================================================================
// Memory Pressure — Allocate Many Large Commands
// =============================================================================

TEST(CommandBucketBenchmark, LargeCommandMemoryPressure)
{
    CommandAllocator allocator;
    constexpr u32 N = 5000;

    auto start = Clock::now();

    for (u32 i = 0; i < N; ++i)
    {
        auto cmd = MakeSyntheticDrawMeshCommand(i % 64 + 1, i % 128 + 1, static_cast<f32>(i) / static_cast<f32>(N));
        auto* packet = allocator.CreateCommandPacket(cmd);
        ASSERT_NE(packet, nullptr) << "Allocation failed at " << i;
    }

    auto end = Clock::now();
    auto durationMs = std::chrono::duration<double, std::milli>(end - start).count();

    std::cout << "[BENCHMARK] " << N << " DrawMeshCommand allocations (" << sizeof(DrawMeshCommand)
              << " bytes each): " << durationMs << " ms\n";

    EXPECT_EQ(allocator.GetAllocationCount(), N);
}
