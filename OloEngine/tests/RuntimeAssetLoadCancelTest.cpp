// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "TestAssetPackWriter.h"
#include "TestAsyncLoadHooks.h"
#include "TestTempDir.h"

// =============================================================================
// RuntimeAssetLoadCancelTest — issue #1365 on the cooked (asset pack) path.
//
// Pins RuntimeAssetSystem's cancellation contract and the byte accounting that
// RuntimeAssetManager reports, against a real pack read on a real worker:
//
//   - a request cancelled while genuinely queued (held behind a task-event
//     prerequisite) never reads the pack;
//   - a request cancelled while the worker is mid-load (blocked on an event in
//     the load hook, before or after the real pack read) finishes, and its
//     result is dropped: never integrated, never retrieved;
//   - a finished-but-unretrieved result is discarded by cancel;
//   - StopAndWait (manager Shutdown) waits for an abandoned load that is still
//     running, so no load outlives the manager it calls back into;
//   - an unhooked queue-then-cancel race, repeated, never integrates anything;
//   - sizes: the pack's packed size is an Estimate before and during the load,
//     the measured size is Actual once resident, and a size nobody knows is
//     Unknown (no bytes), not zero.
//
// The hooks block on events, never on sleeps: a test that only cancels a quiet
// queue proves nothing about the race. Timeouts are failure bounds, not
// synchronisation.
//
// What this does NOT cover: the loose (region) path, which is
// Streaming/SceneStreamerBudgetTest.cpp.
// =============================================================================

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Asset/AssetByteSize.h"
#include "OloEngine/Asset/AssetManager/RuntimeAssetManager.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
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

    // AudioFileSourceSerializer::DeserializeFromAssetPack reads one string, the
    // source path: [u64 length][bytes]. The file does not exist, so the load
    // degrades to default metadata — still a real deserialize producing a real asset.
    Tests::PackEntry MakeAudioEntry(AssetHandle handle, const std::string& sourcePath)
    {
        Tests::PackEntry entry;
        entry.Handle = handle;
        entry.Type = AssetType::Audio;
        const u64 length = static_cast<u64>(sourcePath.size());
        entry.Data.append(reinterpret_cast<const char*>(&length), sizeof(length));
        entry.Data.append(sourcePath);
        return entry;
    }

    // Pump the main-thread sync until `done` holds; false on timeout.
    bool PumpUntil(RuntimeAssetManager& mgr, const std::function<bool()>& done)
    {
        const auto deadline = std::chrono::steady_clock::now() + kLoadHookFailAfter;
        while (std::chrono::steady_clock::now() < deadline)
        {
            mgr.SyncWithAssetThread();
            if (done())
                return true;
            std::this_thread::yield();
        }
        return false;
    }

    class RuntimeAssetLoadCancelTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            Tests::EnsureTaskSchedulerStarted();
            m_PackPath = Tests::TempFile("cancel.olopack"); // TempFile is already per test

            // kNoSize is indexed with a loadable type but a zero packed size: no estimate exists.
            Tests::PackEntry noSize;
            noSize.Handle = kNoSize;
            noSize.Type = AssetType::Audio;
            Tests::WritePack(m_PackPath, { MakeAudioEntry(kAudio, kAudioSource), MakeAudioEntry(kSecondAudio, kSecondSource), noSize });

            m_Manager = std::make_unique<RuntimeAssetManager>(/*autoLoadDefaultPack=*/false);
            ASSERT_TRUE(m_Manager->LoadAssetPack(m_PackPath));
        }

        void TearDown() override
        {
            // Let every blocked worker and gated task go before shutting down, so a
            // failed assertion ends the test instead of hanging it.
            m_Hook->Release.Set();
            for (const Tasks::FTaskEvent& gate : m_Gates)
                gate.Trigger();
            m_Manager.reset();
            std::error_code ec;
            fs::remove(m_PackPath, ec);
        }

        [[nodiscard]] static u64 PackedSizeOf(AssetHandle handle)
        {
            return handle == kAudio ? MakeAudioEntry(kAudio, kAudioSource).Data.size()
                                    : MakeAudioEntry(kSecondAudio, kSecondSource).Data.size();
        }

        [[nodiscard]] FRuntimeAssetLoadStats Loads() const
        {
            return m_Manager->GetStreamingReport().Loads;
        }

        // A start gate that TearDown releases even if the test body bailed out.
        Tasks::FTaskEvent MakeGate(const char* name)
        {
            m_Gates.emplace_back(name);
            return m_Gates.back();
        }

        static inline const AssetHandle kAudio{ 0xCA7C0001ULL };
        static inline const AssetHandle kSecondAudio{ 0xCA7C0002ULL };
        static inline const AssetHandle kNoSize{ 0xCA7C0003ULL };
        static inline const std::string kAudioSource = "sounds/cancel.wav";
        static inline const std::string kSecondSource = "sounds/other.wav";

        std::shared_ptr<LoadHookState> m_Hook = Tests::MakeLoadHookState();
        fs::path m_PackPath;
        std::unique_ptr<RuntimeAssetManager> m_Manager;
        std::vector<Tasks::FTaskEvent> m_Gates;
    };
} // namespace

// -----------------------------------------------------------------------------
// Queued, not started: the load body never reads the pack.
// -----------------------------------------------------------------------------
TEST_F(RuntimeAssetLoadCancelTest, CancelBeforeStartNeverReadsThePack)
{
    Tasks::FTaskEvent gate = MakeGate("CancelBeforeStartGate");
    FRuntimeAssetLoadTestHooks hooks;
    hooks.StartGate = gate;
    hooks.OnLoadStarted = [hook = m_Hook](AssetHandle)
    { hook->Started.fetch_add(1); };
    m_Manager->SetAsyncLoadTestHooks(std::move(hooks));

    ASSERT_FALSE(m_Manager->GetAssetAsync(kAudio).IsReady) << "Audio is off-thread-safe, so the request must queue";
    ASSERT_EQ(Loads().PendingCount, 1u);

    EXPECT_EQ(m_Manager->CancelAssetLoad(kAudio), EAssetLoadCancelResult::CancelledBeforeStart);
    EXPECT_EQ(Loads().PendingCount, 0u);
    EXPECT_EQ(m_Manager->CancelAssetLoad(kAudio), EAssetLoadCancelResult::NotPending) << "a second cancel finds nothing";

    // Release the task: it runs its body, sees the Cancelled ticket and returns.
    gate.Trigger();
    ASSERT_TRUE(PumpUntil(*m_Manager, [this]
                          { return Loads().AbandonedRunningCount == 0; }));
    m_Manager->SyncWithAssetThread();

    EXPECT_EQ(m_Hook->Started.load(), 0) << "a load cancelled before start must never reach the pack read";
    EXPECT_FALSE(m_Manager->IsAssetLoaded(kAudio));
    const FRuntimeAssetLoadStats stats = Loads();
    EXPECT_EQ(stats.CancelledBeforeStart, 1u);
    EXPECT_EQ(stats.AbandonedInFlight, 0u);
    EXPECT_EQ(stats.AbandonedResultsDropped, 0u) << "nothing ran, so nothing was produced to drop";
}

// -----------------------------------------------------------------------------
// The real race: the worker is inside the load when the cancel arrives.
// -----------------------------------------------------------------------------
enum class EAssetBlockAt
{
    BeforePackRead,
    AfterPackRead
};

class RuntimeAssetMidLoadCancel : public RuntimeAssetLoadCancelTest, public ::testing::WithParamInterface<EAssetBlockAt>
{
};

TEST_P(RuntimeAssetMidLoadCancel, TheResultIsDroppedNeverIntegrated)
{
    const bool blockBeforeRead = GetParam() == EAssetBlockAt::BeforePackRead;
    FRuntimeAssetLoadTestHooks hooks;
    if (blockBeforeRead)
    {
        hooks.OnLoadStarted = [hook = m_Hook](AssetHandle)
        { hook->Block(); };
    }
    hooks.OnLoadFinished = [hook = m_Hook, blockBeforeRead](AssetHandle, const Ref<Asset>& asset)
    {
        hook->ProducedResult.store(static_cast<bool>(asset));
        if (!blockBeforeRead)
            hook->Block();
    };
    m_Manager->SetAsyncLoadTestHooks(std::move(hooks));

    ASSERT_FALSE(m_Manager->GetAssetAsync(kAudio).IsReady);
    ASSERT_TRUE(m_Hook->WorkerInside.WaitFor()) << "the worker never entered the load";

    // The worker is blocked inside the load body. An unfinished load is not retrieved.
    m_Manager->SyncWithAssetThread();
    ASSERT_FALSE(m_Manager->IsAssetLoaded(kAudio));

    EXPECT_EQ(m_Manager->CancelAssetLoad(kAudio), EAssetLoadCancelResult::AbandonedInFlight);
    EXPECT_EQ(Loads().PendingCount, 0u);
    EXPECT_EQ(Loads().AbandonedRunningCount, 1u);

    m_Hook->Release.Set();
    ASSERT_TRUE(PumpUntil(*m_Manager, [this]
                          { return Loads().AbandonedResultsDropped == 1; }))
        << "the abandoned task never finished";

    EXPECT_TRUE(m_Hook->ProducedResult.load()) << "the pack read must really have produced an asset, or nothing was dropped";
    EXPECT_FALSE(m_Manager->IsAssetLoaded(kAudio)) << "a result arriving after cancellation must never be integrated";
    EXPECT_EQ(Loads().AbandonedRunningCount, 0u);
    EXPECT_EQ(Loads().AbandonedInFlight, 1u);
}

INSTANTIATE_TEST_SUITE_P(BlockPoint, RuntimeAssetMidLoadCancel,
                         ::testing::Values(EAssetBlockAt::BeforePackRead, EAssetBlockAt::AfterPackRead),
                         [](const ::testing::TestParamInfo<EAssetBlockAt>& info)
                         { return info.param == EAssetBlockAt::BeforePackRead ? std::string("BeforePackRead") : std::string("AfterPackRead"); });

// -----------------------------------------------------------------------------
// Finished but not yet retrieved: cancel releases the result.
// -----------------------------------------------------------------------------
TEST_F(RuntimeAssetLoadCancelTest, CancelOfACompletedUnretrievedLoadDiscardsIt)
{
    ASSERT_FALSE(m_Manager->GetAssetAsync(kAudio).IsReady);

    // Wait WITHOUT SyncWithAssetThread, so the finished result stays unretrieved.
    const auto deadline = std::chrono::steady_clock::now() + kLoadHookFailAfter;
    while (Loads().CompletedUnretrievedCount == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    ASSERT_EQ(Loads().CompletedUnretrievedCount, 1u);

    EXPECT_EQ(m_Manager->CancelAssetLoad(kAudio), EAssetLoadCancelResult::DiscardedCompleted);
    m_Manager->SyncWithAssetThread();
    EXPECT_FALSE(m_Manager->IsAssetLoaded(kAudio));
    EXPECT_EQ(Loads().DiscardedCompleted, 1u);
    EXPECT_EQ(Loads().PendingCount, 0u);
}

// -----------------------------------------------------------------------------
// Re-request while the abandoned predecessor still runs: only the new one lands.
// -----------------------------------------------------------------------------
TEST_F(RuntimeAssetLoadCancelTest, RequeueAfterAbandonLoadsFreshAndDropsTheOldResult)
{
    FRuntimeAssetLoadTestHooks hooks;
    hooks.OnLoadStarted = [hook = m_Hook](AssetHandle)
    {
        if (hook->Started.fetch_add(1) == 0)
            hook->Block(); // only the first load blocks
    };
    m_Manager->SetAsyncLoadTestHooks(std::move(hooks));

    ASSERT_FALSE(m_Manager->GetAssetAsync(kAudio).IsReady);
    ASSERT_TRUE(m_Hook->WorkerInside.WaitFor());
    ASSERT_EQ(m_Manager->CancelAssetLoad(kAudio), EAssetLoadCancelResult::AbandonedInFlight);

    // The same handle again, while the first worker is still blocked.
    ASSERT_FALSE(m_Manager->GetAssetAsync(kAudio).IsReady);
    ASSERT_TRUE(PumpUntil(*m_Manager, [this]
                          { return m_Manager->IsAssetLoaded(kAudio); }))
        << "the second request must load even though the first is still running";
    EXPECT_EQ(Loads().AbandonedRunningCount, 1u) << "the first load is still blocked";

    m_Hook->Release.Set();
    ASSERT_TRUE(PumpUntil(*m_Manager, [this]
                          { return Loads().AbandonedResultsDropped == 1; }));
    EXPECT_EQ(m_Hook->Started.load(), 2);
    EXPECT_TRUE(m_Manager->IsAssetLoaded(kAudio));
}

// -----------------------------------------------------------------------------
// Shutdown with a cancellation in flight: StopAndWait waits for it.
// -----------------------------------------------------------------------------
TEST_F(RuntimeAssetLoadCancelTest, ShutdownWaitsForAnAbandonedLoadStillRunning)
{
    FRuntimeAssetLoadTestHooks hooks;
    hooks.OnLoadFinished = [hook = m_Hook](AssetHandle, const Ref<Asset>&)
    {
        hook->Block();
        hook->WorkerExitedAt.store(hook->Sequence.fetch_add(1));
    };
    m_Manager->SetAsyncLoadTestHooks(std::move(hooks));

    ASSERT_FALSE(m_Manager->GetAssetAsync(kAudio).IsReady);
    ASSERT_TRUE(m_Hook->WorkerInside.WaitFor());
    ASSERT_EQ(m_Manager->CancelAssetLoad(kAudio), EAssetLoadCancelResult::AbandonedInFlight);

    // Shutdown on another thread: it must block on the abandoned task, which is
    // blocked until the release below.
    std::thread shutdown([this, hook = m_Hook]
                         {
        m_Manager->Shutdown();
        hook->ShutdownReturnedAt.store(hook->Sequence.fetch_add(1)); });

    // A window for a wrong implementation to return early. A correct Shutdown
    // cannot return before the release, so this wait cannot fail it.
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(m_Hook->ShutdownReturnedAt.load(), -1) << "Shutdown returned while an abandoned load was still running";

    m_Hook->Release.Set();
    shutdown.join();

    EXPECT_GE(m_Hook->WorkerExitedAt.load(), 0);
    EXPECT_GE(m_Hook->ShutdownReturnedAt.load(), 0);
    EXPECT_LT(m_Hook->WorkerExitedAt.load(), m_Hook->ShutdownReturnedAt.load())
        << "StopAndWait must not return before every abandoned load has finished";
}

// -----------------------------------------------------------------------------
// No hooks: queue and cancel back to back, many times, so the cancel lands in
// whatever state the worker happens to be in. Nothing may ever be integrated.
// -----------------------------------------------------------------------------
TEST_F(RuntimeAssetLoadCancelTest, UnhookedQueueCancelRaceNeverIntegrates)
{
    constexpr int kIterations = 300;
    for (int i = 0; i < kIterations; ++i)
    {
        ASSERT_FALSE(m_Manager->GetAssetAsync(kAudio).IsReady);
        // Give the worker a chance to be anywhere: not started, mid-load, or done.
        for (int spin = 0; spin < (i % 7) * 50; ++spin)
            std::this_thread::yield();
        ASSERT_NE(m_Manager->CancelAssetLoad(kAudio), EAssetLoadCancelResult::NotPending)
            << "nothing retrieves between queue and cancel";
        m_Manager->SyncWithAssetThread();
        ASSERT_FALSE(m_Manager->IsAssetLoaded(kAudio)) << "iteration " << i << ": a cancelled load was integrated";
    }

    ASSERT_TRUE(PumpUntil(*m_Manager, [this]
                          { return Loads().AbandonedRunningCount == 0; }));
    m_Manager->SyncWithAssetThread();
    EXPECT_FALSE(m_Manager->IsAssetLoaded(kAudio));

    const FRuntimeAssetLoadStats stats = Loads();
    EXPECT_EQ(stats.CancelledBeforeStart + stats.AbandonedInFlight + stats.DiscardedCompleted, static_cast<u64>(kIterations));
    // The mix, so a run that never actually raced is visible in the log.
    std::printf("[race mix] beforeStart=%llu inFlight=%llu completed=%llu\n",
                static_cast<unsigned long long>(stats.CancelledBeforeStart),
                static_cast<unsigned long long>(stats.AbandonedInFlight),
                static_cast<unsigned long long>(stats.DiscardedCompleted));
}

// -----------------------------------------------------------------------------
// Byte accounting: Estimate before completion, Actual once resident, Unknown
// when nobody knows — never zero.
// -----------------------------------------------------------------------------
TEST_F(RuntimeAssetLoadCancelTest, SizeIsAnEstimateBeforeCompletionAndActualOnceResident)
{
    const u64 packed = PackedSizeOf(kAudio);

    // Not requested yet: the pack's packed size, so a caller can budget first.
    EXPECT_EQ(m_Manager->GetAssetByteSize(kAudio), FAssetByteSize::Estimate(packed));

    // Pending (held queued): the same estimate, and it is in the pending total.
    Tasks::FTaskEvent gate = MakeGate("SizeGate");
    FRuntimeAssetLoadTestHooks hooks;
    hooks.StartGate = gate;
    m_Manager->SetAsyncLoadTestHooks(std::move(hooks));
    ASSERT_FALSE(m_Manager->GetAssetAsync(kAudio).IsReady);
    EXPECT_EQ(m_Manager->GetAssetByteSize(kAudio), FAssetByteSize::Estimate(packed));
    {
        const FRuntimeAssetStreamingReport report = m_Manager->GetStreamingReport();
        EXPECT_EQ(report.Loads.PendingBytes.Count, 1u);
        EXPECT_EQ(report.Loads.PendingBytes.KnownBytes, packed);
        EXPECT_EQ(report.Loads.PendingBytes.EstimateBytes, packed);
        EXPECT_EQ(report.Loads.PendingBytes.UnknownCount, 0u);
    }

    gate.Trigger();
    ASSERT_TRUE(PumpUntil(*m_Manager, [this]
                          { return m_Manager->IsAssetLoaded(kAudio); }));

    // Resident: AudioFile measures itself, so the figure is now Actual.
    const FAssetByteSize resident = m_Manager->GetAssetByteSize(kAudio);
    EXPECT_TRUE(resident.IsActual());
    EXPECT_EQ(resident, FAssetByteSize::Actual(sizeof(AudioFile)));
    {
        const FRuntimeAssetStreamingReport report = m_Manager->GetStreamingReport();
        EXPECT_EQ(report.Resident.Count, 1u);
        EXPECT_EQ(report.Resident.ActualBytes, sizeof(AudioFile));
        EXPECT_EQ(report.Resident.UnknownCount, 0u);
        EXPECT_EQ(report.Loads.PendingBytes.Count, 0u);
    }
}

TEST_F(RuntimeAssetLoadCancelTest, AnUnknownSizeIsReportedAsUnknownNotZero)
{
    // Indexed with a zero packed size: no figure exists before the load.
    const FAssetByteSize noSize = m_Manager->GetAssetByteSize(kNoSize);
    EXPECT_FALSE(noSize.IsKnown());
    EXPECT_FALSE(noSize.GetBytes().has_value()) << "an unknown size must carry no byte count at all";

    // Not in any pack: equally unknown.
    EXPECT_FALSE(m_Manager->GetAssetByteSize(AssetHandle(0xDEADBEEFULL)).IsKnown());

    // Pending with an unknown estimate: counted as unknown in the pending total.
    Tasks::FTaskEvent gate = MakeGate("UnknownGate");
    FRuntimeAssetLoadTestHooks hooks;
    hooks.StartGate = gate;
    m_Manager->SetAsyncLoadTestHooks(std::move(hooks));
    ASSERT_FALSE(m_Manager->GetAssetAsync(kNoSize).IsReady);
    ASSERT_FALSE(m_Manager->GetAssetAsync(kSecondAudio).IsReady);
    const FRuntimeAssetStreamingReport report = m_Manager->GetStreamingReport();
    EXPECT_EQ(report.Loads.PendingBytes.Count, 2u);
    EXPECT_EQ(report.Loads.PendingBytes.UnknownCount, 1u);
    EXPECT_EQ(report.Loads.PendingBytes.KnownBytes, PackedSizeOf(kSecondAudio));
    EXPECT_FALSE(report.Loads.PendingBytes.IsComplete());
}

// -----------------------------------------------------------------------------
// The tagged size and the total, on their own.
// -----------------------------------------------------------------------------
TEST(AssetByteSize, UnknownHasNoBytesAndTotalsCountIt)
{
    EXPECT_FALSE(FAssetByteSize{}.IsKnown());
    EXPECT_FALSE(FAssetByteSize::Unknown().GetBytes().has_value());
    EXPECT_EQ(FAssetByteSize::Estimate(0).GetBytes(), std::optional<u64>(0u)) << "a known zero is still known";
    EXPECT_NE(FAssetByteSize::Estimate(64), FAssetByteSize::Actual(64)) << "estimate and actual must be distinguishable";

    FAssetByteTotal total;
    total.Add(FAssetByteSize::Estimate(100));
    total.Add(FAssetByteSize::Actual(28));
    total.Add(FAssetByteSize::Unknown());
    EXPECT_EQ(total.Count, 3u);
    EXPECT_EQ(total.KnownBytes, 128u);
    EXPECT_EQ(total.EstimateBytes, 100u);
    EXPECT_EQ(total.ActualBytes, 28u);
    EXPECT_EQ(total.UnknownCount, 1u);
    EXPECT_FALSE(total.IsComplete());
}
