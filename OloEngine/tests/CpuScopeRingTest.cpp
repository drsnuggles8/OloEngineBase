// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include "OloEngine/Core/CpuScopeRing.h"
#include "OloEngine/Core/PerformanceProfiler.h"

#include <gtest/gtest.h>
#include <array>
#include <limits>
#include <thread>

using OloEngine::CpuScopeRing;

namespace
{
    void RecordFrame(CpuScopeRing& ring, u64 startNs, u64 durationNs)
    {
        ring.BeginFrame(startNs);
        const auto token = ring.BeginScope("work", 0, 7, startNs);
        ring.EndScope(token, startNs + durationNs);
        ring.EndFrame(startNs + durationNs, {});
    }
} // namespace

TEST(CpuScopeRingTest, WrapKeepsChronologicalCompleteFramesAndReportsLoss)
{
    CpuScopeRing ring;
    ring.Arm(3, 2);
    for (u64 i = 0; i < 8; ++i)
        RecordFrame(ring, i * 1000000, 1000000);
    const auto capture = ring.Capture();
    ASSERT_EQ(capture.Frames.Num(), 3);
    EXPECT_EQ(capture.Frames[0].Id, 6);
    EXPECT_EQ(capture.Frames[2].Id, 8);
    EXPECT_EQ(capture.FramesOverwritten, 5);
    EXPECT_EQ(capture.Events.Num(), 3);
    EXPECT_DOUBLE_EQ(capture.Events[2].DurationMs, 1);
}

TEST(CpuScopeRingTest, ThresholdContextClipsAndDeduplicates)
{
    CpuScopeRing ring;
    ring.Arm(8, 1);
    for (u64 i = 0; i < 7; ++i)
        RecordFrame(ring, i * 10000000, (i == 0 || i == 2 || i == 6) ? 5000000 : 1000000);
    const auto capture = ring.Capture(5, 1);
    ASSERT_EQ(capture.Frames.Num(), 6);
    EXPECT_EQ(capture.Frames[0].Id, 1);
    EXPECT_EQ(capture.Frames[3].Id, 4);
    EXPECT_EQ(capture.Frames[4].Id, 6);
    EXPECT_EQ(capture.Frames[5].Id, 7);
    EXPECT_TRUE(ring.Capture(6, 1).Frames.IsEmpty());
    EXPECT_THROW((void)ring.Capture(std::numeric_limits<f64>::quiet_NaN()), std::invalid_argument);
}

TEST(CpuScopeRingTest, DisarmFreezesAndRearmRejectsOldTokens)
{
    CpuScopeRing ring;
    ring.Arm(2, 4);
    RecordFrame(ring, 0, 1000000);
    ring.BeginFrame(2000000);
    const auto old = ring.BeginScope("unfinished", 0, 1, 2000000);
    ring.Disarm();
    ring.EndScope(old, 9000000);
    ring.EndFrame(10000000, {});
    EXPECT_EQ(ring.Capture().Frames.Num(), 1);
    EXPECT_FALSE(ring.Capture().Armed);
    EXPECT_EQ(ring.Capture().PartialFramesDiscarded, 1);
    ring.Arm(2, 4);
    ring.BeginFrame(0);
    const auto fresh = ring.BeginScope("fresh", 0, 1, 0);
    ring.EndScope(old, 999000000);
    ring.EndScope(fresh, 1000000);
    ring.EndFrame(1000000, {});
    ASSERT_EQ(ring.Capture().Events.Num(), 1);
    EXPECT_DOUBLE_EQ(ring.Capture().Events[0].DurationMs, 1);
}

TEST(CpuScopeRingTest, BoundedEventsOwnNamesAndExposeIncompleteAndLateScopes)
{
    CpuScopeRing ring;
    ring.Arm(2, 2);
    ring.BeginFrame(0);
    char temporary[] = "parent";
    const auto parent = ring.BeginScope(temporary, 0, 12, 0);
    const auto child = ring.BeginScope("child", 1, 12, 100000);
    temporary[0] = 'X';
    EXPECT_EQ(ring.BeginScope("overflow", 2, 12, 200000).FrameId, 0);
    ring.EndScope(child, 300000);
    ring.EndFrame(1000000, {});
    auto capture = ring.Capture();
    EXPECT_STREQ(capture.Events[0].Name.data(), "parent");
    EXPECT_FALSE(capture.Events[0].Complete);
    EXPECT_EQ(capture.Events[1].Depth, 1);
    EXPECT_EQ(capture.Events[1].Thread, 12);
    EXPECT_DOUBLE_EQ(capture.Events[1].StartMs, 0.1);
    EXPECT_DOUBLE_EQ(capture.Events[1].DurationMs, 0.2);
    EXPECT_EQ(capture.Frames[0].DroppedEvents, 1);
    RecordFrame(ring, 1000000, 1000000);
    RecordFrame(ring, 2000000, 1000000);
    ring.EndScope(parent, 3000000);
    EXPECT_EQ(ring.Capture().LateCompletions, 1);
}

TEST(CpuScopeRingTest, WorkerScopesShareAFrameWithoutGrowingStorage)
{
    CpuScopeRing ring;
    ring.Arm(2, 128);
    const u64 bytes = ring.Capture(0, 0, false).RetainedBytes;
    for (int frame = 0; frame < 100; ++frame)
    {
        ring.BeginFrame(0);
        std::thread worker([&ring]()
                           {
            for (int i = 0; i < 100; ++i)
            {
                const auto token = ring.BeginScope("worker", 0, 42, 0);
                ring.EndScope(token, 1000000);
            } });
        worker.join();
        ring.EndFrame(1000000, {});
    }
    const auto capture = ring.Capture();
    EXPECT_EQ(capture.RetainedBytes, bytes);
    ASSERT_EQ(capture.Frames.Num(), 2);
    EXPECT_EQ(capture.Frames[1].EventCount, 100);
    EXPECT_EQ(capture.Frames[1].DroppedEvents, 0);
    EXPECT_THROW(ring.Arm(2048, 4096), std::invalid_argument);
    EXPECT_EQ(ring.Capture().FramesWritten, 100); // Rejected arm preserves the recording.
}

TEST(CpuScopeRingTest, CompletionAcrossFrameBoundaryPreservesItsOrigin)
{
    CpuScopeRing ring;
    ring.Arm(3, 1);
    ring.BeginFrame(0);
    const auto token = ring.BeginScope("worker", 0, 10, 0);
    ring.EndFrame(1000000, {});
    ring.BeginFrame(1000000);
    ring.EndScope(token, 2000000);
    ring.EndFrame(2000000, {});
    const auto capture = ring.Capture();
    EXPECT_EQ(capture.Frames[0].Id, 1);
    EXPECT_EQ(capture.Frames[0].EventCount, 1);
    EXPECT_EQ(capture.Frames[1].EventCount, 0);
    EXPECT_TRUE(capture.Events[0].Complete);
    EXPECT_DOUBLE_EQ(capture.Events[0].DurationMs, 2);
}

TEST(CpuScopeRingTest, ProductionTimersRecordNestedEventsAndKeepTheAggregate)
{
    OloEngine::PerformanceProfiler profiler;
    auto& ring = profiler.GetCpuScopeRing();
    ring.Arm(2, 8);
    ring.BeginFrame(CpuScopeRing::NowNs());
    {
        OloEngine::ScopedPerformanceTimer parent("parent", &profiler);
        {
            OloEngine::ScopedPerformanceTimer child("child", &profiler);
        }
    }
    ring.EndFrame(CpuScopeRing::NowNs(), {});
    profiler.EndFrame();
    const auto capture = ring.Capture();
    ASSERT_EQ(capture.Events.Num(), 2);
    EXPECT_EQ(capture.Events[0].Depth, 0);
    EXPECT_EQ(capture.Events[1].Depth, 1);
    EXPECT_EQ(capture.Events[0].Thread, capture.Events[1].Thread);
    EXPECT_GE(capture.Events[0].DurationMs, capture.Events[1].DurationMs);
    EXPECT_EQ(profiler.GetPreviousFrameData().at("parent").Samples, 1);
    EXPECT_EQ(profiler.GetPreviousFrameData().at("child").Samples, 1);
    ring.Disarm();
    {
        OloEngine::ScopedPerformanceTimer next("disarmed", &profiler);
    }
    EXPECT_EQ(ring.Capture().Events.Num(), 2);
}

TEST(CpuScopeRingTest, LongNamesAreTerminatedAndMarkedAsTruncated)
{
    CpuScopeRing ring;
    ring.Arm(2, 1);
    ring.BeginFrame(0);
    std::array<char, 256> name;
    name.fill('x');
    const auto token = ring.BeginScope(std::string_view(name.data(), name.size()), 0, 1, 0);
    ring.EndScope(token, 1000000);
    ring.EndFrame(1000000, {});
    const auto capture = ring.Capture();
    ASSERT_EQ(capture.Events.Num(), 1);
    EXPECT_TRUE(capture.Events[0].NameTruncated);
    EXPECT_EQ(capture.Events[0].Name.back(), '\0');
    EXPECT_EQ(capture.Events[0].Name.front(), 'x');
}

TEST(CpuScopeRingTest, FilteredHitchKeepsGpuResolutionOutsideItsContext)
{
    CpuScopeRing ring;
    ring.Arm(4, 1);
    ring.BeginFrame(0);
    ring.EndFrame(500000000, { .GpuIssueFrameId = 7, .RendererFrameAdvanced = true, .GpuFrameAdvanced = true });
    ring.BeginFrame(500000000);
    ring.EndFrame(501000000, { .GpuIssueFrameId = 7 }); // A skipped loop is not another issue.
    ring.BeginFrame(501000000);
    ring.EndFrame(502000000, { .GpuMs = 4.5, .GpuStatus = OloEngine::GpuTimingStatus::Valid, .GpuFrameId = 7, .GpuIssueFrameId = 8, .RendererFrameAdvanced = true, .GpuFrameAdvanced = true });
    const auto filtered = ring.Capture(300);
    ASSERT_EQ(filtered.Frames.Num(), 1);
    EXPECT_EQ(filtered.Frames[0].SameFrameGpu.Status, OloEngine::GpuTimingStatus::Valid);
    EXPECT_DOUBLE_EQ(filtered.Frames[0].SameFrameGpu.GpuMs, 4.5);
    EXPECT_EQ(filtered.Frames[0].Render.GpuStatus, OloEngine::GpuTimingStatus::Unavailable);
    const auto all = ring.Capture();
    EXPECT_EQ(all.Frames[1].SameFrameGpu.Status, OloEngine::GpuTimingStatus::Unavailable);
    EXPECT_EQ(all.Frames[2].SameFrameGpu.Status, OloEngine::GpuTimingStatus::Pending);
}

TEST(CpuScopeRingTest, WrappedGpuOriginDoesNotResolveAnUnrelatedFrame)
{
    CpuScopeRing ring;
    ring.Arm(2, 1);
    for (u64 id = 1; id <= 3; ++id)
    {
        ring.BeginFrame(id * 1000000);
        ring.EndFrame((id + 1) * 1000000, { .GpuMs = 9, .GpuStatus = OloEngine::GpuTimingStatus::Valid, .GpuFrameId = 1, .GpuIssueFrameId = id, .RendererFrameAdvanced = true, .GpuFrameAdvanced = true });
    }
    const auto capture = ring.Capture();
    ASSERT_EQ(capture.Frames.Num(), 2);
    EXPECT_EQ(capture.Frames[0].Id, 2);
    EXPECT_EQ(capture.Frames[0].SameFrameGpu.Status, OloEngine::GpuTimingStatus::Pending);
    EXPECT_EQ(capture.Frames[1].SameFrameGpu.Status, OloEngine::GpuTimingStatus::Pending);
}

TEST(CpuScopeRingTest, ResolvedInvalidGpuSamplePreservesItsStatus)
{
    CpuScopeRing ring;
    ring.Arm(2, 1);
    ring.BeginFrame(0);
    ring.EndFrame(500000000, { .GpuIssueFrameId = 7, .RendererFrameAdvanced = true, .GpuFrameAdvanced = true });
    ring.BeginFrame(500000000);
    ring.EndFrame(501000000, { .GpuStatus = OloEngine::GpuTimingStatus::OutOfOrder, .GpuFrameId = 7, .GpuIssueFrameId = 8 });
    const auto capture = ring.Capture(300);
    ASSERT_EQ(capture.Frames.Num(), 1);
    EXPECT_EQ(capture.Frames[0].SameFrameGpu.Status, OloEngine::GpuTimingStatus::OutOfOrder);
}

TEST(CpuScopeRingTest, GpuCounterResetSeparatesIdentitySequences)
{
    CpuScopeRing ring;
    ring.Arm(3, 1);
    ring.BeginFrame(0);
    ring.EndFrame(500000000, { .GpuIssueFrameId = 7, .RendererFrameAdvanced = true, .GpuFrameAdvanced = true });
    ring.BeginFrame(500000000);
    ring.EndFrame(501000000, { .GpuIssueFrameId = 0 });
    ring.BeginFrame(501000000);
    ring.EndFrame(502000000, { .GpuMs = 99, .GpuStatus = OloEngine::GpuTimingStatus::Valid, .GpuFrameId = 7, .GpuIssueFrameId = 8 });
    const auto capture = ring.Capture(300);
    ASSERT_EQ(capture.Frames.Num(), 1);
    EXPECT_EQ(capture.Frames[0].SameFrameGpu.Status, OloEngine::GpuTimingStatus::Pending);
}
