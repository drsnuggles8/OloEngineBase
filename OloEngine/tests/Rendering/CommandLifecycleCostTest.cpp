// OLO_TEST_LAYER: plumbing
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "FrameDataBufferFixture.h"
#include "MockRendererAPI.h"
#include "RenderingTestUtils.h"
#include "OloEngine/Renderer/Commands/CommandAllocator.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Commands/CommandLifecycle.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>
#include <vector>

// Hot-path cost of the command lifecycle (issue #1335, AC4): allocation,
// sort, batch and replay, per phase, on one fixed synthetic frame. The numbers
// are printed as `[LIFECYCLE-COST]` lines so a before/after pair can be read
// off two runs of the same binary target; there is no timing assertion,
// because a wall-clock bound on a shared box measures the box.
//
// Replay uses a no-op dispatch function per packet, so what it times is the
// bucket's own walk (and, since #1335, the freeze and checks it performs
// around the walk) rather than the dispatcher's GL work. It is timed twice:
// with lifecycle validation OFF, the Release default and the number to
// compare against the pre-#1335 baseline, and ON, the Debug / test-suite
// mode that digests every packet. The test binary turns validation on
// globally, so both are set explicitly here.

using namespace OloEngine;          // NOLINT(google-build-using-namespace) — test file
using namespace OloEngine::Testing; // NOLINT(google-build-using-namespace) — test file

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr u32 kPackets = 10000;
    constexpr u32 kRepetitions = 25;
    // 100 geometry/material groups of 100 draws: the batcher collapses each
    // group into one instanced packet, which is the shape of a large scene of
    // repeated props rather than a best or worst case.
    constexpr u32 kGroups = 100;

    void NoopDispatch(const void* /*data*/, RendererAPI& /*api*/) {}

    struct PhaseSamples
    {
        std::vector<f64> Ms;

        void Add(Clock::time_point start)
        {
            Ms.push_back(std::chrono::duration<f64, std::milli>(Clock::now() - start).count());
        }

        [[nodiscard]] f64 Median()
        {
            std::ranges::sort(Ms);
            return Ms[Ms.size() / 2];
        }

        [[nodiscard]] f64 Min() const
        {
            return *std::ranges::min_element(Ms);
        }
    };

    void Report(const char* phase, PhaseSamples& samples)
    {
        const f64 median = samples.Median();
        std::cout << "[LIFECYCLE-COST] " << phase << " n=" << kPackets << " reps=" << samples.Ms.size()
                  << " median_ms=" << median << " min_ms=" << samples.Min()
                  << " ns_per_packet=" << (median * 1.0e6 / static_cast<f64>(kPackets)) << "\n";
    }

    void SubmitFrame(CommandBucket& bucket, CommandAllocator& allocator, bool noopDispatch)
    {
        std::mt19937 rng(1335u);
        std::uniform_int_distribution<u32> depthDist(0, 0xFFFFFF);
        for (u32 i = 0; i < kPackets; ++i)
        {
            const u32 group = i % kGroups;
            auto cmd = MakeSyntheticDrawMeshCommand(1u + group % 8u, 1u + group, 0.5f, static_cast<i32>(i));
            cmd.vertexArrayID = TestHandle(100u + group);
            cmd.transform = glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<f32>(i), 0.0f, 0.0f));
            cmd.prevTransform = cmd.transform;
            PacketMetadata meta;
            meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 1u + group % 8u, 1u + group, depthDist(rng));
            CommandPacket* packet = bucket.Submit(cmd, meta, &allocator);
            if (noopDispatch && packet)
                packet->SetDispatchFunction(&NoopDispatch);
        }
    }
} // namespace

class CommandLifecycleCost : public FrameDataBufferFixture
{
};

TEST_F(CommandLifecycleCost, PerPhaseHotPathCost)
{
    PhaseSamples submit;
    PhaseSamples sort;
    PhaseSamples batch;
    PhaseSamples replay;
    PhaseSamples replayValidated;
    sizet bytesPerFrame = 0;
    // Restored on every exit, including an ASSERT's early return: the rest of
    // the suite runs with validation on.
    struct RestoreValidation
    {
        bool Was = CommandLifecycle::IsValidationEnabled();
        ~RestoreValidation()
        {
            CommandLifecycle::SetValidationEnabled(Was);
        }
    } restoreValidation;

    for (u32 rep = 0; rep < kRepetitions; ++rep)
    {
        // Submission: the allocation hot path, packet construction included.
        {
            FrameDataBufferManager::Get().Reset();
            CommandAllocator allocator;
            CommandBucketConfig config;
            config.EnableBatching = false;
            CommandBucket bucket(config);
            bucket.SetAllocator(&allocator);

            const auto start = Clock::now();
            SubmitFrame(bucket, allocator, true);
            submit.Add(start);
            ASSERT_EQ(bucket.GetCommandCount(), kPackets);
            bytesPerFrame = allocator.GetTotalAllocated();

            // Sort alone, batching off: the radix sort over the flat arrays.
            const auto sortStart = Clock::now();
            bucket.SortCommands();
            sort.Add(sortStart);
            ASSERT_TRUE(bucket.IsSorted());

            // Replay of the sorted, unbatched bucket through a no-op
            // dispatcher: the per-packet walk plus whatever the bucket does
            // before it walks.
            MockRendererAPI api;
            CommandLifecycle::SetValidationEnabled(false);
            const auto replayStart = Clock::now();
            bucket.Execute(api);
            replay.Add(replayStart);
            ASSERT_EQ(bucket.GetStatistics().DrawCalls, kPackets);
        }

        // The same replay with validation on, on a fresh identical frame so
        // the first-replay freeze (which records the digests) is included.
        {
            FrameDataBufferManager::Get().Reset();
            CommandAllocator allocator;
            CommandBucketConfig config;
            config.EnableBatching = false;
            CommandBucket bucket(config);
            bucket.SetAllocator(&allocator);
            SubmitFrame(bucket, allocator, true);
            bucket.SortCommands();
            MockRendererAPI api;
            CommandLifecycle::SetValidationEnabled(true);
            const auto replayStart = Clock::now();
            bucket.Execute(api);
            replayValidated.Add(replayStart);
            ASSERT_EQ(bucket.GetStatistics().DrawCalls, kPackets);
        }

        // Batching (which re-sorts what it produces) on a fresh frame.
        {
            FrameDataBufferManager::Get().Reset();
            CommandAllocator allocator;
            CommandBucket bucket;
            bucket.SetAllocator(&allocator);
            SubmitFrame(bucket, allocator, false);

            const auto start = Clock::now();
            bucket.BatchCommands(allocator);
            batch.Add(start);
            ASSERT_EQ(bucket.GetCommandCount(), kGroups);
        }
    }

    std::cout << "[LIFECYCLE-COST] allocator_bytes_per_frame=" << bytesPerFrame
              << " bytes_per_packet=" << (bytesPerFrame / kPackets)
              << " sizeof_CommandPacket=" << sizeof(CommandPacket) << "\n";
    Report("submit", submit);
    Report("sort", sort);
    Report("batch", batch);
    Report("replay", replay);
    Report("replay_validated", replayValidated);
}
