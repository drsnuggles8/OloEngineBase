// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include "OloEngine/Asset/AssetSystem/RepresentationStreaming.h"
#include "TestAsyncLoadHooks.h"

#include <gtest/gtest.h>
#include <chrono>
#include <limits>
#include <thread>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

namespace
{
    class TestRepresentationPayload : public FRepresentationPayload
    {
      public:
        explicit TestRepresentationPayload(u64 identity, u64 bytes = 40)
            : Identity(identity), Bytes(bytes) {}
        ~TestRepresentationPayload() override
        {
            if (OnDestroy)
                OnDestroy();
        }
        u64 GetCpuBytes() const noexcept override
        {
            return Bytes;
        }
        std::optional<FRepresentationIOStats> GetIOStats() const noexcept override
        {
            return IO;
        }
        u64 Identity;
        u64 Bytes;
        std::optional<FRepresentationIOStats> IO;
        std::function<void()> OnDestroy;
    };

    FRepresentationDescriptor Descriptor(u64 cpu = 40, u64 gpu = 60, u64 upload = 30)
    {
        return { FAssetByteSize::Estimate(cpu), FAssetByteSize::Estimate(10), upload, gpu };
    }

    template<typename Predicate>
    bool Until(Predicate predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + Tests::kLoadHookFailAfter;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (predicate())
                return true;
            std::this_thread::yield();
        }
        return predicate();
    }

    class RepresentationStreamingTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            Tests::EnsureTaskSchedulerStarted();
            RepresentationStreaming::Get().Reset();
            RepresentationStreaming::Get().SetStagingBudget(80);
        }
        void TearDown() override
        {
            EXPECT_EQ(RepresentationStreaming::Get().GetStats().StagingCpuBytes, 0u);
            RepresentationStreaming::Get().Reset();
            RepresentationStreaming::Get().SetStagingBudget(0);
        }
    };
} // namespace

TEST_F(RepresentationStreamingTest, CancelledRunningWorkKeepsSharedStagingUntilItFinishes)
{
    FRepresentationLoadQueue first;
    FRepresentationLoadQueue second;
    auto hook = Tests::MakeLoadHookState();
    ASSERT_EQ(first.Request(7, Descriptor(60), [hook]() -> Ref<FRepresentationPayload>
                            {
                                hook->Block();
                                return Ref<TestRepresentationPayload>::Create(7); }),
              ERepresentationRequestResult::Queued);
    ASSERT_TRUE(hook->WorkerInside.WaitFor());
    EXPECT_EQ(first.Cancel(7), EAssetLoadCancelResult::AbandonedInFlight);
    EXPECT_EQ(first.GetStats().StagingBytes, 60u);
    EXPECT_EQ(second.Request(7, Descriptor(), []() -> Ref<FRepresentationPayload>
                             { return Ref<TestRepresentationPayload>::Create(7); }),
              ERepresentationRequestResult::StagingBudget);
    hook->Release.Set();
    ASSERT_TRUE(Until([&first]
                      {
                          first.ReapAbandoned();
                          return first.GetStats().StagingBytes == 0; }));
    EXPECT_FALSE(first.HasFailed(7));
    EXPECT_EQ(first.GetStats().Cancellation.AbandonedResultsDropped, 1u);
}

TEST_F(RepresentationStreamingTest, CancelledQueuedWorkNeverRunsPreparation)
{
    FRepresentationLoadQueue queue;
    Tasks::FTaskEvent gate("RepresentationQueuedCancel");
    queue.SetStartGate(gate);
    auto started = std::make_shared<std::atomic<u32>>(0);
    const auto result = queue.Request(7, Descriptor(), [started]() -> Ref<FRepresentationPayload>
                                      {
                                          ++*started;
                                          return Ref<TestRepresentationPayload>::Create(7); });
    EXPECT_EQ(result, ERepresentationRequestResult::Queued);
    EXPECT_EQ(queue.Cancel(7), EAssetLoadCancelResult::CancelledBeforeStart);
    EXPECT_EQ(queue.GetStats().StagingBytes, 0u); // gate is still closed
    EXPECT_EQ(RepresentationStreaming::Get().GetStats().StagingCpuBytes, 0u);
    // Another owner can immediately use the allowance: only running or ready
    // preparation may retain staging, never an atomically excluded callback.
    const auto replacement = RepresentationStreaming::Get().ReserveStaging(FAssetByteSize::Estimate(80));
    EXPECT_TRUE(replacement);
    if (replacement)
        RepresentationStreaming::Get().ReleaseStaging(*replacement);
    gate.Trigger();
    queue.Shutdown();
    EXPECT_EQ(started->load(), 0u);
    EXPECT_EQ(queue.GetStats().StagingBytes, 0u);
}

TEST_F(RepresentationStreamingTest, CancelCompletedWorkDropsItsPayloadAndReservation)
{
    FRepresentationLoadQueue queue;
    ASSERT_EQ(queue.Request(7, Descriptor(), []() -> Ref<FRepresentationPayload>
                            { return Ref<TestRepresentationPayload>::Create(7); }),
              ERepresentationRequestResult::Queued);
    ASSERT_TRUE(Until([&queue]
                      { return queue.GetStats().CompletedUnretrievedCount == 1; }));
    EXPECT_EQ(queue.Cancel(7), EAssetLoadCancelResult::DiscardedCompleted);
    TArray<FCompletedRepresentationLoad> results;
    queue.RetrieveCompleted(results);
    EXPECT_TRUE(results.IsEmpty());
    EXPECT_EQ(queue.GetStats().StagingBytes, 0u);
}

TEST_F(RepresentationStreamingTest, RetrievedReadyPayloadStillConsumesSharedStaging)
{
    FRepresentationLoadQueue queue;
    ASSERT_EQ(queue.Request(7, Descriptor(), []() -> Ref<FRepresentationPayload>
                            { return Ref<TestRepresentationPayload>::Create(7); }),
              ERepresentationRequestResult::Queued);
    TArray<FCompletedRepresentationLoad> results;
    ASSERT_TRUE(Until([&]
                      {
                          queue.RetrieveCompleted(results);
                          return results.Num() == 1; }));
    EXPECT_EQ(queue.GetStats().HeldCompletionCount, 1u);
    EXPECT_EQ(queue.GetStats().StagingBytes, 40u);
    EXPECT_EQ(queue.Request(8, Descriptor(60), []() -> Ref<FRepresentationPayload>
                            { return Ref<TestRepresentationPayload>::Create(8); }),
              ERepresentationRequestResult::StagingBudget);
    const u64 ticket = results[0].StagingTicket;
    results.Reset();
    queue.ReleaseStaging(ticket);
    EXPECT_EQ(queue.GetStats().HeldCompletionCount, 0u);
}

TEST_F(RepresentationStreamingTest, UnknownAndUnderestimatedPreparationDoNotPublishUnboundedPayloads)
{
    FRepresentationLoadQueue queue;
    auto unknown = Descriptor();
    unknown.CpuBytes = FAssetByteSize::Unknown();
    EXPECT_EQ(queue.Request(7, unknown, []() -> Ref<FRepresentationPayload>
                            { return Ref<TestRepresentationPayload>::Create(7); }),
              ERepresentationRequestResult::StagingBudget);
    ASSERT_EQ(queue.Request(7, Descriptor(1), []() -> Ref<FRepresentationPayload>
                            { return Ref<TestRepresentationPayload>::Create(7); }),
              ERepresentationRequestResult::Queued);
    TArray<FCompletedRepresentationLoad> results;
    ASSERT_TRUE(Until([&]
                      {
                          queue.RetrieveCompleted(results);
                          return results.Num() == 1; }));
    EXPECT_FALSE(results[0].Payload);
    EXPECT_TRUE(queue.HasFailed(7));
    EXPECT_EQ(queue.Request(7, Descriptor(), []() -> Ref<FRepresentationPayload>
                            { return Ref<TestRepresentationPayload>::Create(7); }),
              ERepresentationRequestResult::Failed);
    EXPECT_EQ(queue.GetStats().StagingBytes, 0u);
}

TEST_F(RepresentationStreamingTest, EvictionAndReloadKeepTheContentIdentity)
{
    FRepresentationLoadQueue queue;
    for (i32 reload = 0; reload != 2; ++reload)
    {
        ASSERT_EQ(queue.Request(1234, Descriptor(), []() -> Ref<FRepresentationPayload>
                                { return Ref<TestRepresentationPayload>::Create(1234); }),
                  ERepresentationRequestResult::Queued);
        TArray<FCompletedRepresentationLoad> results;
        ASSERT_TRUE(Until([&]
                          {
                              queue.RetrieveCompleted(results);
                              return results.Num() == 1; }));
        EXPECT_EQ(results[0].Key, 1234u);
        EXPECT_EQ(results[0].Payload.As<TestRepresentationPayload>()->Identity, 1234u);
        const u64 ticket = results[0].StagingTicket;
        results.Reset();
        queue.ReleaseStaging(ticket);
    }
}

TEST_F(RepresentationStreamingTest, MandatoryFallbackIsReportedWhileOptionalDetailWaitsForPhysicalReclaim)
{
    FRepresentationStreamingBudget budget;
    budget.BeginFrame(1, 100, 100);
    EXPECT_TRUE(budget.TryAdmit(1, Descriptor(40, 200, 120), true));
    EXPECT_EQ(budget.GetStats().PinnedResidentGpuBytes, 200u);
    EXPECT_EQ(budget.GetStats().PinnedUploadBytesThisFrame, 120u);
    EXPECT_FALSE(budget.TryAdmit(2, Descriptor())); // mandatory upload exhausted this frame
    budget.BeginFrame(2, 100, 100);
    EXPECT_TRUE(budget.TryAdmit(2, Descriptor()));
    budget.Release(2);
    budget.SetRetiringGpuBytes(60); // physical report, not an invented frame age
    EXPECT_FALSE(budget.TryAdmit(2, Descriptor()));
    EXPECT_EQ(budget.GetStats().OptionalResidentGpuBytes, 0u);
    budget.SetRetiringGpuBytes(0);
    EXPECT_TRUE(budget.TryAdmit(2, Descriptor()));
    EXPECT_TRUE(budget.IsResident(1));
}

TEST_F(RepresentationStreamingTest, SharedFrameUploadAndResidentExpansionAreAdmittedOnlyOnce)
{
    FRepresentationStreamingBudget budget;
    budget.BeginFrame(1, 100, 40);
    EXPECT_TRUE(budget.TryAdmit(1, Descriptor()));
    budget.BeginFrame(1, 100, 40);
    EXPECT_TRUE(budget.TryAdmit(1, Descriptor()));
    EXPECT_EQ(budget.GetStats().UploadedBytesThisFrame, 30u);
    EXPECT_FALSE(budget.TryAdmit(2, Descriptor(40, 10, 20)));
    EXPECT_FALSE(budget.TryAdmit(1, Descriptor(40, 110, 35)));
    EXPECT_TRUE(budget.TryAdmit(1, Descriptor(40, 70, 35)));
    EXPECT_EQ(budget.GetStats().OptionalResidentGpuBytes, 70u);
    EXPECT_EQ(budget.GetStats().UploadedBytesThisFrame, 35u);
    budget.BeginFrame(2, 100, 40);
    EXPECT_TRUE(budget.TryAdmit(2, Descriptor(40, 10, 20)));
}

TEST_F(RepresentationStreamingTest, AdmissionRefreshesPhysicalRetirementAfterASameFrameEviction)
{
    FRepresentationStreamingBudget budget;
    u64 backendRetiring = 0;
    budget.SetRetiringGpuBytesProvider([&backendRetiring]
                                       { return backendRetiring; });
    budget.BeginFrame(1, 100, 0);
    EXPECT_TRUE(budget.TryAdmit(1, Descriptor()));
    budget.Release(1);
    backendRetiring = 60;
    EXPECT_FALSE(budget.TryAdmit(2, Descriptor()));
    EXPECT_EQ(budget.GetStats().RetiringGpuBytes, 60u);
    backendRetiring = 0;
    EXPECT_TRUE(budget.TryAdmit(2, Descriptor()));
}

TEST_F(RepresentationStreamingTest, SharedTelemetryPreservesReadProvenanceAndReadyQueueOwnership)
{
    FRepresentationLoadQueue queue;
    auto knownRead = Descriptor();
    knownRead.DiskBytes = FAssetByteSize::Actual(8);
    ASSERT_EQ(queue.Request(1, knownRead, []() -> Ref<FRepresentationPayload>
                            { return Ref<TestRepresentationPayload>::Create(1); }),
              ERepresentationRequestResult::Queued);
    ASSERT_EQ(queue.Request(2, Descriptor(), []() -> Ref<FRepresentationPayload>
                            { return Ref<TestRepresentationPayload>::Create(2); }),
              ERepresentationRequestResult::Queued);
    ASSERT_TRUE(Until([&queue]
                      { return queue.GetStats().CompletedUnretrievedCount == 2; }));
    auto stats = RepresentationStreaming::Get().GetStats();
    EXPECT_EQ(stats.CompletedLoads, 2u);
    EXPECT_EQ(stats.ReadBytes, 8u);
    EXPECT_EQ(stats.UnknownReadCount, 1u); // estimates are not actual source reads
    EXPECT_EQ(stats.PendingLoads, 2u);
    EXPECT_EQ(stats.CompletedUnretrievedLoads, 2u);
    TArray<FCompletedRepresentationLoad> results;
    queue.RetrieveCompleted(results);
    EXPECT_EQ(RepresentationStreaming::Get().GetStats().HeldCompletedLoads, 2u);
    for (FCompletedRepresentationLoad& result : results)
    {
        result.Payload = nullptr;
        queue.ReleaseStaging(result.StagingTicket);
    }
    RepresentationStreaming::Get().RecordUpload(15, 22);
    stats = RepresentationStreaming::Get().GetStats();
    EXPECT_EQ(stats.ActualUploadedBytes, 15u);
    EXPECT_EQ(stats.UploadMicroseconds, 22u);
    EXPECT_EQ(stats.HeldCompletedLoads, 0u);
}

TEST_F(RepresentationStreamingTest, CompletedCancellationDropsTheTaskResultBeforeReleasingItsReservation)
{
    bool destroyedWhileCharged = false;
    FRepresentationLoadQueue queue;
    ASSERT_EQ(queue.Request(1, Descriptor(), [&destroyedWhileCharged]() -> Ref<FRepresentationPayload>
                            {
                                auto payload = Ref<TestRepresentationPayload>::Create(1);
                                payload->OnDestroy = [&destroyedWhileCharged]
                                { destroyedWhileCharged = RepresentationStreaming::Get().GetStats().StagingCpuBytes == 40; };
                                return payload; }),
              ERepresentationRequestResult::Queued);
    ASSERT_TRUE(Until([&queue]
                      { return queue.GetStats().CompletedUnretrievedCount == 1; }));
    EXPECT_EQ(queue.Cancel(1), EAssetLoadCancelResult::DiscardedCompleted);
    EXPECT_TRUE(destroyedWhileCharged);
    EXPECT_EQ(RepresentationStreaming::Get().GetStats().StagingCpuBytes, 0u);
}

TEST_F(RepresentationStreamingTest, MeasuredReadStatsOverrideSourceSizeAndStaySeparateFromPreparation)
{
    FRepresentationLoadQueue queue;
    auto descriptor = Descriptor();
    descriptor.DiskBytes = FAssetByteSize::Actual(900);
    ASSERT_EQ(queue.Request(1, descriptor, []() -> Ref<FRepresentationPayload>
                            {
                                auto payload = Ref<TestRepresentationPayload>::Create(1);
                                payload->IO = FRepresentationIOStats{ 17, 9 };
                                return payload; }),
              ERepresentationRequestResult::Queued);
    ASSERT_EQ(queue.Request(2, descriptor, []() -> Ref<FRepresentationPayload>
                            { return nullptr; }),
              ERepresentationRequestResult::Queued);
    ASSERT_TRUE(Until([&queue]
                      { return queue.GetStats().CompletedUnretrievedCount == 2; }));
    TArray<FCompletedRepresentationLoad> results;
    queue.RetrieveCompleted(results);
    RepresentationStreaming::Get().RecordIO({ 5, 3 }); // serial texture source read
    const auto stats = RepresentationStreaming::Get().GetStats();
    EXPECT_EQ(stats.ReadBytes, 22u); // neither source-size estimate nor failed read
    EXPECT_EQ(stats.ReadMicroseconds, 12u);
    EXPECT_EQ(stats.UnknownReadCount, 0u);
    EXPECT_EQ(stats.FailedLoads, 1u);
    EXPECT_EQ(stats.UploadMicroseconds, 0u);
    for (FCompletedRepresentationLoad& result : results)
    {
        result.Payload.Reset();
        queue.ReleaseStaging(result.StagingTicket);
    }
}

TEST_F(RepresentationStreamingTest, LoweredResidentBudgetDemotesExistingDetailUntilPhysicalRetirementDrains)
{
    FRepresentationStreamingBudget budget;
    u64 backendRetiring = 0;
    budget.SetRetiringGpuBytesProvider([&backendRetiring]
                                       { return backendRetiring; });
    budget.BeginFrame(1, 100, 100);
    ASSERT_TRUE(budget.TryAdmit(1, Descriptor()));
    budget.BeginFrame(2, 40, 100);
    EXPECT_FALSE(budget.TryAdmit(1, Descriptor()));
    EXPECT_TRUE(budget.IsResident(1)); // denial requests demotion; consumer owns eviction
    budget.Release(1);
    backendRetiring = 60;
    EXPECT_FALSE(budget.TryAdmit(1, Descriptor(40, 30, 30)));
    backendRetiring = 0;
    EXPECT_TRUE(budget.TryAdmit(1, Descriptor(40, 30, 30)));
}

TEST_F(RepresentationStreamingTest, ExistingDetailUsesNoUploadEvenAfterTheFrameAllowanceIsSpent)
{
    FRepresentationStreamingBudget budget;
    budget.BeginFrame(1, 100, 30);
    ASSERT_TRUE(budget.TryAdmit(1, Descriptor()));
    budget.BeginFrame(1, 100, 1);
    EXPECT_TRUE(budget.TryAdmit(1, Descriptor()));
    EXPECT_EQ(budget.GetStats().UploadedBytesThisFrame, 30u);
    EXPECT_EQ(budget.GetStats().DeniedForUpload, 0u);
}

TEST_F(RepresentationStreamingTest, CapacityReportingDoesNotRecursivelyInvokeTheRetirementProvider)
{
    FRepresentationStreamingBudget budget;
    u32 queries = 0;
    budget.SetRetiringGpuBytesProvider([&]
                                       {
                                          ++queries;
                                          EXPECT_EQ(budget.GetStats().OptionalResidentGpuBytes, 0u);
                                          return 10; });
    budget.BeginFrame(1, 100, 100);
    EXPECT_EQ(budget.GetStats().RetiringGpuBytes, 0u);
    EXPECT_EQ(queries, 0u);
    ASSERT_TRUE(budget.TryAdmit(1, Descriptor()));
    EXPECT_EQ(queries, 1u);
    EXPECT_EQ(budget.GetStats().RetiringGpuBytes, 10u);
}

TEST_F(RepresentationStreamingTest, PressureUsesOverflowSafePhysicalRetirementAndZeroMeansUnlimited)
{
    FRepresentationStreamingBudget budget;
    const u64 largest = std::numeric_limits<u64>::max();
    u64 physicalRetiring = 0;
    budget.SetRetiringGpuBytesProvider([&physicalRetiring]
                                       { return physicalRetiring; });
    budget.BeginFrame(1, largest, 0);
    ASSERT_TRUE(budget.TryAdmit(1, Descriptor(40, 60, 30)));
    ASSERT_TRUE(budget.TryAdmit(2, Descriptor(40, 1000, 30), true));
    physicalRetiring = largest - 60;
    EXPECT_FALSE(budget.IsUnderPressure()); // exact cap, pinned floor excluded
    physicalRetiring = largest - 59;
    EXPECT_TRUE(budget.IsUnderPressure()); // mathematical sum exceeds u64, cannot wrap
    EXPECT_EQ(budget.GetStats().RetiringGpuBytes, physicalRetiring);
    budget.BeginFrame(2, 0, 0);
    EXPECT_FALSE(budget.IsUnderPressure()); // authored zero is no optional cap
}

TEST_F(RepresentationStreamingTest, MeasuredBackingGrowthStaysChargedUntilPhysicalRetirementDrains)
{
    FRepresentationStreamingBudget budget;
    budget.BeginFrame(1, 100, 30);
    ASSERT_TRUE(budget.TryAdmit(1, Descriptor()));
    EXPECT_FALSE(budget.ReconcileResidentGpuBytes(2, 110)); // cannot invent admission
    ASSERT_TRUE(budget.ReconcileResidentGpuBytes(1, 110));  // backing already exists
    EXPECT_TRUE(budget.IsResident(1));
    EXPECT_TRUE(budget.IsUnderPressure());
    EXPECT_EQ(budget.GetStats().OptionalResidentGpuBytes, 110u);
    EXPECT_EQ(budget.GetStats().UploadedBytesThisFrame, 30u);
    EXPECT_FALSE(budget.TryAdmit(2, Descriptor(40, 1, 0)));
    EXPECT_TRUE(budget.ReconcileResidentGpuBytes(1, 60)); // shrink needs actual eviction
    EXPECT_EQ(budget.GetStats().OptionalResidentGpuBytes, 110u);
    budget.Release(1);
    budget.SetRetiringGpuBytes(110);
    EXPECT_TRUE(budget.IsUnderPressure());
    EXPECT_FALSE(budget.TryAdmit(2, Descriptor(40, 1, 0)));
    budget.SetRetiringGpuBytes(0);
    EXPECT_TRUE(budget.TryAdmit(2, Descriptor(40, 1, 0)));
}

TEST_F(RepresentationStreamingTest, ReconciledTotalsCannotWrapAndRecoverAfterEviction)
{
    FRepresentationStreamingBudget budget;
    budget.BeginFrame(1, 0, 0);
    ASSERT_TRUE(budget.TryAdmit(1, Descriptor(40, 60, 0)));
    ASSERT_TRUE(budget.TryAdmit(2, Descriptor(40, 60, 0)));
    ASSERT_TRUE(budget.ReconcileResidentGpuBytes(1, std::numeric_limits<u64>::max()));
    EXPECT_EQ(budget.GetStats().OptionalResidentGpuBytes, std::numeric_limits<u64>::max());
    EXPECT_TRUE(budget.IsUnderPressure()); // unrepresentable accounting cannot admit detail
    EXPECT_FALSE(budget.TryAdmit(3, Descriptor(40, 1, 0)));
    budget.Release(1);
    EXPECT_EQ(budget.GetStats().OptionalResidentGpuBytes, 60u);
    EXPECT_FALSE(budget.IsUnderPressure());
    EXPECT_TRUE(budget.TryAdmit(3, Descriptor(40, 1, 0)));
}
