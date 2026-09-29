// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "TestTempDir.h"

// =============================================================================
// SceneStreamerBudgetTest — issue #1365 on the loose (.oloregion) path.
//
//   - Region load cancellation: queued (held behind a task-event prerequisite),
//     mid-parse (the worker blocked on an event before or after the real parse),
//     UnloadRegion of a Loading region, and Shutdown with an abandoned load
//     still running. A cancelled load's parsed file is never instantiated.
//   - Byte budget: admission defers against the resident budget and the
//     per-frame budget, rejects a region that can never fit, admits an
//     unknown-size region visibly; eviction unloads for bytes as well as count.
//   - StreamingSettings YAML: the new fields round-trip, a scene saved before
//     them loads with "no budget", and corrupt values are sanitised on load.
//
// Region byte estimates are the .oloregion file sizes, so each test reads the
// sizes back from disk rather than assuming them.
// =============================================================================

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Scene/Streaming/SceneStreamer.h"
#include "OloEngine/Scene/Streaming/StreamingSettings.h"
#include "TestAsyncLoadHooks.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

namespace
{
    namespace fs = std::filesystem;
    using namespace std::chrono_literals;
    using Tests::kLoadHookFailAfter;
    using Tests::LoadHookState;

    // One region holding one tagged entity. `padding` grows the file, and with it
    // the region's byte estimate.
    u64 WriteRegion(const fs::path& directory, u64 regionId, u64 entityId, sizet padding = 0)
    {
        const fs::path path = directory / ("region_" + std::to_string(regionId) + ".oloregion");
        {
            std::ofstream file(path, std::ios::binary);
            file << "Region: region_" << regionId << "\n"
                 << "RegionID: " << regionId << "\n"
                 << "# " << std::string(padding, 'x') << "\n"
                 << "Entities:\n"
                 << "  - Entity: " << entityId << "\n"
                 << "    TagComponent:\n"
                 << "      Tag: streamed_" << regionId << "\n";
        }
        return static_cast<u64>(fs::file_size(path));
    }

    f32 BytesToMegabytes(f64 bytes)
    {
        return static_cast<f32>(bytes / (1024.0 * 1024.0));
    }

    class SceneStreamerBudgetTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            Tests::EnsureTaskSchedulerStarted();
            m_Directory = Tests::TempDir("regions"); // TempDir is already per test
            m_Scene = Scene::Create();
            m_Scene->SetRenderingEnabled(false);
        }

        void TearDown() override
        {
            // Let every blocked worker and gated task go first, so a failed
            // assertion ends the test instead of hanging its shutdown.
            m_Hook->Release.Set();
            for (const Tasks::FTaskEvent& gate : m_Gates)
                gate.Trigger();
            m_Streamer.Shutdown();
            m_Scene = nullptr;
        }

        // Discover the regions written so far, with the scene's settings as they stand.
        void Start()
        {
            m_Scene->GetStreamingSettings().RegionDirectory = m_Directory.string();
            m_Streamer.Initialize(m_Scene.Raw(), MakeSceneStreamerConfig(m_Scene->GetStreamingSettings()));
        }

        void Tick()
        {
            m_Streamer.Update(glm::vec3{ 0.0f }, ++m_Frame);
        }

        bool TickUntil(const std::function<bool()>& done)
        {
            const auto deadline = std::chrono::steady_clock::now() + kLoadHookFailAfter;
            while (std::chrono::steady_clock::now() < deadline)
            {
                Tick();
                if (done())
                    return true;
                std::this_thread::yield();
            }
            return false;
        }

        [[nodiscard]] Ref<StreamingRegion> Region(u64 id) const
        {
            const auto regions = m_Streamer.GetRegions();
            const auto it = regions.find(RegionID{ id });
            return it == regions.end() ? nullptr : it->second;
        }

        [[nodiscard]] bool EntityExists(u64 entityId) const
        {
            return m_Scene->TryGetEntityWithUUID(UUID{ entityId }).has_value();
        }

        Tasks::FTaskEvent MakeGate(const char* name)
        {
            m_Gates.emplace_back(name);
            return m_Gates.back();
        }

        std::shared_ptr<LoadHookState> m_Hook = Tests::MakeLoadHookState();
        fs::path m_Directory;
        Ref<Scene> m_Scene;
        SceneStreamer m_Streamer;
        u64 m_Frame = 0;
        std::vector<Tasks::FTaskEvent> m_Gates;
    };
} // namespace

// -----------------------------------------------------------------------------
// Cancellation on the loose path
// -----------------------------------------------------------------------------
TEST_F(SceneStreamerBudgetTest, RegionLoadCancelledBeforeStartNeverParses)
{
    WriteRegion(m_Directory, 1, 1001);
    Start();

    FRegionLoadTestHooks hooks;
    hooks.StartGate = MakeGate("RegionGate");
    hooks.OnLoadStarted = [hook = m_Hook](RegionID)
    { hook->Started.fetch_add(1); };
    m_Streamer.SetTestHooks(hooks);

    ASSERT_EQ(m_Streamer.LoadRegion(RegionID{ 1 }), EStreamingAdmission::Admitted);
    ASSERT_EQ(Region(1)->m_State, StreamingRegion::State::Loading);

    EXPECT_EQ(m_Streamer.CancelRegionLoad(RegionID{ 1 }), EAssetLoadCancelResult::CancelledBeforeStart);
    EXPECT_EQ(Region(1)->m_State, StreamingRegion::State::Unloaded);
    EXPECT_EQ(m_Streamer.GetPendingLoadCount(), 0u);

    hooks.StartGate->Trigger();
    ASSERT_TRUE(TickUntil([this]
                          { return m_Streamer.GetStats().AbandonedLoadsRunning == 0; }));
    Tick();

    EXPECT_EQ(m_Hook->Started.load(), 0) << "a region load cancelled before start must never parse";
    EXPECT_EQ(Region(1)->m_State, StreamingRegion::State::Unloaded);
    EXPECT_FALSE(EntityExists(1001));
    EXPECT_EQ(m_Streamer.GetStats().CancelledBeforeStart, 1u);
}

namespace
{
    enum class EBlockAt
    {
        BeforeParse,
        AfterParse
    };
} // namespace

class SceneStreamerMidParseCancel : public SceneStreamerBudgetTest, public ::testing::WithParamInterface<EBlockAt>
{
};

TEST_P(SceneStreamerMidParseCancel, ParsedResultIsNeverInstantiated)
{
    WriteRegion(m_Directory, 2, 2002);
    Start();

    auto block = [hook = m_Hook](RegionID)
    { hook->Block(); };
    FRegionLoadTestHooks hooks;
    if (GetParam() == EBlockAt::BeforeParse)
        hooks.OnLoadStarted = block;
    else
        hooks.OnLoadFinished = block;
    m_Streamer.SetTestHooks(hooks);

    ASSERT_EQ(m_Streamer.LoadRegion(RegionID{ 2 }), EStreamingAdmission::Admitted);
    ASSERT_TRUE(m_Hook->WorkerInside.WaitFor()) << "the worker never entered the parse";

    Tick(); // an unfinished load is not processed
    ASSERT_EQ(Region(2)->m_State, StreamingRegion::State::Loading);

    EXPECT_EQ(m_Streamer.CancelRegionLoad(RegionID{ 2 }), EAssetLoadCancelResult::AbandonedInFlight);
    EXPECT_EQ(Region(2)->m_State, StreamingRegion::State::Unloaded);
    EXPECT_EQ(m_Streamer.GetStats().AbandonedLoadsRunning, 1u);

    m_Hook->Release.Set();
    ASSERT_TRUE(TickUntil([this]
                          { return m_Streamer.GetStats().AbandonedResultsDropped == 1; }));

    EXPECT_EQ(Region(2)->m_State, StreamingRegion::State::Unloaded) << "a result arriving after cancellation must not make the region Ready";
    EXPECT_FALSE(EntityExists(2002)) << "the abandoned parse must never be instantiated into the scene";
    EXPECT_EQ(m_Streamer.GetStats().AbandonedInFlight, 1u);

    // The region is loadable again and loads for real.
    m_Streamer.SetTestHooks({});
    ASSERT_EQ(m_Streamer.LoadRegion(RegionID{ 2 }), EStreamingAdmission::Admitted);
    ASSERT_TRUE(TickUntil([this]
                          { return Region(2)->m_State == StreamingRegion::State::Ready; }));
    EXPECT_TRUE(EntityExists(2002));
}

INSTANTIATE_TEST_SUITE_P(BlockPoint, SceneStreamerMidParseCancel, ::testing::Values(EBlockAt::BeforeParse, EBlockAt::AfterParse),
                         [](const ::testing::TestParamInfo<EBlockAt>& info)
                         { return info.param == EBlockAt::BeforeParse ? std::string("BeforeParse") : std::string("AfterParse"); });

// The region's state alone cannot protect a re-request: once the region is
// Loading again, a stale result arriving first would look like the new one. Only
// taking the abandoned load out of the pending set keeps it out. Here the
// abandoned worker holds the OLD file's parse and finishes while the new
// request (reading a rewritten file) is still gated.
TEST_F(SceneStreamerBudgetTest, AnAbandonedParseCannotLandInALaterRequestForTheSameRegion)
{
    WriteRegion(m_Directory, 5, 5001);
    Start();

    FRegionLoadTestHooks blocking;
    blocking.OnLoadFinished = [hook = m_Hook](RegionID)
    { hook->Block(); };
    m_Streamer.SetTestHooks(blocking);
    ASSERT_EQ(m_Streamer.LoadRegion(RegionID{ 5 }), EStreamingAdmission::Admitted);
    ASSERT_TRUE(m_Hook->WorkerInside.WaitFor()) << "the worker never finished parsing";
    ASSERT_EQ(m_Streamer.CancelRegionLoad(RegionID{ 5 }), EAssetLoadCancelResult::AbandonedInFlight);

    // New contents on disk, and a new request that stays queued behind a gate.
    WriteRegion(m_Directory, 5, 5002);
    FRegionLoadTestHooks gated;
    gated.StartGate = MakeGate("ReRequestGate");
    m_Streamer.SetTestHooks(gated);
    ASSERT_EQ(m_Streamer.LoadRegion(RegionID{ 5 }), EStreamingAdmission::Admitted);
    ASSERT_EQ(Region(5)->m_State, StreamingRegion::State::Loading);

    // The stale parse finishes first, while the region is Loading for the new request.
    m_Hook->Release.Set();
    EXPECT_TRUE(TickUntil([this]
                          { return m_Streamer.GetStats().AbandonedResultsDropped == 1; }));
    EXPECT_FALSE(EntityExists(5001)) << "the abandoned parse was instantiated into the later request";
    EXPECT_EQ(Region(5)->m_State, StreamingRegion::State::Loading);

    gated.StartGate->Trigger();
    ASSERT_TRUE(TickUntil([this]
                          { return Region(5)->m_State == StreamingRegion::State::Ready; }));
    EXPECT_TRUE(EntityExists(5002)) << "the region must hold what the live request parsed";
    EXPECT_FALSE(EntityExists(5001));
}

TEST_F(SceneStreamerBudgetTest, UnloadRegionOfALoadingRegionCancelsItsLoad)
{
    WriteRegion(m_Directory, 3, 3003);
    Start();

    FRegionLoadTestHooks hooks;
    hooks.StartGate = MakeGate("UnloadGate");
    m_Streamer.SetTestHooks(hooks);

    ASSERT_EQ(m_Streamer.LoadRegion(RegionID{ 3 }), EStreamingAdmission::Admitted);
    m_Streamer.UnloadRegion(RegionID{ 3 });
    EXPECT_EQ(Region(3)->m_State, StreamingRegion::State::Unloaded);
    EXPECT_EQ(m_Streamer.GetPendingLoadCount(), 0u);
    EXPECT_EQ(m_Streamer.GetStats().CancelledBeforeStart, 1u);

    hooks.StartGate->Trigger();
    ASSERT_TRUE(TickUntil([this]
                          { return m_Streamer.GetStats().AbandonedLoadsRunning == 0; }));
    EXPECT_FALSE(EntityExists(3003));
}

TEST_F(SceneStreamerBudgetTest, ShutdownWaitsForAnAbandonedRegionLoadStillRunning)
{
    WriteRegion(m_Directory, 4, 4004);
    Start();

    FRegionLoadTestHooks hooks;
    hooks.OnLoadFinished = [hook = m_Hook](RegionID)
    {
        hook->Block();
        hook->WorkerExitedAt.store(hook->Sequence.fetch_add(1));
    };
    m_Streamer.SetTestHooks(hooks);

    ASSERT_EQ(m_Streamer.LoadRegion(RegionID{ 4 }), EStreamingAdmission::Admitted);
    ASSERT_TRUE(m_Hook->WorkerInside.WaitFor());
    ASSERT_EQ(m_Streamer.CancelRegionLoad(RegionID{ 4 }), EAssetLoadCancelResult::AbandonedInFlight);

    std::thread shutdown([this, hook = m_Hook]
                         {
        m_Streamer.Shutdown();
        hook->ShutdownReturnedAt.store(hook->Sequence.fetch_add(1)); });

    // A window for a wrong implementation to return early; a correct one cannot
    // return before the release below, so this wait cannot fail it.
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(m_Hook->ShutdownReturnedAt.load(), -1) << "Shutdown returned while an abandoned region load was still running";

    m_Hook->Release.Set();
    shutdown.join();
    EXPECT_GE(m_Hook->WorkerExitedAt.load(), 0);
    EXPECT_LT(m_Hook->WorkerExitedAt.load(), m_Hook->ShutdownReturnedAt.load());
}

// -----------------------------------------------------------------------------
// Admission against the byte budgets
// -----------------------------------------------------------------------------
TEST_F(SceneStreamerBudgetTest, ResidentBudgetDefersWhatDoesNotFitAndRejectsWhatNeverCan)
{
    const u64 a = WriteRegion(m_Directory, 11, 1101, 1000);
    const u64 b = WriteRegion(m_Directory, 12, 1201, 1000);
    const u64 c = WriteRegion(m_Directory, 13, 1301, 1000);
    const u64 huge = WriteRegion(m_Directory, 14, 1401, 20000);

    // Room for two of the three small regions (plus half a region of slack), never the huge one.
    const f64 budgetBytes = static_cast<f64>(a + b) + static_cast<f64>(c) / 2.0;
    m_Scene->GetStreamingSettings().MaxResidentMegabytes = BytesToMegabytes(budgetBytes);
    Start();
    const u64 budget = m_Streamer.GetConfig().MaxResidentBytes;
    ASSERT_GE(budget, a + b);
    ASSERT_LT(budget, a + b + c);
    ASSERT_GT(huge, budget);

    EXPECT_EQ(Region(11)->m_EstimatedSize, FAssetByteSize::Estimate(a)) << "a region's estimate is its file size, known before any load";

    EXPECT_EQ(m_Streamer.LoadRegion(RegionID{ 11 }), EStreamingAdmission::Admitted);
    EXPECT_EQ(m_Streamer.LoadRegion(RegionID{ 12 }), EStreamingAdmission::Admitted);
    EXPECT_EQ(m_Streamer.LoadRegion(RegionID{ 13 }), EStreamingAdmission::Deferred);
    EXPECT_EQ(Region(13)->m_AdmissionStatus, EStreamingAdmissionStatus::Deferred);
    EXPECT_EQ(Region(13)->m_AdmissionReason, EStreamingAdmissionReason::ResidentBudget);
    EXPECT_EQ(Region(13)->m_State, StreamingRegion::State::Unloaded);

    EXPECT_EQ(m_Streamer.LoadRegion(RegionID{ 14 }), EStreamingAdmission::Rejected);
    EXPECT_EQ(Region(14)->m_AdmissionReason, EStreamingAdmissionReason::LargerThanResidentBudget);

    // Loading regions already count: the pending total is what was admitted.
    FSceneStreamingStats stats = m_Streamer.GetStats();
    EXPECT_EQ(stats.PendingBytes.KnownBytes, a + b);
    EXPECT_EQ(stats.DeferredRegions, 1u);
    EXPECT_EQ(stats.RejectedRegions, 1u);
    EXPECT_EQ(stats.DeferredRequests, 1u);
    EXPECT_EQ(stats.RejectedRequests, 1u);

    ASSERT_TRUE(TickUntil([this]
                          { return m_Streamer.GetLoadedRegionCount() == 2; }));
    stats = m_Streamer.GetStats();
    EXPECT_EQ(stats.ResidentBytes.KnownBytes, a + b);
    EXPECT_EQ(stats.ResidentBytes.EstimateBytes, a + b);
    EXPECT_EQ(stats.ResidentBytes.UnknownCount, 0u);
    EXPECT_LE(stats.ResidentBytes.KnownBytes, budget);

    // Make room: once a region unloads, the deferred one is admitted.
    m_Streamer.UnloadRegion(RegionID{ 11 });
    EXPECT_EQ(m_Streamer.LoadRegion(RegionID{ 13 }), EStreamingAdmission::Admitted);
    EXPECT_EQ(Region(13)->m_AdmissionStatus, EStreamingAdmissionStatus::None);
}

TEST_F(SceneStreamerBudgetTest, PerFrameBudgetAdmitsOneRegionThenDefersUntilTheNextFrame)
{
    const u64 a = WriteRegion(m_Directory, 21, 2101, 1000);
    const u64 b = WriteRegion(m_Directory, 22, 2201, 1000);
    const u64 big = WriteRegion(m_Directory, 23, 2301, 5000);

    m_Scene->GetStreamingSettings().MaxAdmittedMegabytesPerFrame = BytesToMegabytes(static_cast<f64>(a) * 1.5);
    Start();
    Tick();
    ASSERT_LT(m_Streamer.GetConfig().MaxAdmittedBytesPerFrame, a + b);
    ASSERT_GT(big, m_Streamer.GetConfig().MaxAdmittedBytesPerFrame);

    EXPECT_EQ(m_Streamer.LoadRegion(RegionID{ 21 }), EStreamingAdmission::Admitted);
    EXPECT_EQ(m_Streamer.LoadRegion(RegionID{ 22 }), EStreamingAdmission::Deferred);
    EXPECT_EQ(Region(22)->m_AdmissionReason, EStreamingAdmissionReason::FrameBudget);
    EXPECT_EQ(m_Streamer.GetStats().AdmittedBytesThisFrame, a);

    // A new frame resets the per-frame total, and the streamer retries the standing
    // manual request itself.
    Tick();
    EXPECT_NE(Region(22)->m_State, StreamingRegion::State::Unloaded);
    EXPECT_EQ(Region(22)->m_AdmissionStatus, EStreamingAdmissionStatus::None);
    EXPECT_EQ(m_Streamer.GetStats().AdmittedBytesThisFrame, a);

    Tick();
    EXPECT_EQ(m_Streamer.LoadRegion(RegionID{ 23 }), EStreamingAdmission::Admitted)
        << "the first region of a frame is admitted even when it alone exceeds the per-frame budget";
}

TEST_F(SceneStreamerBudgetTest, AnUnknownSizeIsAdmittedVisiblyAndCountedAsUnknown)
{
    WriteRegion(m_Directory, 31, 3101, 1000);
    m_Scene->GetStreamingSettings().MaxResidentMegabytes = 1.0f;
    Start();

    // Nothing on disk produces an unknown size here, so plant one the way an
    // unreadable file size would leave it.
    Region(31)->m_EstimatedSize = FAssetByteSize::Unknown();

    EXPECT_EQ(m_Streamer.LoadRegion(RegionID{ 31 }), EStreamingAdmission::Admitted);
    EXPECT_EQ(m_Streamer.GetStats().AdmittedUnknownSize, 1u);
    ASSERT_TRUE(TickUntil([this]
                          { return m_Streamer.GetLoadedRegionCount() == 1; }));

    const FSceneStreamingStats stats = m_Streamer.GetStats();
    EXPECT_EQ(stats.ResidentBytes.Count, 1u);
    EXPECT_EQ(stats.ResidentBytes.UnknownCount, 1u);
    EXPECT_EQ(stats.ResidentBytes.KnownBytes, 0u);
    EXPECT_FALSE(stats.ResidentBytes.IsComplete()) << "an unknown region must not read as a zero-byte one";
}

// -----------------------------------------------------------------------------
// Eviction consults bytes as well as the region count
// -----------------------------------------------------------------------------
TEST_F(SceneStreamerBudgetTest, LoweringTheByteBudgetEvictsLeastRecentlyUsedRegions)
{
    const u64 a = WriteRegion(m_Directory, 41, 4101, 1000);
    const u64 b = WriteRegion(m_Directory, 42, 4201, 1000);
    const u64 c = WriteRegion(m_Directory, 43, 4301, 1000);
    Start();

    for (u64 id : { 41ull, 42ull, 43ull })
        ASSERT_EQ(m_Streamer.LoadRegion(RegionID{ id }), EStreamingAdmission::Admitted);
    ASSERT_TRUE(TickUntil([this]
                          { return m_Streamer.GetLoadedRegionCount() == 3; }));
    ASSERT_EQ(m_Streamer.GetStats().ResidentBytes.KnownBytes, a + b + c);

    // Region 43 is the most recently used; 41 the least.
    Region(41)->m_LastUsedFrame = 1;
    Region(42)->m_LastUsedFrame = 2;
    Region(43)->m_LastUsedFrame = 3;

    // Now fits only one region.
    m_Scene->GetStreamingSettings().MaxResidentMegabytes = BytesToMegabytes(static_cast<f64>(c) * 1.5);
    Tick();

    const FSceneStreamingStats stats = m_Streamer.GetStats();
    EXPECT_EQ(stats.LoadedRegions, 1u);
    EXPECT_EQ(stats.EvictedForBytes, 2u);
    EXPECT_EQ(stats.EvictedForCount, 0u);
    EXPECT_LE(stats.ResidentBytes.KnownBytes, stats.MaxResidentBytes);
    EXPECT_EQ(Region(43)->m_State, StreamingRegion::State::Ready) << "the most recently used region survives";
    EXPECT_FALSE(EntityExists(4101));
    EXPECT_FALSE(EntityExists(4201));
    EXPECT_TRUE(EntityExists(4301));
}

TEST_F(SceneStreamerBudgetTest, TheRegionCountStillEvictsAlongsideTheByteBudget)
{
    WriteRegion(m_Directory, 51, 5101);
    WriteRegion(m_Directory, 52, 5201);
    m_Scene->GetStreamingSettings().MaxResidentMegabytes = 64.0f; // generous: the count binds first
    Start();

    ASSERT_EQ(m_Streamer.LoadRegion(RegionID{ 51 }), EStreamingAdmission::Admitted);
    ASSERT_EQ(m_Streamer.LoadRegion(RegionID{ 52 }), EStreamingAdmission::Admitted);
    ASSERT_TRUE(TickUntil([this]
                          { return m_Streamer.GetLoadedRegionCount() == 2; }));

    m_Scene->GetStreamingSettings().MaxLoadedRegions = 1;
    Tick();
    EXPECT_EQ(m_Streamer.GetLoadedRegionCount(), 1u);
    EXPECT_EQ(m_Streamer.GetStats().EvictedForCount, 1u);
    EXPECT_EQ(m_Streamer.GetStats().EvictedForBytes, 0u);
}

// -----------------------------------------------------------------------------
// Review follow-ups: requests that must not be lost, room that must be made,
// and re-entry from a streamed entity's OnCreate
// -----------------------------------------------------------------------------
TEST_F(SceneStreamerBudgetTest, ADeferredManualLoadIsRetriedUntilAdmitted)
{
    const u64 a = WriteRegion(m_Directory, 61, 6101, 1000);
    WriteRegion(m_Directory, 62, 6201, 1000);
    m_Scene->GetStreamingSettings().MaxAdmittedMegabytesPerFrame = BytesToMegabytes(static_cast<f64>(a) * 1.5);
    Start();
    Tick();

    ASSERT_EQ(m_Streamer.LoadRegion(RegionID{ 61 }), EStreamingAdmission::Admitted);
    ASSERT_EQ(m_Streamer.LoadRegion(RegionID{ 62 }), EStreamingAdmission::Deferred);

    // No second LoadRegion: the streamer itself must ask again on a later frame.
    ASSERT_TRUE(TickUntil([this]
                          { return Region(62)->m_State == StreamingRegion::State::Ready; }))
        << "a deferred manual request was dropped instead of retried";
    EXPECT_TRUE(EntityExists(6201));
    EXPECT_EQ(Region(62)->m_AdmissionStatus, EStreamingAdmissionStatus::None);
}

TEST_F(SceneStreamerBudgetTest, UnloadingADeferredManualRegionWithdrawsTheRequest)
{
    const u64 a = WriteRegion(m_Directory, 63, 6301, 1000);
    WriteRegion(m_Directory, 64, 6401, 1000);
    m_Scene->GetStreamingSettings().MaxAdmittedMegabytesPerFrame = BytesToMegabytes(static_cast<f64>(a) * 1.5);
    Start();
    Tick();

    ASSERT_EQ(m_Streamer.LoadRegion(RegionID{ 63 }), EStreamingAdmission::Admitted);
    ASSERT_EQ(m_Streamer.LoadRegion(RegionID{ 64 }), EStreamingAdmission::Deferred);
    m_Streamer.UnloadRegion(RegionID{ 64 });

    for (int i = 0; i < 5; ++i)
        Tick();
    EXPECT_EQ(Region(64)->m_State, StreamingRegion::State::Unloaded);
    EXPECT_FALSE(EntityExists(6401));
}

TEST_F(SceneStreamerBudgetTest, DeferredDemandFreesARegionHeldOnlyByHysteresis)
{
    // A at x=0 and B at x=55, each with load radius 10 and unload radius 60. The
    // budget fits one region. Standing at x=50, B is inside its load radius and A
    // is only inside its unload radius: A is kept by hysteresis, B is wanted.
    const u64 a = WriteRegion(m_Directory, 71, 7101, 1000);
    const u64 b = WriteRegion(m_Directory, 72, 7201, 1000);
    auto addVolume = [this](u64 regionId, f32 x)
    {
        Entity volume = m_Scene->CreateEntity("volume_" + std::to_string(regionId));
        volume.GetComponent<TransformComponent>().Translation = glm::vec3{ x, 0.0f, 0.0f };
        auto& vol = volume.AddComponent<StreamingVolumeComponent>();
        vol.RegionAssetHandle = AssetHandle{ regionId };
        vol.LoadRadius = 10.0f;
        vol.UnloadRadius = 60.0f;
    };
    addVolume(71, 0.0f);
    addVolume(72, 55.0f);
    m_Scene->GetStreamingSettings().MaxResidentMegabytes = BytesToMegabytes(static_cast<f64>(std::max(a, b)) * 1.5);
    Start();

    auto tickAt = [this](f32 x)
    { m_Streamer.Update(glm::vec3{ x, 0.0f, 0.0f }, ++m_Frame); };
    const auto deadline = std::chrono::steady_clock::now() + kLoadHookFailAfter;
    while (Region(71)->m_State != StreamingRegion::State::Ready && std::chrono::steady_clock::now() < deadline)
        tickAt(0.0f);
    ASSERT_EQ(Region(71)->m_State, StreamingRegion::State::Ready);

    // Walk into B while still inside A's unload radius.
    while (Region(72)->m_State != StreamingRegion::State::Ready && std::chrono::steady_clock::now() < deadline)
        tickAt(50.0f);

    EXPECT_EQ(Region(72)->m_State, StreamingRegion::State::Ready) << "the region the player needs was starved by one held only by hysteresis";
    EXPECT_EQ(Region(71)->m_State, StreamingRegion::State::Unloaded);
    const FSceneStreamingStats stats = m_Streamer.GetStats();
    EXPECT_EQ(stats.EvictedForBytes, 1u);
    EXPECT_LE(stats.ResidentBytes.KnownBytes, stats.MaxResidentBytes);
}

TEST_F(SceneStreamerBudgetTest, ReEnteringFromOnCreateNeitherCorruptsNorLosesRequests)
{
    WriteRegion(m_Directory, 81, 8101);
    WriteRegion(m_Directory, 82, 8201);
    WriteRegion(m_Directory, 83, 8301);
    Start();

    // What a streamed entity's OnCreate can do: unload its own region while it is
    // being instantiated, and request another one.
    FRegionLoadTestHooks hooks;
    hooks.OnRegionInstantiated = [this](RegionID id)
    {
        if (id == RegionID{ 81 })
        {
            m_Streamer.UnloadRegion(RegionID{ 81 });
            EXPECT_EQ(m_Streamer.LoadRegion(RegionID{ 83 }), EStreamingAdmission::Admitted);
        }
    };
    m_Streamer.SetTestHooks(hooks);

    ASSERT_EQ(m_Streamer.LoadRegion(RegionID{ 81 }), EStreamingAdmission::Admitted);
    ASSERT_EQ(m_Streamer.LoadRegion(RegionID{ 82 }), EStreamingAdmission::Admitted);
    ASSERT_TRUE(TickUntil([this]
                          { return Region(82)->m_State == StreamingRegion::State::Ready &&
                                   Region(83)->m_State == StreamingRegion::State::Ready; }));

    EXPECT_EQ(Region(81)->m_State, StreamingRegion::State::Unloaded) << "the unload asked for during instantiation was lost";
    EXPECT_FALSE(EntityExists(8101));
    EXPECT_TRUE(EntityExists(8201));
    EXPECT_TRUE(EntityExists(8301));
    EXPECT_EQ(m_Streamer.GetPendingLoadCount(), 0u);
}

TEST_F(SceneStreamerBudgetTest, AnUnknownSizeUnderOnlyAFrameBudgetIsStillCounted)
{
    WriteRegion(m_Directory, 91, 9101, 1000);
    m_Scene->GetStreamingSettings().MaxAdmittedMegabytesPerFrame = 1.0f;
    Start();
    Region(91)->m_EstimatedSize = FAssetByteSize::Unknown();

    EXPECT_EQ(m_Streamer.LoadRegion(RegionID{ 91 }), EStreamingAdmission::Admitted);
    EXPECT_EQ(m_Streamer.GetStats().AdmittedUnknownSize, 1u) << "an unknown size slipped past the frame budget silently";
}

// -----------------------------------------------------------------------------
// StreamingSettings in scene YAML
// -----------------------------------------------------------------------------
TEST(StreamingSettingsYaml, ByteBudgetsRoundTrip)
{
    Ref<Scene> scene = Scene::Create();
    scene->GetStreamingSettings().MaxResidentMegabytes = 384.5f;
    scene->GetStreamingSettings().MaxAdmittedMegabytesPerFrame = 12.25f;

    const std::string yaml = SceneSerializer(scene).SerializeToYAML();
    ASSERT_NE(yaml.find("MaxResidentMegabytes"), std::string::npos);

    Ref<Scene> loaded = Scene::Create();
    ASSERT_TRUE(SceneSerializer(loaded).DeserializeFromYAML(yaml));
    EXPECT_FLOAT_EQ(loaded->GetStreamingSettings().MaxResidentMegabytes, 384.5f);
    EXPECT_FLOAT_EQ(loaded->GetStreamingSettings().MaxAdmittedMegabytesPerFrame, 12.25f);

    // The file path writes the same block as the string path.
    const fs::path path = Tests::TempFile("streaming_budget_roundtrip.olo");
    SceneSerializer(scene).Serialize(path);
    Ref<Scene> fromFile = Scene::Create();
    ASSERT_TRUE(SceneSerializer(fromFile).Deserialize(path));
    EXPECT_FLOAT_EQ(fromFile->GetStreamingSettings().MaxResidentMegabytes, 384.5f);
    EXPECT_FLOAT_EQ(fromFile->GetStreamingSettings().MaxAdmittedMegabytesPerFrame, 12.25f);
    std::error_code ec;
    fs::remove(path, ec);
}

TEST(StreamingSettingsYaml, ASceneSavedBeforeTheByteBudgetLoadsWithNoBudget)
{
    // The StreamingSettings block exactly as scenes were written before #1365, at
    // the current scene version: the new keys are optional, so no version bump.
    const std::string kPreBudgetScene =
        "Scene: PreBudget\n"
        "Version: " +
        std::to_string(SceneSerializer::CurrentVersion) + "\n"
                                                          "StreamingSettings:\n"
                                                          "  Enabled: true\n"
                                                          "  DefaultLoadRadius: 150\n"
                                                          "  DefaultUnloadRadius: 180\n"
                                                          "  MaxLoadedRegions: 5\n"
                                                          "  RegionDirectory: Regions/World\n"
                                                          "Entities: []\n";

    Ref<Scene> scene = Scene::Create();
    ASSERT_TRUE(SceneSerializer(scene).DeserializeFromYAML(kPreBudgetScene));
    const StreamingSettings& ss = scene->GetStreamingSettings();
    EXPECT_TRUE(ss.Enabled);
    EXPECT_FLOAT_EQ(ss.DefaultLoadRadius, 150.0f);
    EXPECT_EQ(ss.MaxLoadedRegions, 5u);
    EXPECT_EQ(ss.RegionDirectory, "Regions/World");
    EXPECT_FLOAT_EQ(ss.MaxResidentMegabytes, 0.0f) << "absent means no byte budget";
    EXPECT_FLOAT_EQ(ss.MaxAdmittedMegabytesPerFrame, 0.0f);
    EXPECT_EQ(MakeSceneStreamerConfig(ss).MaxResidentBytes, 0u);
}

TEST(StreamingSettingsYaml, CorruptBudgetsAreSanitisedOnLoad)
{
    auto load = [](const std::string& resident, const std::string& perFrame)
    {
        const std::string yaml = "Scene: Corrupt\n"
                                 "Version: " +
                                 std::to_string(SceneSerializer::CurrentVersion) +
                                 "\n"
                                 "StreamingSettings:\n"
                                 "  MaxResidentMegabytes: " +
                                 resident + "\n  MaxAdmittedMegabytesPerFrame: " + perFrame + "\nEntities: []\n";
        Ref<Scene> scene = Scene::Create();
        EXPECT_TRUE(SceneSerializer(scene).DeserializeFromYAML(yaml));
        return scene->GetStreamingSettings();
    };

    StreamingSettings nan = load(".nan", ".inf");
    EXPECT_FLOAT_EQ(nan.MaxResidentMegabytes, 0.0f) << "NaN is no budget, not a tiny one";
    EXPECT_FLOAT_EQ(nan.MaxAdmittedMegabytesPerFrame, 0.0f);

    StreamingSettings negative = load("-64", "-1");
    EXPECT_FLOAT_EQ(negative.MaxResidentMegabytes, 0.0f);
    EXPECT_FLOAT_EQ(negative.MaxAdmittedMegabytesPerFrame, 0.0f);

    StreamingSettings huge = load("1e7", "3e38");
    EXPECT_FLOAT_EQ(huge.MaxResidentMegabytes, kMaxStreamingBudgetMegabytes);
    EXPECT_FLOAT_EQ(huge.MaxAdmittedMegabytesPerFrame, kMaxStreamingBudgetMegabytes);
    EXPECT_EQ(StreamingBudgetMegabytesToBytes(huge.MaxResidentMegabytes), 1024ull * 1024ull * 1024ull * 1024ull);
}
