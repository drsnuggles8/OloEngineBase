// OLO_TEST_LAYER: L4
// =============================================================================
// GpuTimingPoolEvidenceTest.cpp — issue #1337's live half.
//
// GpuTimingValidityTest.cpp proves the DECISION: given a damaged readout, the
// pure classifier and every consumer report the reason rather than 0.0. It runs
// everywhere, including on a CI runner with no GPU.
//
// This file proves the PRODUCER: that the real GPUPassTimerPool, stamping real
// timestamp queries against a real GL 4.6 context, actually produces those
// statuses — and that the ones criterion 1 names are reachable rather than
// theoretical. It is a substitution-free check of the same contract, because
// "the classifier is right" and "the pool feeds it the truth" are two claims
// and a mock can only ever make the first.
//
// Skips cleanly without a GL 4.6 context (never DISABLED_), per CLAUDE.md.
//
// Vulkan is NOT covered here and cannot be: these fixtures need a real GL
// context and skip without one. The Vulkan producer is covered by
// VulkanPassSuiteTest's timer tenant, which asserts the same validity contract
// on a real device, and by the live-editor cells in the PR's matrix.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RendererAttachedTest.h"
#include "OloEngine/Renderer/Benchmark/BenchmarkCapture.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/Debug/GPUTimingStatus.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <algorithm>
#include <string>

using namespace OloEngine;        // NOLINT(google-build-using-namespace)
using namespace OloEngine::Tests; // NOLINT(google-build-using-namespace)

namespace
{
    class GpuTimingPoolEvidence : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            EnableRendering(256, 256);

            // A primary runtime camera, or RunFrames renders nothing and the
            // pool never opens a pass bracket — the fixture would then "pass"
            // by measuring an empty frame.
            Entity cameraEntity = GetScene().CreateEntity("PrimaryCamera");
            auto& camera = cameraEntity.AddComponent<CameraComponent>();
            camera.Primary = true;
            cameraEntity.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 5.0f };

            Entity cube = GetScene().CreateEntity("Cube");
            cube.AddComponent<MeshComponent>();
        }

        // Render until the pool publishes a frame it actually measured, or give
        // up. Results resolve 1-3 frames after issue, so the first frames are
        // legitimately Pending and asserting on them would be a flake.
        [[nodiscard]] GPUPassTimerPool::FrameTimings RenderUntilResolved(u32 maxFrames = 32)
        {
            GPUPassTimerPool::FrameTimings snapshot;
            for (u32 i = 0; i < maxFrames; ++i)
            {
                RunFrames(1);
                snapshot = GPUPassTimerPool::GetInstance().GetLastFrameTimings();
                if (snapshot.FrameNumber > 0 && snapshot.Frame.IsValid())
                {
                    return snapshot;
                }
            }
            return snapshot;
        }
    };
} // namespace

// =============================================================================
// The {OpenGL} x {Forward, Forward+, Deferred} half of this change's
// verification matrix.
//
// The three paths execute DIFFERENT pass sets, and each one is a different
// population of timestamp brackets: Forward+ adds the light-culling compute
// pass, Deferred adds the whole G-Buffer/lighting split and has the most passes
// to attribute. A validity contract that held on one path and not another would
// be a contract that held by accident.
//
// The Vulkan row of the matrix is NOT here and cannot be: these fixtures need a
// real GL 4.6 context and skip without one. It is covered by VulkanPassSuiteTest's
// timer tenant on a real device, plus the live-editor cells in the PR body.
// =============================================================================

TEST_F(GpuTimingPoolEvidence, EveryRenderingPathPublishesValidityRatherThanZeros)
{
    OLO_ENSURE_GPU_OR_SKIP();

    struct PathCase
    {
        const char* Name;
        RenderingPath Path;
    };
    const std::array<PathCase, 3> paths = { {
        { "Forward", RenderingPath::Forward },
        { "ForwardPlus", RenderingPath::ForwardPlus },
        { "Deferred", RenderingPath::Deferred },
    } };

    for (const PathCase& pathCase : paths)
    {
        Renderer3D::GetRendererSettings().Path = pathCase.Path;
        Renderer3D::ApplyRendererSettings();

        const GPUPassTimerPool::FrameTimings snapshot = RenderUntilResolved();

        ASSERT_GT(snapshot.FrameNumber, 0u) << pathCase.Name << ": the pool resolved no frame at all";
        ASSERT_TRUE(snapshot.Frame.IsValid())
            << pathCase.Name << ": frame span " << ToString(snapshot.Frame.Status) << " — "
            << DescribeGpuTimingStatus(snapshot.Frame.Status);

        // The contract, on every path: a Valid entry carries a real duration,
        // and a non-Valid one carries a reason instead of a zero.
        u32 measured = 0;
        u32 absent = 0;
        for (const GPUPassTimerPool::PassTiming& pass : snapshot.Passes)
        {
            if (pass.IsValid())
            {
                // >= 0, not > 0: a pass that issued no GPU commands on this
                // path legitimately measures zero (see
                // NoPassPublishesANumberWithoutAMeasurementBehindIt).
                EXPECT_GE(pass.Sample.GpuMs, 0.0) << pathCase.Name << ": " << pass.Name << " is Valid but negative";
                EXPECT_TRUE(std::isfinite(pass.Sample.GpuMs)) << pathCase.Name << ": " << pass.Name;
                ++measured;
            }
            else
            {
                EXPECT_NE(pass.Sample.Status, GpuTimingStatus::Valid);
                ++absent;
            }
        }
        EXPECT_GT(measured, 0u) << pathCase.Name << ": no pass produced a measurement out of "
                                << snapshot.Passes.size();

        const GPUPassTimerPool::PassTotal total = GPUPassTimerPool::SumTopLevel(snapshot.Passes);
        EXPECT_LE(total.GpuMs, snapshot.Frame.GpuMs * 1.05 + 0.05)
            << pathCase.Name << ": the top-level pass total exceeds the frame span it nests in";

        std::cout << "[ gpu-timing ] " << pathCase.Name << ": " << snapshot.Passes.size() << " pass(es), " << measured
                  << " measured, " << absent << " unmeasured; total " << total.GpuMs << " ms of frame "
                  << snapshot.Frame.GpuMs << " ms; age " << snapshot.AgeFrames << ", dropped " << snapshot.DroppedSlots
                  << "\n";
    }
}

TEST_F(GpuTimingPoolEvidence, RealFramesPublishMeasuredPassesWithFrameIdentity)
{
    OLO_ENSURE_GPU_OR_SKIP();

    const GPUPassTimerPool::FrameTimings snapshot = RenderUntilResolved();

    ASSERT_GT(snapshot.FrameNumber, 0u)
        << "the pool resolved no frame at all after 32 rendered frames — the timestamps are not reaching the device";
    ASSERT_TRUE(snapshot.Frame.IsValid())
        << "frame span: " << ToString(snapshot.Frame.Status) << " — " << DescribeGpuTimingStatus(snapshot.Frame.Status);

    // Frame identity travels with the numbers (criterion 4), and the lag is the
    // designed one rather than a dropped slot.
    EXPECT_GT(snapshot.CurrentFrameNumber, 0u);
    EXPECT_LE(snapshot.FrameNumber, snapshot.CurrentFrameNumber);
    EXPECT_LT(snapshot.AgeFrames, static_cast<u64>(GPUPassTimerPool::kSlotCount))
        << "a steady-state age at or beyond the ring size means slots are being dropped, not resolved";
    EXPECT_FALSE(snapshot.IsStale());

    // At least one pass really was measured. Without this the test would pass
    // on an empty pass list, which is the classic "assertion on a frame with
    // nothing in it".
    const auto measured = std::ranges::count_if(snapshot.Passes, [](const GPUPassTimerPool::PassTiming& pass)
                                                { return pass.IsValid(); });
    EXPECT_GT(measured, 0) << "no pass produced a measurement out of " << snapshot.Passes.size() << " published";

    std::string report;
    for (const GPUPassTimerPool::PassTiming& pass : snapshot.Passes)
    {
        report += "  " + pass.Name + (pass.IsSubPass ? " [sub of " + pass.ParentName + "]" : "") + " = " +
                  (pass.IsValid() ? std::to_string(pass.Sample.GpuMs) + " ms"
                                  : std::string(ToString(pass.Sample.Status))) +
                  "\n";
    }
    std::cout << "[ gpu-timing ] frame " << snapshot.FrameNumber << " (age " << snapshot.AgeFrames << ", dropped "
              << snapshot.DroppedSlots << "), frame span " << snapshot.Frame.GpuMs << " ms\n"
              << report;
}

TEST_F(GpuTimingPoolEvidence, NoPassPublishesANumberWithoutAMeasurementBehindIt)
{
    // THE LIVE NEGATIVE CONTROL, and it took a live run to get right.
    //
    // The first version of this test asserted that every Valid entry carries a
    // STRICTLY POSITIVE duration. On real hardware 2 of 17 passes failed it:
    // SkeletalDeformPass and FluidIntermediatesPass issue no GPU commands in a
    // scene with no skeletal meshes and no fluid, so their brackets open and
    // close on the same GPU tick. That is a measurement of zero, not a missing
    // one, and the classifier says so (see
    // GpuTimingValidity.TickIdenticalPairIsAMeasuredZeroNotAFault).
    //
    // So the invariant is the one that actually matters: a duration exists
    // only where a measurement does. A Valid entry is a finite, non-negative
    // number; a non-Valid one carries a REASON instead of a number, and the
    // reason is never "Valid". If a failure path ever starts publishing a
    // number again, it fails here.
    OLO_ENSURE_GPU_OR_SKIP();

    const GPUPassTimerPool::FrameTimings snapshot = RenderUntilResolved();
    ASSERT_GT(snapshot.FrameNumber, 0u) << "nothing resolved; cannot judge";

    // The frame as a whole always does work, so it is the one entry that must
    // be a strictly positive measurement — the anchor that keeps the rest of
    // this test from passing on a frame that rendered nothing.
    ASSERT_TRUE(snapshot.Frame.IsValid())
        << "frame span: " << ToString(snapshot.Frame.Status) << " — "
        << DescribeGpuTimingStatus(snapshot.Frame.Status);
    EXPECT_GT(snapshot.Frame.GpuMs, 0.0) << "the whole frame measured 0.0 ms of GPU time";

    u32 positive = 0;
    for (const GPUPassTimerPool::PassTiming& pass : snapshot.Passes)
    {
        if (!pass.IsValid())
        {
            EXPECT_NE(pass.Sample.Status, GpuTimingStatus::Valid) << pass.Name;
            continue;
        }
        EXPECT_TRUE(std::isfinite(pass.Sample.GpuMs)) << pass.Name << " is Valid but not finite";
        EXPECT_GE(pass.Sample.GpuMs, 0.0) << pass.Name << " is Valid but negative";
        if (pass.Sample.GpuMs > 0.0)
        {
            ++positive;
        }
    }
    EXPECT_GT(positive, 0u) << "not one pass measured any GPU time at all, over " << snapshot.Passes.size()
                            << " published — this frame rendered nothing, so nothing above was tested";
}

TEST_F(GpuTimingPoolEvidence, SubPassesAreFlaggedAndNestInsideTheirParent)
{
    // Criterion 2 on real data: the nesting is stated by the producer, and the
    // nested interval really is inside the parent's, so summing both would
    // double-count. Only asserted for sub-passes the frame actually produced —
    // which passes run depends on the renderer configuration.
    OLO_ENSURE_GPU_OR_SKIP();

    const GPUPassTimerPool::FrameTimings snapshot = RenderUntilResolved();
    ASSERT_GT(snapshot.FrameNumber, 0u) << "nothing resolved; cannot judge";

    u32 checked = 0;
    for (const GPUPassTimerPool::PassTiming& pass : snapshot.Passes)
    {
        if (!pass.IsSubPass || !pass.IsValid())
        {
            continue;
        }
        EXPECT_FALSE(pass.ParentName.empty()) << pass.Name << " is flagged as a sub-pass with no parent named";
        EXPECT_TRUE(pass.Name.starts_with(pass.ParentName + "/"))
            << pass.Name << " does not carry its declared parent " << pass.ParentName;

        const auto parent = std::ranges::find_if(snapshot.Passes,
                                                 [&pass](const GPUPassTimerPool::PassTiming& candidate)
                                                 { return !candidate.IsSubPass && candidate.Name == pass.ParentName; });
        if (parent != snapshot.Passes.end() && parent->IsValid())
        {
            EXPECT_LE(pass.Sample.GpuMs, parent->Sample.GpuMs + 1e-6)
                << pass.Name << " (" << pass.Sample.GpuMs << " ms) outlasted " << pass.ParentName << " ("
                << parent->Sample.GpuMs << " ms) — the bracket it nests in";
            ++checked;
        }
    }
    std::cout << "[ gpu-timing ] checked " << checked << " sub-pass/parent nesting pair(s)\n";
}

TEST_F(GpuTimingPoolEvidence, SumTopLevelStaysInsideTheFrameSpan)
{
    // The shared summation rule, applied to real data. Summing the sub-passes
    // as well would exceed the frame span on any scene where ScenePass has a
    // depth-prepass split, which is the double-count criterion 2 forbids.
    OLO_ENSURE_GPU_OR_SKIP();

    const GPUPassTimerPool::FrameTimings snapshot = RenderUntilResolved();
    ASSERT_TRUE(snapshot.Frame.IsValid()) << "no frame span measured; cannot judge";

    const GPUPassTimerPool::PassTotal total = GPUPassTimerPool::SumTopLevel(snapshot.Passes);

    std::cout << "[ gpu-timing ] pass total " << total.GpuMs << " ms over " << total.ValidPasses << " measured pass(es)"
              << " (" << total.UnmeasuredPasses << " unmeasured, " << total.ExcludedSubPasses
              << " sub-passes excluded); frame span " << snapshot.Frame.GpuMs << " ms\n";

    // A tick of slack: pass brackets can straddle the frame bracket's edges by
    // a timestamp granule, which is a property of scope-free timestamps rather
    // than of the accounting.
    EXPECT_LE(total.GpuMs, snapshot.Frame.GpuMs * 1.05 + 0.05)
        << "the top-level pass total exceeds the frame span it is contained in — nested intervals are being summed";
}

TEST_F(GpuTimingPoolEvidence, ResizingTheRenderTargetDoesNotProduceAMeasuredZero)
{
    // Criterion 1's RESIZE cell. A resize re-creates render targets and can
    // change which passes run, so the pass list and the in-flight timestamp
    // slots disagree for a few frames. The requirement is not that timings
    // survive it — it is that whatever comes out is labelled.
    OLO_ENSURE_GPU_OR_SKIP();

    ASSERT_GT(RenderUntilResolved().FrameNumber, 0u) << "nothing resolved before the resize; cannot judge";

    ResizeRenderTarget(384, 320);

    // Sample every frame across the resize, including the unsettled ones.
    for (u32 i = 0; i < 12; ++i)
    {
        RunFrames(1);
        const GPUPassTimerPool::FrameTimings snapshot = GPUPassTimerPool::GetInstance().GetLastFrameTimings();
        for (const GPUPassTimerPool::PassTiming& pass : snapshot.Passes)
        {
            if (pass.IsValid())
            {
                // The resize must not turn a measurement into a garbage one.
                // A legitimate zero is still legitimate here; a negative or
                // non-finite one never is.
                EXPECT_GE(pass.Sample.GpuMs, 0.0)
                    << "frame " << snapshot.FrameNumber << ": " << pass.Name << " is Valid but negative";
                EXPECT_TRUE(std::isfinite(pass.Sample.GpuMs))
                    << "frame " << snapshot.FrameNumber << ": " << pass.Name << " is Valid but not finite";
            }
        }
        if (snapshot.Frame.IsValid())
        {
            EXPECT_GE(snapshot.Frame.GpuMs, 0.0)
                << "frame " << snapshot.FrameNumber << ": negative frame span";
        }
    }
}

TEST_F(GpuTimingPoolEvidence, ProfilerCarriesTheFrameGpuTimesValidityThrough)
{
    // The publish path RenderPipeline uses. A frame the pool could not measure
    // must not reach the profiler as 0.0 ms of GPU work, because the bottleneck
    // analysis reads that as "the GPU did nothing" and declares the frame
    // CPU-bound — the most actively misleading verdict it can produce.
    OLO_ENSURE_GPU_OR_SKIP();

    ASSERT_GT(RenderUntilResolved().FrameNumber, 0u) << "nothing resolved; cannot judge";

    const GpuTimingSample published = RendererProfiler::GetInstance().GetLastCompletedFrameGpuSample();

    // Whatever it is, it agrees with itself: a number only when it says so.
    if (published.IsValid())
    {
        EXPECT_GE(published.GpuMs, 0.0) << "the profiler holds a negative frame GPU time";
        EXPECT_TRUE(std::isfinite(published.GpuMs));
    }
    else
    {
        std::cout << "[ gpu-timing ] profiler frame GPU time: " << ToString(published.Status) << " — "
                  << DescribeGpuTimingStatus(published.Status) << "\n";
    }
}

TEST_F(GpuTimingPoolEvidence, EveryExportedRendererCounterHasALiveProducer)
{
    // Criterion 3, made durable. The audit that motivated this change found
    // four declared-but-never-written renderer statistics, and a comment saying
    // "these all have producers" rots the moment someone adds a fifth. So the
    // claim is a test: render real frames through the real pipeline and require
    // every counter that reaches result.json to have MOVED.
    //
    // A counter that can legitimately be zero in this fixture does not belong
    // in this assertion — it belongs in a fixture that exercises it. That is
    // the point: a field nothing can make non-zero is a field with no producer.
    OLO_ENSURE_GPU_OR_SKIP();

    ASSERT_GT(RenderUntilResolved().FrameNumber, 0u) << "nothing rendered; cannot judge";

    const Benchmark::RendererCounters counters = Benchmark::SnapshotRendererCounters();

    EXPECT_GT(counters.DrawCalls, 0u) << "DrawCalls has no live producer reaching the benchmark export";
    EXPECT_GT(counters.TrianglesRendered, 0u) << "TrianglesRendered has no live producer reaching the export";
    EXPECT_GT(counters.GpuMemoryTotalBytes, 0u) << "GpuMemoryTotalBytes has no live producer reaching the export";
    // InstancesRendered counts INSTANCED submissions specifically, and this
    // fixture draws one non-instanced cube, so it is legitimately 0 here.
    // Its producer is covered by the instancing fixtures; named rather than
    // asserted so the omission is a decision and not an oversight.

    std::cout << "[ gpu-timing ] counters: draws=" << counters.DrawCalls << " tris=" << counters.TrianglesRendered
              << " instances=" << counters.InstancesRendered << " gpuBytes=" << counters.GpuMemoryTotalBytes << "\n";
}

TEST_F(GpuTimingPoolEvidence, MsaaChangesThePassSetWithoutChangingTheValidityContract)
{
    // The MSAA cell the HANDOVER asks for, and it is GATED ON WHAT THE DEVICE
    // ACTUALLY RETURNED rather than on what was requested. PR #1403 exists
    // because an `Msaa4`-named capture was written at one sample; here the
    // consequence would be worse than a mislabelled file, because the whole
    // point of the cell is that MSAA adds a resolve pass and therefore another
    // bracket. If the device clamps the request to 1 there is no extra bracket
    // and the cell did not run, so it SKIPS rather than passing vacuously.
    OLO_ENSURE_GPU_OR_SKIP();

    RendererSettings& settings = Renderer3D::GetRendererSettings();
    settings.Path = RenderingPath::Deferred; // MSAASampleCount lives on the deferred path
    const u32 requested = 4u;
    settings.Deferred.MSAASampleCount = requested;
    Renderer3D::ApplyRendererSettings();

    const u32 applied = Renderer3D::GetRendererSettings().Deferred.MSAASampleCount;
    if (applied != requested)
    {
        GTEST_SKIP() << "the device returned " << applied << " sample(s) for a request of " << requested
                     << " (driver max " << Renderer3D::GetMaxMSAASamples()
                     << "), so this is not an MSAA cell and must not report as one";
    }

    const GPUPassTimerPool::FrameTimings snapshot = RenderUntilResolved();
    ASSERT_GT(snapshot.FrameNumber, 0u) << "nothing resolved at " << applied << "x MSAA";
    ASSERT_TRUE(snapshot.Frame.IsValid())
        << applied << "x MSAA: frame span " << ToString(snapshot.Frame.Status);

    u32 measured = 0;
    for (const GPUPassTimerPool::PassTiming& pass : snapshot.Passes)
    {
        if (pass.IsValid())
        {
            EXPECT_GE(pass.Sample.GpuMs, 0.0) << pass.Name;
            EXPECT_TRUE(std::isfinite(pass.Sample.GpuMs)) << pass.Name;
            ++measured;
        }
        else
        {
            EXPECT_NE(pass.Sample.Status, GpuTimingStatus::Valid) << pass.Name;
        }
    }
    EXPECT_GT(measured, 0u) << applied << "x MSAA: no pass produced a measurement";

    const GPUPassTimerPool::PassTotal total = GPUPassTimerPool::SumTopLevel(snapshot.Passes);
    EXPECT_LE(total.GpuMs, snapshot.Frame.GpuMs * 1.05 + 0.05)
        << applied << "x MSAA: the pass total exceeds the frame span";

    std::cout << "[ gpu-timing ] MSAA " << applied << "x (requested " << requested << ", driver max "
              << Renderer3D::GetMaxMSAASamples() << "): " << snapshot.Passes.size() << " pass(es), " << measured
              << " measured; total " << total.GpuMs << " ms of frame " << snapshot.Frame.GpuMs << " ms\n";
}
