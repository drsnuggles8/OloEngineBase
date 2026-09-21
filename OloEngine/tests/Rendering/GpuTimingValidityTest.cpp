// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// GpuTimingValidityTest — the negative controls for issue #1337.
//
// THE POINT OF THIS FILE, stated once: a GPU timing that could not be measured
// must reach every consumer as "unavailable", never as 0.0. A plausible zero is
// indistinguishable from a pass that genuinely cost nothing, and the checked-in
// parallel-recording study (docs/analysis/vulkan-parallel-recording-1013.md)
// recorded exactly that failure — "GPU timestamp samples were sometimes stale
// or zero" — with no way to tell which.
//
// So most of what follows is written as NEGATIVE CONTROLS: each test starves a
// measurement in one specific way and asserts the consumer says so. A test that
// only checked the happy path would have passed against the old code, because
// the old code's happy path was fine. The bug was entirely in what it did when
// the measurement failed.
//
// Everything here is pure: the validity decision (ResolveGpuTimingPair), the
// summation rule (GPUPassTimerPool::SumTopLevel) and the MCP JSON shaping
// (PassTimings::BuildPassTimings) are all free functions over plain data, so
// the cases no GPU produces on demand — a backwards timestamp pair, a ring that
// wrapped — are reachable here without a device. The live-device half is
// GpuTimingPoolEvidenceTest.cpp, which drives the real pool on a real context.
// =============================================================================

#include "MCP/McpPassTimings.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/Debug/GPUTimingStatus.h"

#include <array>
#include <set>
#include <string>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace)
namespace PT = OloEngine::MCP::PassTimings;

namespace
{
    // A readout that WOULD resolve to a real measurement. Each test below
    // damages exactly one field of it, so the assertion names the cause.
    GpuTimingPairReadout HealthyReadout(u64 beginNs = 1'000'000, u64 endNs = 3'500'000)
    {
        return GpuTimingPairReadout{ .BeginStamped = true,
                                     .EndStamped = true,
                                     .BeginReadable = true,
                                     .EndReadable = true,
                                     .BeginNs = beginNs,
                                     .EndNs = endNs };
    }

    GPUPassTimerPool::PassTiming Pass(std::string name, GpuTimingSample sample, bool isSubPass = false,
                                      std::string parent = {})
    {
        return GPUPassTimerPool::PassTiming{ std::move(name), sample, isSubPass, std::move(parent) };
    }
} // namespace

// =============================================================================
// 1. The validity decision itself.
// =============================================================================

TEST(GpuTimingValidity, HealthyPairResolvesToAMeasurement)
{
    // The control the negative controls are measured against. 1.0 ms to 3.5 ms
    // on the GPU timeline is 2.5 ms of work.
    const GpuTimingSample sample = ResolveGpuTimingPair(HealthyReadout(1'000'000, 3'500'000));

    EXPECT_EQ(sample.Status, GpuTimingStatus::Valid);
    EXPECT_TRUE(sample.IsValid());
    EXPECT_DOUBLE_EQ(sample.GpuMs, 2.5);
}

TEST(GpuTimingValidity, RefusedBeginStampIsNotStampedNotZero)
{
    // THE VULKAN PARALLEL-RECORDING CASE. WriteTimestamp is refused on a
    // RecordParallel worker (ADR 0011 amendment (92) rule 7). The query then
    // holds whatever it held before — zero on a fresh object — and reads back
    // perfectly cleanly. The old code subtracted two such reads and published
    // the difference as a measured 0.0 ms.
    GpuTimingPairReadout readout = HealthyReadout();
    readout.BeginStamped = false;

    const GpuTimingSample sample = ResolveGpuTimingPair(readout);

    EXPECT_EQ(sample.Status, GpuTimingStatus::NotStamped);
    EXPECT_FALSE(sample.IsValid());
    // The substitute is the CALLER's choice, made visibly at the call site.
    EXPECT_DOUBLE_EQ(sample.ValueOr(-1.0), -1.0);
}

TEST(GpuTimingValidity, RefusedEndStampIsNotStamped)
{
    GpuTimingPairReadout readout = HealthyReadout();
    readout.EndStamped = false;

    EXPECT_EQ(ResolveGpuTimingPair(readout).Status, GpuTimingStatus::NotStamped);
}

TEST(GpuTimingValidity, UnstampedPairWithStaleValuesStillReportsNotStamped)
{
    // The nastier shape of the same case. The queries are REUSED every ring
    // cycle, so a refused stamp can leave a pair holding a four-frame-old
    // interval that subtracts to a believable duration. Being unreadable is
    // not what makes it wrong; never having been stamped is.
    GpuTimingPairReadout readout = HealthyReadout(5'000'000, 9'000'000);
    readout.BeginStamped = false;
    readout.EndStamped = false;

    const GpuTimingSample sample = ResolveGpuTimingPair(readout);

    EXPECT_EQ(sample.Status, GpuTimingStatus::NotStamped)
        << "a plausible 4.0 ms from a pair that was never stamped is exactly the #1337 defect";
    EXPECT_FALSE(sample.IsValid());
}

TEST(GpuTimingValidity, UnreadableResultIsPendingNotZero)
{
    // Stamped, but the device has not finished. The designed state for the
    // first frames of a session — and distinct from NotStamped, because this
    // one will resolve on its own.
    GpuTimingPairReadout readout = HealthyReadout();
    readout.EndReadable = false;

    const GpuTimingSample sample = ResolveGpuTimingPair(readout);

    EXPECT_EQ(sample.Status, GpuTimingStatus::Pending);
    EXPECT_FALSE(sample.IsValid());
}

TEST(GpuTimingValidity, BackwardsPairIsOutOfOrderNotZero)
{
    // THE CROSS-QUEUE CASE. Timestamps from two queue families share no
    // timebase, so an async-compute pass can end "before" it began. The old
    // code's `end > begin ? diff : 0.0` turned that into a free pass. A
    // MEASURED zero (equal stamps) is a different thing and stays Valid — see
    // TickIdenticalPairIsAMeasuredZeroNotAFault.
    const GpuTimingSample sample = ResolveGpuTimingPair(HealthyReadout(9'000'000, 2'000'000));

    EXPECT_EQ(sample.Status, GpuTimingStatus::OutOfOrder);
    EXPECT_FALSE(sample.IsValid());
}

TEST(GpuTimingValidity, TickIdenticalPairIsAMeasuredZeroNotAFault)
{
    // THE LINE THIS CHANGE MUST NOT OVERSHOOT. A pass that issued no GPU
    // commands opens and closes its bracket on the same tick, and that is a
    // real measurement of zero work — not a failure. An earlier revision of
    // this fix classified it as OutOfOrder, and a live run showed 2 of 17
    // passes per frame landing there on an ordinary scene, which would have
    // sent a reader hunting a cross-queue bug that did not exist and marked the
    // frame's pass total incomplete on essentially every frame.
    //
    // The zeros #1337 deletes are the ones with nothing behind them. This one
    // has a stamped, readable, correctly ordered pair behind it.
    const GpuTimingSample sample = ResolveGpuTimingPair(HealthyReadout(7'000'000, 7'000'000));

    EXPECT_EQ(sample.Status, GpuTimingStatus::Valid);
    EXPECT_TRUE(sample.IsValid());
    EXPECT_DOUBLE_EQ(sample.GpuMs, 0.0);
}

TEST(GpuTimingValidity, BackwardsPairIsStillAFault)
{
    // The other side of that line: end BEFORE begin has no duration in it at
    // all, and stays a fault.
    EXPECT_EQ(ResolveGpuTimingPair(HealthyReadout(9'000'000, 8'999'999)).Status, GpuTimingStatus::OutOfOrder);
}

TEST(GpuTimingValidity, NotStampedOutranksPendingAndOutOfOrder)
{
    // Order of causes: a pair that was never stamped is not "pending" (nothing
    // is coming) and not "out of order" (there is nothing to compare). The
    // reported reason has to be the one a reader can act on.
    GpuTimingPairReadout readout;
    readout.BeginStamped = false;
    readout.EndStamped = false;
    readout.BeginReadable = false;
    readout.EndReadable = false;
    readout.BeginNs = 9'000'000;
    readout.EndNs = 1'000'000;

    EXPECT_EQ(ResolveGpuTimingPair(readout).Status, GpuTimingStatus::NotStamped);
}

TEST(GpuTimingValidity, EveryStatusHasADistinctWireNameAndDescription)
{
    // The vocabulary reaches JSON, result.json and the editor panels. A
    // duplicate or empty spelling would make two different causes read alike —
    // which is the defect again, one level up.
    const std::array<GpuTimingStatus, 7> all{ GpuTimingStatus::Valid, GpuTimingStatus::Pending,
                                              GpuTimingStatus::Dropped, GpuTimingStatus::NotStamped,
                                              GpuTimingStatus::OutOfOrder, GpuTimingStatus::NotTimed,
                                              GpuTimingStatus::Unavailable };
    std::set<std::string> names;
    std::set<std::string> descriptions;
    for (const GpuTimingStatus status : all)
    {
        const std::string name{ ToString(status) };
        const std::string description{ DescribeGpuTimingStatus(status) };
        EXPECT_FALSE(name.empty());
        EXPECT_FALSE(description.empty());
        names.insert(name);
        descriptions.insert(description);
    }
    EXPECT_EQ(names.size(), all.size()) << "two statuses share a wire name";
    EXPECT_EQ(descriptions.size(), all.size()) << "two statuses share a description";
}

// =============================================================================
// 2. Totalling a list that has holes in it.
//    Criterion 2: duplicate/nested intervals are not summed as elapsed time.
// =============================================================================

TEST(GpuTimingValidity, SumExcludesSubPassesSoNestedTimeIsNotDoubleCounted)
{
    // ScenePass's bracket CONTAINS its DepthPrepass and Color sub-brackets.
    // Summing all three reports 2.0 + 0.5 + 1.2 = 3.7 ms of GPU work for 2.0 ms
    // of GPU work.
    const std::vector<GPUPassTimerPool::PassTiming> passes{
        Pass("ScenePass", GpuTimingSample::Measured(2.0)),
        Pass("ScenePass/DepthPrepass", GpuTimingSample::Measured(0.5), true, "ScenePass"),
        Pass("ScenePass/Color", GpuTimingSample::Measured(1.2), true, "ScenePass"),
        Pass("Bloom", GpuTimingSample::Measured(0.3)),
    };

    const GPUPassTimerPool::PassTotal total = GPUPassTimerPool::SumTopLevel(passes);

    EXPECT_DOUBLE_EQ(total.GpuMs, 2.3);
    EXPECT_EQ(total.ValidPasses, 2u);
    EXPECT_EQ(total.ExcludedSubPasses, 2u);
    EXPECT_TRUE(total.IsComplete());
}

TEST(GpuTimingValidity, SumSkipsUnmeasuredPassesAndSaysTheTotalIsAFloor)
{
    // NEGATIVE CONTROL for the total. A pass with no measurement contributes
    // nothing — correct — but a total that silently loses a pass and still
    // presents itself as the frame's pass time is the same lie one level up.
    const std::vector<GPUPassTimerPool::PassTiming> passes{
        Pass("ScenePass", GpuTimingSample::Measured(2.0)),
        Pass("Shadow", GpuTimingSample::Absent(GpuTimingStatus::NotStamped)),
        Pass("GTAO", GpuTimingSample::Absent(GpuTimingStatus::Dropped)),
    };

    const GPUPassTimerPool::PassTotal total = GPUPassTimerPool::SumTopLevel(passes);

    EXPECT_DOUBLE_EQ(total.GpuMs, 2.0);
    EXPECT_EQ(total.ValidPasses, 1u);
    EXPECT_EQ(total.UnmeasuredPasses, 2u);
    EXPECT_FALSE(total.IsComplete()) << "a total missing two passes must not claim to be complete";
}

TEST(GpuTimingValidity, SubPassFlagIsTheProducersNotDerivedFromTheName)
{
    // REGRESSION GUARD for the old string-parsing rule. A pass whose own name
    // contains a slash used to be read as somebody's sub-pass and dropped out
    // of the total. The flag says it is top-level, so it counts.
    const std::vector<GPUPassTimerPool::PassTiming> passes{
        Pass("Terrain/VirtualTexture", GpuTimingSample::Measured(1.5)),
    };

    const GPUPassTimerPool::PassTotal total = GPUPassTimerPool::SumTopLevel(passes);

    EXPECT_DOUBLE_EQ(total.GpuMs, 1.5);
    EXPECT_EQ(total.ExcludedSubPasses, 0u);
}

// =============================================================================
// 3. The consumer. Criterion 4's negative control for stale data.
//    This is the one that proves the fix: the MCP tool must report
//    "unavailable" rather than 0.0 for a starved readback.
// =============================================================================

namespace
{
    PT::FrameTotals ValidTotals(f64 gpuMs = 4.0)
    {
        PT::FrameTotals totals;
        totals.FrameTimeMs = 16.6;
        totals.CpuMs = 5.0;
        totals.Gpu = GpuTimingSample::Measured(gpuMs);
        totals.GpuMeasurementFrameId = 100;
        totals.CurrentFrameId = 102;
        totals.GpuResultsAgeFrames = 2;
        return totals;
    }
} // namespace

TEST(GpuTimingValidity, ConsumerReportsNullAndAStatusForAStarvedPass)
{
    // THE CENTREPIECE. One pass measured, one starved. The starved one must
    // arrive as JSON null with a status naming the cause — not as 0.
    const std::vector<PT::GpuPassEntry> gpuPasses{
        PT::GpuPassEntry{ "ScenePass", GpuTimingSample::Measured(2.0), false, {} },
        PT::GpuPassEntry{ "Shadow", GpuTimingSample::Absent(GpuTimingStatus::Dropped), false, {} },
    };

    const auto json = PT::BuildPassTimings(gpuPasses, {}, ValidTotals());

    ASSERT_EQ(json["passes"].size(), 2u);
    EXPECT_DOUBLE_EQ(json["passes"][0]["gpuMs"].get<f64>(), 2.0);
    EXPECT_EQ(json["passes"][0]["gpuStatus"].get<std::string>(), "valid");

    EXPECT_TRUE(json["passes"][1]["gpuMs"].is_null())
        << "a dropped readback must not publish a number at all; it published " << json["passes"][1]["gpuMs"].dump();
    EXPECT_FALSE(json["passes"][1]["gpuMs"].is_number())
        << "0 would be indistinguishable from a pass that cost nothing — the whole of #1337";
    EXPECT_EQ(json["passes"][1]["gpuStatus"].get<std::string>(), "dropped");
}

TEST(GpuTimingValidity, ConsumerReportsTheFrameTotalAsNullWhenTheFrameWasNotMeasured)
{
    PT::FrameTotals totals = ValidTotals();
    totals.Gpu = GpuTimingSample::Absent(GpuTimingStatus::Unavailable);

    const auto json = PT::BuildPassTimings({}, {}, totals);

    EXPECT_TRUE(json["frame"]["gpuMs"].is_null());
    EXPECT_EQ(json["frame"]["gpuStatus"].get<std::string>(), "unavailable");
    EXPECT_EQ(json["gpuResultsStatus"].get<std::string>(), "unavailable");
}

TEST(GpuTimingValidity, CpuOnlyPassIsNotTimedRatherThanZero)
{
    // A pass the graph executed that the GPU list does not carry — timer-pool
    // overflow, or a topology change between the resolved GPU frame and the
    // current CPU frame. It used to be appended with gpuMs 0, which reads as
    // "this pass is free on the GPU".
    const std::vector<PT::CpuPassEntry> cpuPasses{ PT::CpuPassEntry{ "LateGeometry", 0.4 } };

    const auto json = PT::BuildPassTimings({}, cpuPasses, ValidTotals());

    ASSERT_EQ(json["passes"].size(), 1u);
    EXPECT_EQ(json["passes"][0]["pass"].get<std::string>(), "LateGeometry");
    EXPECT_TRUE(json["passes"][0]["gpuMs"].is_null());
    EXPECT_EQ(json["passes"][0]["gpuStatus"].get<std::string>(), "notTimed");
    EXPECT_DOUBLE_EQ(json["passes"][0]["cpuMs"].get<f64>(), 0.4);

    // But NOT counted as a hole in the resolved GPU frame. The GPU numbers lag
    // the CPU ones by 1-3 frames, so a name in one list and not the other is
    // the normal consequence of that lag, and its GPU time is not missing from
    // passGpuTotalMs — it belongs to a different frame. Counting it would
    // null out unattributedGpuMs on healthy frames.
    EXPECT_EQ(json["unmeasuredPasses"].get<u32>(), 0u);
    EXPECT_TRUE(json["passGpuTotalIsComplete"].get<bool>());
}

TEST(GpuTimingValidity, AnUntimedPassInTheResolvedFrameDoesBreakCompleteness)
{
    // The control for the test above. A pass the POOL published as NotTimed
    // really was in the resolved frame and really has no number, so the total
    // is a floor and says so. This is the case the per-frame timer budget
    // produces, and it is what unmeasuredPasses is actually for.
    const std::vector<PT::GpuPassEntry> gpuPasses{
        PT::GpuPassEntry{ "ScenePass", GpuTimingSample::Measured(2.0), false, {} },
        PT::GpuPassEntry{ "<3 pass(es) over the per-frame timer budget>",
                          GpuTimingSample::Absent(GpuTimingStatus::NotTimed),
                          false,
                          {} },
    };

    const auto json = PT::BuildPassTimings(gpuPasses, {}, ValidTotals(4.0));

    EXPECT_EQ(json["unmeasuredPasses"].get<u32>(), 1u);
    EXPECT_FALSE(json["passGpuTotalIsComplete"].get<bool>());
    EXPECT_TRUE(json["unattributedGpuMs"].is_null());
}

TEST(GpuTimingValidity, NothingEverResolvedReportsStaleNotFresh)
{
    // The MCP flag and GPUPassTimerPool::FrameTimings::IsStale() must answer
    // the same question the same way. A pool that has never produced a frame
    // has an AGE of 0, so an age-only test called it fresh while the benchmark
    // export called it stale — two answers to one question.
    PT::FrameTotals totals;
    totals.GpuMeasurementFrameId = 0;
    totals.GpuResultsAgeFrames = 0;

    EXPECT_TRUE(PT::BuildPassTimings({}, {}, totals)["gpuResultsStale"].get<bool>());
}

TEST(GpuTimingValidity, UnstampedFramesAreCountedApartFromDroppedSlots)
{
    // Two faults, two counters. A backend that declines timestamps would
    // otherwise make gpuDroppedSlots climb every frame and report a GPU
    // backlog that never happened.
    PT::FrameTotals totals = ValidTotals();
    totals.GpuDroppedSlots = 2;
    totals.GpuUnstampedFrames = 11;

    const auto json = PT::BuildPassTimings({}, {}, totals);

    EXPECT_EQ(json["gpuDroppedSlots"].get<u32>(), 2u);
    EXPECT_EQ(json["gpuUnstampedFrames"].get<u32>(), 11u);
}

TEST(GpuTimingValidity, UnattributedGpuTimeIsNullWhenAnyPassIsMissing)
{
    // NEGATIVE CONTROL for a derived number. unattributedGpuMs is
    // frameGpu - sum(passes). With a pass missing from the sum, that difference
    // silently ATTRIBUTES the missing pass's time to "unattributed" — inventing
    // a finding about barriers and transient materialization that is really
    // just the pass nobody timed.
    const std::vector<PT::GpuPassEntry> gpuPasses{
        PT::GpuPassEntry{ "ScenePass", GpuTimingSample::Measured(2.0), false, {} },
        PT::GpuPassEntry{ "Shadow", GpuTimingSample::Absent(GpuTimingStatus::NotStamped), false, {} },
    };

    const auto json = PT::BuildPassTimings(gpuPasses, {}, ValidTotals(4.0));

    EXPECT_TRUE(json["unattributedGpuMs"].is_null())
        << "with Shadow unmeasured, 4.0 - 2.0 = 2.0 ms would be reported as unattributed GPU time "
           "when it is really Shadow's";
    EXPECT_DOUBLE_EQ(json["passGpuTotalMs"].get<f64>(), 2.0);
    EXPECT_EQ(json["unmeasuredPasses"].get<u32>(), 1u);
}

TEST(GpuTimingValidity, UnattributedGpuTimeIsDerivedWhenEverythingWasMeasured)
{
    // The positive control for the test above: with a complete list the
    // subtraction is legitimate and still happens.
    const std::vector<PT::GpuPassEntry> gpuPasses{
        PT::GpuPassEntry{ "ScenePass", GpuTimingSample::Measured(2.0), false, {} },
        PT::GpuPassEntry{ "Shadow", GpuTimingSample::Measured(1.0), false, {} },
    };

    const auto json = PT::BuildPassTimings(gpuPasses, {}, ValidTotals(4.0));

    ASSERT_FALSE(json["unattributedGpuMs"].is_null());
    EXPECT_DOUBLE_EQ(json["unattributedGpuMs"].get<f64>(), 1.0);
    EXPECT_TRUE(json["passGpuTotalIsComplete"].get<bool>());
}

TEST(GpuTimingValidity, SubPassTimeIsAttachedToItsParentAndKeptOutOfTheTotal)
{
    const std::vector<PT::GpuPassEntry> gpuPasses{
        PT::GpuPassEntry{ "ScenePass", GpuTimingSample::Measured(2.0), false, {} },
        PT::GpuPassEntry{ "ScenePass/DepthPrepass", GpuTimingSample::Measured(0.5), true, "ScenePass" },
    };

    const auto json = PT::BuildPassTimings(gpuPasses, {}, ValidTotals(2.0));

    ASSERT_EQ(json["passes"].size(), 1u) << "the sub-pass must not become a second top-level entry";
    ASSERT_TRUE(json["passes"][0].contains("subPasses"));
    EXPECT_EQ(json["passes"][0]["subPasses"][0]["name"].get<std::string>(), "DepthPrepass");
    EXPECT_DOUBLE_EQ(json["passes"][0]["subPasses"][0]["gpuMs"].get<f64>(), 0.5);
    EXPECT_DOUBLE_EQ(json["passGpuTotalMs"].get<f64>(), 2.0) << "0.5 ms is already inside ScenePass's 2.0 ms";
}

TEST(GpuTimingValidity, StarvedSubPassAlsoReportsItsStatus)
{
    const std::vector<PT::GpuPassEntry> gpuPasses{
        PT::GpuPassEntry{ "ScenePass", GpuTimingSample::Measured(2.0), false, {} },
        PT::GpuPassEntry{ "ScenePass/DepthPrepass", GpuTimingSample::Absent(GpuTimingStatus::OutOfOrder), true,
                          "ScenePass" },
    };

    const auto json = PT::BuildPassTimings(gpuPasses, {}, ValidTotals(2.0));

    ASSERT_TRUE(json["passes"][0].contains("subPasses"));
    EXPECT_TRUE(json["passes"][0]["subPasses"][0]["gpuMs"].is_null());
    EXPECT_EQ(json["passes"][0]["subPasses"][0]["gpuStatus"].get<std::string>(), "outOfOrder");
}

TEST(GpuTimingValidity, OrphanSubPassIsPublishedTopLevelAndCounted)
{
    // Its parent was not GPU-timed this frame, so there is no enclosing bracket
    // for its time to be double-counted inside. Dropping it would lose real
    // measured work.
    const std::vector<PT::GpuPassEntry> gpuPasses{
        PT::GpuPassEntry{ "ScenePass/DepthPrepass", GpuTimingSample::Measured(0.5), true, "ScenePass" },
    };

    const auto json = PT::BuildPassTimings(gpuPasses, {}, ValidTotals(0.5));

    ASSERT_EQ(json["passes"].size(), 1u);
    EXPECT_EQ(json["passes"][0]["pass"].get<std::string>(), "ScenePass/DepthPrepass");
    EXPECT_DOUBLE_EQ(json["passGpuTotalMs"].get<f64>(), 0.5);
}

TEST(GpuTimingValidity, FrameIdentityTravelsWithTheNumbers)
{
    // Criterion 4: measurement frame IDs. The GPU numbers lag the CPU numbers
    // by design, so which frame each half describes has to be stated rather
    // than assumed to be "this one".
    PT::FrameTotals totals = ValidTotals();
    totals.GpuMeasurementFrameId = 4100;
    totals.CurrentFrameId = 4102;
    totals.GpuResultsAgeFrames = 2;
    totals.GpuDroppedSlots = 7;

    const auto json = PT::BuildPassTimings({}, {}, totals);

    EXPECT_EQ(json["frame"]["gpuMeasurementFrameId"].get<u64>(), 4100u);
    EXPECT_EQ(json["frame"]["currentFrameId"].get<u64>(), 4102u);
    EXPECT_EQ(json["gpuResultsAgeFrames"].get<u64>(), 2u);
    EXPECT_EQ(json["gpuDroppedSlots"].get<u32>(), 7u) << "frames the ring lost outright must be visible";
    EXPECT_FALSE(json["gpuResultsStale"].get<bool>()) << "an age of 2 is the designed resolve latency";
}

TEST(GpuTimingValidity, StaleAgeIsFlaggedAtTheRingSize)
{
    PT::FrameTotals totals = ValidTotals();
    totals.GpuResultsAgeFrames = PT::kGpuResultsStaleThreshold;

    EXPECT_TRUE(PT::BuildPassTimings({}, {}, totals)["gpuResultsStale"].get<bool>());
}

TEST(GpuTimingValidity, FenceAndPresentWaitsAreReportedApart)
{
    // Criterion 2: they say opposite things. A fence wait means the GPU is
    // behind; a present wait means the display is pacing you. Summed into one
    // "gpuWaitMs" they are unreadable as either.
    PT::FrameTotals totals = ValidTotals();
    totals.FenceWaitMs = 1.5;
    totals.PresentWaitMs = 8.0;
    totals.GpuWaitMs = 9.5;

    const auto json = PT::BuildPassTimings({}, {}, totals);

    EXPECT_DOUBLE_EQ(json["frame"]["fenceWaitMs"].get<f64>(), 1.5);
    EXPECT_DOUBLE_EQ(json["frame"]["presentWaitMs"].get<f64>(), 8.0);
    EXPECT_DOUBLE_EQ(json["frame"]["gpuWaitMs"].get<f64>(), 9.5);
}

// =============================================================================
// 4. The producer's own "nothing is running" state.
//    Criterion 1's "unsupported device" cell, reachable with no device at all.
// =============================================================================

TEST(GpuTimingValidity, UninitializedPoolReportsUnavailableRatherThanZeros)
{
    // NEGATIVE CONTROL for the whole producer. A pool that was never started —
    // because the backend declined timestamp queries, or timing is off for this
    // session — must not hand out a zero-filled snapshot that reads as a frame
    // with no GPU work in it.
    //
    // Deliberately does NOT touch the singleton: a device-less test process has
    // no query objects, and the singleton is shared with the Vulkan pass suite.
    // A default-constructed snapshot is what an uninitialized pool returns.
    const GPUPassTimerPool::FrameTimings snapshot{};

    EXPECT_EQ(snapshot.Frame.Status, GpuTimingStatus::Unavailable);
    EXPECT_FALSE(snapshot.Frame.IsValid());
    EXPECT_TRUE(snapshot.Passes.empty());
    EXPECT_EQ(snapshot.FrameNumber, 0u);
    EXPECT_TRUE(snapshot.IsStale()) << "nothing has ever resolved, so nothing here is current";
}

TEST(GpuTimingValidity, SnapshotIsStaleOnceItIsOlderThanTheRing)
{
    GPUPassTimerPool::FrameTimings snapshot;
    snapshot.FrameNumber = 100;
    snapshot.CurrentFrameNumber = 100 + GPUPassTimerPool::kSlotCount - 1;
    snapshot.AgeFrames = GPUPassTimerPool::kSlotCount - 1;
    EXPECT_FALSE(snapshot.IsStale()) << "still inside the ring: this is the designed resolve latency";

    snapshot.AgeFrames = GPUPassTimerPool::kSlotCount;
    EXPECT_TRUE(snapshot.IsStale());
}

TEST(GpuTimingValidity, TheMcpStalenessThresholdMatchesThePoolsRingSize)
{
    // The MCP header duplicates the ring size as an engine-free constant so it
    // unit-tests without the pool. This is the assertion that keeps the two
    // from drifting in the direction the static_assert in McpToolsPerf.cpp
    // cannot see from the test binary.
    EXPECT_EQ(PT::kGpuResultsStaleThreshold, static_cast<u64>(GPUPassTimerPool::kSlotCount));
}

TEST(GpuTimingValidity, TheSevenMeasurementsAreLabelledElapsedOrSum)
{
    // Criterion 2: "Separate CPU preparation, recording wall time, sum of
    // worker times, join time, fence waits, present waits and GPU execution."
    //
    // The numbers all existed; what did not was any way to tell a SUM from an
    // ELAPSED quantity. The checked-in study measured 25.3-27.8 ms of summed
    // worker CPU inside a 2.4 ms wall, so a reader who takes the sum for
    // elapsed frame time concludes the change made things ten times slower.
    PT::FrameTotals totals = ValidTotals(4.0);
    totals.FenceWaitMs = 1.5;
    totals.PresentWaitMs = 8.0;
    totals.ParallelRecording.RegionWallMs = 2.4;
    totals.ParallelRecording.WorkerRecordMs = 26.5; // concurrent: legitimately larger
    totals.ParallelRecording.JoinWaitMs = 0.55;
    totals.ParallelRecording.RegionTimings.push_back(
        PT::ParallelRegionStats{ .PassName = "ScenePass", .SelectionSeedMs = 0.037, .FrontendPrepareMs = 0.04 });

    const auto json = PT::BuildPassTimings({}, {}, totals);
    const auto& breakdown = json["recordingBreakdown"];

    EXPECT_DOUBLE_EQ(breakdown["elapsedRecordingWallMs"].get<f64>(), 2.4);
    EXPECT_DOUBLE_EQ(breakdown["summedWorkerCpuMs"].get<f64>(), 26.5);
    EXPECT_DOUBLE_EQ(breakdown["joinWaitMs"].get<f64>(), 0.55);
    // NEAR, not DOUBLE_EQ: this is a sum of two decimal literals put through
    // the 3-decimal rounding, and the assertion is about the LABEL, not about
    // the last bit of the mantissa.
    EXPECT_NEAR(breakdown["summedCpuPrepareMs"].get<f64>(), 0.077, 1e-9);
    EXPECT_DOUBLE_EQ(breakdown["fenceWaitMs"].get<f64>(), 1.5);
    EXPECT_DOUBLE_EQ(breakdown["presentWaitMs"].get<f64>(), 8.0);
    EXPECT_DOUBLE_EQ(breakdown["gpuExecutionMs"].get<f64>(), 4.0);

    // The sum exceeding the wall is the NORMAL case under concurrency, and the
    // output must not have quietly reconciled them.
    EXPECT_GT(breakdown["summedWorkerCpuMs"].get<f64>(), breakdown["elapsedRecordingWallMs"].get<f64>());
    EXPECT_NE(breakdown["note"].get<std::string>().find("not elapsed"), std::string::npos);
}

TEST(GpuTimingValidity, GpuExecutionInTheBreakdownIsNullWhenUnmeasured)
{
    PT::FrameTotals totals = ValidTotals();
    totals.Gpu = GpuTimingSample::Absent(GpuTimingStatus::NotStamped);

    const auto json = PT::BuildPassTimings({}, {}, totals);

    EXPECT_TRUE(json["recordingBreakdown"]["gpuExecutionMs"].is_null());
    EXPECT_EQ(json["recordingBreakdown"]["gpuExecutionStatus"].get<std::string>(), "notStamped");
}
