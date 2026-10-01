// OLO_TEST_LAYER: plumbing
//
// The physical-allocation-aware renderer memory report (issue #1342), CPU half.
//
// Everything here runs against the real RendererMemoryTracker singleton with FAKE
// addresses (the tracker never dereferences them), so it needs no device and runs on
// every CI runner. Each test measures a DELTA from the report it took on entry: other
// tests in the binary own live GPU allocations, and the totals are process-wide.
//
// What it pins:
//   * the pure reconciliation classifier (ReconcileCommittedBytes);
//   * a view/alias adds logical bytes and ZERO physical bytes — and the #1342 negative
//     control, Levers::FaultCountAliasAsBacking, turns that into a double count this
//     file then detects (the snapshot check must go red on the planted fault);
//   * retiring bytes stay resident until the deferred delete releases them, and a peak
//     window records the old+new coexistence a resize or reload produces;
//   * owner scopes attribute, innermost first; GPU and CPU never share a total;
//   * reconciliation against an allocator observation, through a fake observer;
//   * the shared format-size helper (mip chains, block formats, every attachment).
//
// The GPU halves — real textures and views on OpenGL, and VMA reconciliation on Vulkan —
// are RendererMemoryAccountingEvidenceTest.cpp and a VulkanPassSuite tenant.

#include "OloEnginePCH.h"

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Debug/RendererMemoryFormat.h"
#include "OloEngine/Renderer/Debug/RendererMemoryReport.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"

#include <gtest/gtest.h>

#include <array>
#include <string_view>

namespace OloEngine::Tests
{
    namespace
    {
        using Tracker = RendererMemoryTracker;
        using ResourceType = RendererMemoryTracker::ResourceType;

        // Distinct fake addresses per test: the tracker keys on them and never reads them.
        struct FakeObjects
        {
            std::array<u64, 8> Storage{};
            [[nodiscard]] void* At(sizet i)
            {
                return &Storage[i];
            }
        };

        class ScopedFaultCountAliasAsBacking
        {
          public:
            explicit ScopedFaultCountAliasAsBacking(bool on) : m_Previous(Levers::FaultCountAliasAsBacking())
            {
                Levers::SetFaultCountAliasAsBacking(on);
            }
            ~ScopedFaultCountAliasAsBacking()
            {
                Levers::SetFaultCountAliasAsBacking(m_Previous);
            }
            ScopedFaultCountAliasAsBacking(const ScopedFaultCountAliasAsBacking&) = delete;
            ScopedFaultCountAliasAsBacking& operator=(const ScopedFaultCountAliasAsBacking&) = delete;

          private:
            bool m_Previous;
        };

        // A fake allocator for the reconciliation tests; restores the real observer on exit.
        struct FakeObserver
        {
            static inline u64 s_AllocationBytes = 0;
            static bool Observe(BackendMemoryObservation& out)
            {
                out = BackendMemoryObservation{};
                out.Backend = MemoryBackend::Vulkan;
                out.HasAllocatorTotals = true;
                out.AllocationBytes = s_AllocationBytes;
                out.Residency = MemoryResidencyStatus::OsReported;
                return true;
            }
        };
        class ScopedFakeObserver
        {
          public:
            ScopedFakeObserver()
            {
                Tracker::GetInstance().SetBackendObserver(&FakeObserver::Observe);
            }
            ~ScopedFakeObserver()
            {
                Tracker::GetInstance().SetBackendObserver(&Tracker::ObserveActiveRendererAPI);
            }
            ScopedFakeObserver(const ScopedFakeObserver&) = delete;
            ScopedFakeObserver& operator=(const ScopedFakeObserver&) = delete;
        };

        void TrackCommittedVulkan(void* address, sizet bytes, std::string_view name)
        {
            Tracker::AllocationDesc desc;
            desc.Address = address;
            desc.Size = bytes;
            desc.Type = ResourceType::Texture2D;
            desc.Name = name;
            desc.IsGPU = true;
            desc.Backend = MemoryBackend::Vulkan;
            desc.SizeSource = MemorySizeSource::Committed;
            desc.File = __FILE__;
            desc.Line = __LINE__;
            Tracker::GetInstance().TrackAllocation(desc);
        }

        // THE SNAPSHOT CHECK. Between two reports, the physical GPU bytes must have moved by
        // exactly the bytes of the BACKING allocations made in between — every view or alias
        // made in the same window contributes logical bytes only. Returns false (with the
        // numbers) when a view was counted as backing.
        [[nodiscard]] testing::AssertionResult PhysicalDeltaIsBackingOnly(const RendererMemoryReport& before,
                                                                          const RendererMemoryReport& after,
                                                                          u64 expectedBackingBytes)
        {
            const i64 delta = static_cast<i64>(after.Gpu.ResidentBytes()) - static_cast<i64>(before.Gpu.ResidentBytes());
            if (delta == static_cast<i64>(expectedBackingBytes))
                return testing::AssertionSuccess();
            return testing::AssertionFailure() << "physical GPU bytes moved by " << delta << ", but the backing allocations made "
                                               << "were " << expectedBackingBytes << " bytes — a view or alias was counted as its "
                                               << "own backing allocation";
        }
    } // namespace

    // --- The pure classifier --------------------------------------------------------------

    TEST(RendererMemoryReport, ReconcileClassifiesEveryOutcome)
    {
        EXPECT_EQ(ReconcileCommittedBytes(10, nullptr, false).Status, MemoryReconciliationStatus::NoDevice);

        BackendMemoryObservation gl;
        gl.Backend = MemoryBackend::OpenGL;
        gl.HasAllocatorTotals = false;
        const auto notObservable = ReconcileCommittedBytes(10, &gl, false);
        EXPECT_EQ(notObservable.Status, MemoryReconciliationStatus::NotObservable);
        EXPECT_FALSE(notObservable.DifferenceBytes.has_value()) << "no allocator: no difference exists, not a zero one";
        EXPECT_FALSE(notObservable.ObservedAllocationBytes.has_value());

        BackendMemoryObservation vk;
        vk.Backend = MemoryBackend::Vulkan;
        vk.HasAllocatorTotals = true;
        vk.AllocationBytes = 1000;

        const auto reconciled = ReconcileCommittedBytes(1000, &vk, false);
        EXPECT_EQ(reconciled.Status, MemoryReconciliationStatus::Reconciled);
        EXPECT_EQ(reconciled.DifferenceBytes, 0);

        const auto untracked = ReconcileCommittedBytes(600, &vk, false);
        EXPECT_EQ(untracked.Status, MemoryReconciliationStatus::Untracked);
        EXPECT_EQ(untracked.DifferenceBytes, 400);

        const auto overCounted = ReconcileCommittedBytes(1500, &vk, false);
        EXPECT_EQ(overCounted.Status, MemoryReconciliationStatus::OverCounted);
        EXPECT_EQ(overCounted.DifferenceBytes, -500);

        const auto racing = ReconcileCommittedBytes(1500, &vk, true);
        EXPECT_EQ(racing.Status, MemoryReconciliationStatus::Racing);
        EXPECT_FALSE(racing.DifferenceBytes.has_value()) << "two readings of different instants have no difference";
    }

    // --- Views and aliases ----------------------------------------------------------------

    TEST(RendererMemoryReport, AnAliasAddsLogicalBytesAndNoPhysicalBytes)
    {
        Tracker& tracker = Tracker::GetInstance();
        FakeObjects objects;

        const RendererMemoryReport before = tracker.BuildReport();
        tracker.TrackAllocation(objects.At(0), 4096, ResourceType::Texture2D, "backing", true, __FILE__, __LINE__);
        tracker.TrackAlias(objects.At(1), objects.At(0), 4096, ResourceType::Texture2D, "view", __FILE__, __LINE__);
        tracker.TrackAlias(objects.At(2), objects.At(0), 4096, ResourceType::Texture2D, "second view", __FILE__, __LINE__);
        const RendererMemoryReport after = tracker.BuildReport();

        EXPECT_TRUE(PhysicalDeltaIsBackingOnly(before, after, 4096));
        EXPECT_EQ(after.AliasCount - before.AliasCount, 2u);
        EXPECT_EQ(after.AliasLogicalBytes - before.AliasLogicalBytes, 8192u);
        EXPECT_EQ(after.OrphanAliasCount, before.OrphanAliasCount);

        // A view outliving its backing is an orphan: reported as a hole, not as bytes.
        tracker.TrackDeallocation(objects.At(0), __FILE__, __LINE__);
        EXPECT_EQ(tracker.BuildReport().OrphanAliasCount - before.OrphanAliasCount, 2u);

        tracker.TrackDeallocation(objects.At(1), __FILE__, __LINE__);
        tracker.TrackDeallocation(objects.At(2), __FILE__, __LINE__);
        const RendererMemoryReport end = tracker.BuildReport();
        EXPECT_TRUE(PhysicalDeltaIsBackingOnly(before, end, 0));
        EXPECT_EQ(end.AliasCount, before.AliasCount);
    }

    // The negative control. With the fault planted, TrackAlias books the view as a second
    // backing allocation — exactly the logical-versus-physical double count the issue names —
    // and the snapshot check above must catch it. A check that stays green here checks nothing.
    TEST(RendererMemoryReport, NegativeControl_TheSnapshotCheckCatchesAnAliasCountedAsBacking)
    {
        Tracker& tracker = Tracker::GetInstance();
        FakeObjects objects;

        {
            const ScopedFaultCountAliasAsBacking fault(true);
            const RendererMemoryReport before = tracker.BuildReport();
            tracker.TrackAllocation(objects.At(0), 4096, ResourceType::Texture2D, "backing", true, __FILE__, __LINE__);
            tracker.TrackAlias(objects.At(1), objects.At(0), 4096, ResourceType::Texture2D, "view", __FILE__, __LINE__);
            const RendererMemoryReport after = tracker.BuildReport();

            EXPECT_FALSE(PhysicalDeltaIsBackingOnly(before, after, 4096))
                << "the planted double count went undetected: the snapshot check is vacuous";
            EXPECT_EQ(after.Gpu.ResidentBytes() - before.Gpu.ResidentBytes(), 8192u);
        }

        tracker.TrackDeallocation(objects.At(1), __FILE__, __LINE__);
        tracker.TrackDeallocation(objects.At(0), __FILE__, __LINE__);
    }

    // --- In-flight retirement and transient peaks -----------------------------------------

    TEST(RendererMemoryReport, RetiringBytesStayResidentUntilTheDeferredDeleteRuns)
    {
        Tracker& tracker = Tracker::GetInstance();
        FakeObjects objects;

        const RendererMemoryReport before = tracker.BuildReport();
        tracker.TrackAllocation(objects.At(0), 1000, ResourceType::StorageBuffer, "old", true, __FILE__, __LINE__);

        tracker.BeginPeakWindow();
        const u64 ticket = tracker.RetireAllocation(objects.At(0));
        ASSERT_NE(ticket, 0u);

        // The owner has already replaced it — at the SAME address, which the retire freed.
        tracker.TrackAllocation(objects.At(0), 2000, ResourceType::StorageBuffer, "new", true, __FILE__, __LINE__);

        const RendererMemoryReport coexisting = tracker.BuildReport();
        EXPECT_EQ(coexisting.Gpu.LiveBytes - before.Gpu.LiveBytes, 2000u);
        EXPECT_EQ(coexisting.Gpu.RetiringBytes - before.Gpu.RetiringBytes, 1000u);
        EXPECT_GE(coexisting.Gpu.WindowPeakBytes, before.Gpu.ResidentBytes() + 3000u)
            << "the window peak must hold old and new together — the resize/reload coexistence";

        tracker.ReleaseRetired(ticket);
        const RendererMemoryReport reclaimed = tracker.BuildReport();
        EXPECT_EQ(reclaimed.Gpu.RetiringBytes, before.Gpu.RetiringBytes) << "retired bytes were never reclaimed";
        EXPECT_EQ(reclaimed.Gpu.ResidentBytes() - before.Gpu.ResidentBytes(), 2000u);
        EXPECT_EQ(reclaimed.Gpu.WindowPeakBytes, coexisting.Gpu.WindowPeakBytes) << "a peak is a maximum; reclaim must not lower it";

        // A second release of the same ticket, or ticket 0, must not subtract again.
        tracker.ReleaseRetired(ticket);
        tracker.ReleaseRetired(0);
        EXPECT_EQ(tracker.BuildReport().Gpu.ResidentBytes(), reclaimed.Gpu.ResidentBytes());

        tracker.TrackDeallocation(objects.At(0), __FILE__, __LINE__);
    }

    TEST(RendererMemoryReport, RetiringAnUntrackedAddressOrAnAliasHandsBackNoTicket)
    {
        Tracker& tracker = Tracker::GetInstance();
        FakeObjects objects;
        EXPECT_EQ(tracker.RetireAllocation(objects.At(0)), 0u);

        tracker.TrackAllocation(objects.At(0), 64, ResourceType::Other, "backing", true, __FILE__, __LINE__);
        tracker.TrackAlias(objects.At(1), objects.At(0), 64, ResourceType::Other, "view", __FILE__, __LINE__);
        EXPECT_EQ(tracker.RetireAllocation(objects.At(1)), 0u) << "a view owns no backing, so there is nothing to wait for";
        tracker.TrackDeallocation(objects.At(0), __FILE__, __LINE__);
    }

    // --- Attribution and units ------------------------------------------------------------

    TEST(RendererMemoryReport, OwnerScopesAttributeInnermostFirst)
    {
        Tracker& tracker = Tracker::GetInstance();
        FakeObjects objects;

        {
            const RendererMemoryOwnerScope outer("MemReportTestOuter", MemoryLifetime::PassOwned);
            tracker.TrackAllocation(objects.At(0), 111, ResourceType::Other, "outer", true, __FILE__, __LINE__);
            {
                const RendererMemoryOwnerScope inner("MemReportTestInner", MemoryLifetime::Pooled);
                tracker.TrackAllocation(objects.At(1), 222, ResourceType::Other, "inner", true, __FILE__, __LINE__);
            }
            EXPECT_EQ(RendererMemoryOwnerScope::Current().Owner, "MemReportTestOuter");
        }
        EXPECT_TRUE(RendererMemoryOwnerScope::Current().Owner.empty());

        const RendererMemoryReport report = tracker.BuildReport();
        const auto find = [&report](std::string_view owner) -> const MemoryOwnerRow*
        {
            for (const auto& row : report.Owners)
                if (row.Owner.ToView() == owner)
                    return &row;
            return nullptr;
        };
        const MemoryOwnerRow* outer = find("MemReportTestOuter");
        const MemoryOwnerRow* inner = find("MemReportTestInner");
        ASSERT_NE(outer, nullptr);
        ASSERT_NE(inner, nullptr);
        EXPECT_EQ(outer->GpuLiveBytes, 111u);
        EXPECT_EQ(outer->Lifetime, MemoryLifetime::PassOwned);
        EXPECT_EQ(inner->GpuLiveBytes, 222u);
        EXPECT_EQ(inner->Lifetime, MemoryLifetime::Pooled);

        tracker.TrackDeallocation(objects.At(0), __FILE__, __LINE__);
        tracker.TrackDeallocation(objects.At(1), __FILE__, __LINE__);
    }

    // The drill-down behind an owner row (olo_memory_report's `owner`): biggest first, capped,
    // live entries only, and an owner nobody booked is "unknown" rather than an empty list.
    TEST(RendererMemoryReport, AnOwnerDrillDownListsItsLargestLiveEntriesAndRejectsAnUnknownOwner)
    {
        Tracker& tracker = Tracker::GetInstance();
        FakeObjects objects;
        {
            const RendererMemoryOwnerScope owner("MemReportTestDrill", MemoryLifetime::Persistent);
            tracker.TrackAllocation(objects.At(0), 100, ResourceType::Other, "small", true, __FILE__, __LINE__);
            tracker.TrackAllocation(objects.At(1), 300, ResourceType::Other, "large", true, __FILE__, __LINE__);
            tracker.TrackAllocation(objects.At(2), 200, ResourceType::Other, "middle", true, __FILE__, __LINE__);
        }
        const u64 ticket = tracker.RetireAllocation(objects.At(1));

        const auto largest = tracker.GetLargestAllocations("MemReportTestDrill", 1);
        ASSERT_TRUE(largest.has_value());
        ASSERT_EQ(largest->Num(), 1);
        EXPECT_EQ((*largest)[0].m_Size, 200u) << "a retiring entry was listed as live, or the order is not biggest first";
        const auto all = tracker.GetLargestAllocations("MemReportTestDrill", 10);
        ASSERT_TRUE(all.has_value());
        EXPECT_EQ(all->Num(), 2);
        EXPECT_FALSE(tracker.GetLargestAllocations("MemReportTestNoSuchOwner", 10).has_value())
            << "an owner nobody booked must be reported as unknown, not as an owner with no bytes";

        tracker.ReleaseRetired(ticket);
        tracker.TrackDeallocation(objects.At(0), __FILE__, __LINE__);
        tracker.TrackDeallocation(objects.At(2), __FILE__, __LINE__);
    }

    TEST(RendererMemoryReport, GpuAndCpuBytesNeverShareATotal)
    {
        Tracker& tracker = Tracker::GetInstance();
        FakeObjects objects;

        const RendererMemoryReport before = tracker.BuildReport();
        tracker.TrackAllocation(objects.At(0), 5000, ResourceType::Shader, "cpu-side", false, __FILE__, __LINE__);
        const RendererMemoryReport after = tracker.BuildReport();
        EXPECT_EQ(after.Gpu.ResidentBytes(), before.Gpu.ResidentBytes()) << "a CPU booking moved the GPU total";
        EXPECT_EQ(after.Cpu.ResidentBytes() - before.Cpu.ResidentBytes(), 5000u);
        tracker.TrackDeallocation(objects.At(0), __FILE__, __LINE__);
    }

    TEST(RendererMemoryReport, CapacityReportersAppearWhileRegisteredOnly)
    {
        Tracker& tracker = Tracker::GetInstance();
        const auto countRows = [&tracker]
        {
            u32 rows = 0;
            for (const auto& row : tracker.BuildReport().Capacity)
                rows += row.Owner.ToView() == "MemReportTestOwner" ? 1u : 0u;
            return rows;
        };
        {
            const RendererMemoryReporterHandle handle([](TArray<MemoryCapacityRow>& rows)
                                                      {
                MemoryCapacityRow row;
                row.Owner = "MemReportTestOwner";
                row.CapacityBytes = 10;
                row.ActiveDemandBytes = 4;
                rows.Add(std::move(row)); });
            EXPECT_EQ(countRows(), 1u);
        }
        EXPECT_EQ(countRows(), 0u) << "a destroyed owner's reporter still ran";
    }

    // --- Reconciliation against an allocator ----------------------------------------------

    TEST(RendererMemoryReport, CommittedBytesReconcileAgainstTheAllocatorAndTheFaultShowsAsOverCounted)
    {
        Tracker& tracker = Tracker::GetInstance();
        FakeObjects objects;
        const ScopedFakeObserver fake;

        // Whatever committed Vulkan bytes the process already holds are the baseline.
        FakeObserver::s_AllocationBytes = 0;
        const u64 baseline = tracker.BuildReport().Reconciliation.TrackedCommittedBytes.value_or(0);

        FakeObserver::s_AllocationBytes = baseline + 65536;
        TrackCommittedVulkan(objects.At(0), 65536, "committed image");
        {
            const RendererMemoryReport report = tracker.BuildReport();
            EXPECT_EQ(report.Reconciliation.Status, MemoryReconciliationStatus::Reconciled);
            EXPECT_EQ(report.Observation.Residency, MemoryResidencyStatus::OsReported);
        }

        // A view of it is logical only: still reconciled.
        tracker.TrackAlias(objects.At(1), objects.At(0), 65536, ResourceType::Texture2D, "view", __FILE__, __LINE__);
        EXPECT_EQ(tracker.BuildReport().Reconciliation.Status, MemoryReconciliationStatus::Reconciled);
        tracker.TrackDeallocation(objects.At(1), __FILE__, __LINE__);

        // The planted double count: the allocator did not allocate twice, the tracker says it did.
        {
            const ScopedFaultCountAliasAsBacking faultOn(true);
            tracker.TrackAlias(objects.At(1), objects.At(0), 65536, ResourceType::Texture2D, "view", __FILE__, __LINE__);
            const RendererMemoryReport report = tracker.BuildReport();
            EXPECT_EQ(report.Reconciliation.Status, MemoryReconciliationStatus::OverCounted);
            EXPECT_EQ(report.Reconciliation.DifferenceBytes, -65536);
        }
        tracker.TrackDeallocation(objects.At(1), __FILE__, __LINE__);

        // And an allocation the tracker never saw reads as untracked, with its size.
        FakeObserver::s_AllocationBytes = baseline + 65536 + 1024;
        const RendererMemoryReport untracked = tracker.BuildReport();
        EXPECT_EQ(untracked.Reconciliation.Status, MemoryReconciliationStatus::Untracked);
        EXPECT_EQ(untracked.Reconciliation.DifferenceBytes, 1024);

        tracker.TrackDeallocation(objects.At(0), __FILE__, __LINE__);
    }

    TEST(RendererMemoryReport, AnOpenMutationBracketReportsRacingRatherThanADifference)
    {
        Tracker& tracker = Tracker::GetInstance();
        const ScopedFakeObserver fake;
        FakeObserver::s_AllocationBytes = 123; // deliberately wrong: must not be published as a difference

        tracker.BeginExternalMutation();
        const RendererMemoryReport report = tracker.BuildReport();
        tracker.EndExternalMutation();

        EXPECT_EQ(report.Reconciliation.Status, MemoryReconciliationStatus::Racing);
        EXPECT_FALSE(report.Reconciliation.DifferenceBytes.has_value());
    }

    TEST(RendererMemoryReport, OpenGLReportsNoAllocatorAndUnknownResidency)
    {
        // What the real OpenGL backend answers: never a zero-byte residency.
        const auto* api = RenderCommand::TryGetRendererAPI();
        if (!api || RendererAPI::GetAPI() != RendererAPI::API::OpenGL)
            GTEST_SKIP() << "the active RendererAPI is not OpenGL";

        BackendMemoryObservation observation;
        ASSERT_TRUE(api->ObserveDeviceMemory(observation));
        EXPECT_EQ(observation.Backend, MemoryBackend::OpenGL);
        EXPECT_FALSE(observation.HasAllocatorTotals);
        EXPECT_EQ(observation.Residency, MemoryResidencyStatus::Unknown);
        EXPECT_TRUE(observation.Heaps.IsEmpty()) << "GL has no heaps to report; an empty list, not zero-byte heaps";

        const RendererMemoryReport report = RendererMemoryTracker::GetInstance().BuildReport();
        EXPECT_EQ(report.Reconciliation.Status, MemoryReconciliationStatus::NotObservable);
        EXPECT_EQ(report.Observation.Residency, MemoryResidencyStatus::Unknown);
    }

    // --- The shared format-size helper ----------------------------------------------------

    TEST(RendererMemoryFormat, MipChainsBlocksAndAttachmentsAreCountedWhole)
    {
        using namespace RendererMemoryFormat;

        EXPECT_EQ(FullMipCount(1024, 512), 11u);
        EXPECT_EQ(FullMipCount(1, 1), 1u);

        // 4x4 RGBA8 with a full chain: 16 + 4 + 1 texels.
        EXPECT_EQ(ImageBytes(ImageFormat::RGBA8, 4, 4, 3), (16u + 4u + 1u) * 4u);
        // MSAA multiplies, layers multiply.
        EXPECT_EQ(ImageBytes(ImageFormat::RGBA16F, 8, 8, 1, 2, 4), 8u * 8u * 8u * 2u * 4u);
        // BC7: 16 bytes per 4x4 block; a 6x6 level rounds up to 2x2 blocks.
        EXPECT_EQ(ImageBytes(ImageFormat::BC7, 6, 6, 1), 4u * 16u);
        EXPECT_EQ(ImageBytes(ImageFormat::BC4, 8, 8, 1), 4u * 8u);
        // Unknown is not zero.
        EXPECT_FALSE(ImageBytes(ImageFormat::None, 8, 8).has_value());

        // Every uncompressed ImageFormat has a size; every block format has a block size.
        for (u32 i = 1; i <= static_cast<u32>(ImageFormat::BC4); ++i)
        {
            const auto format = static_cast<ImageFormat>(i);
            EXPECT_TRUE(BytesPerTexel(format).has_value() != BlockBytes(format).has_value())
                << "ImageFormat " << i << " has no size in the memory-format table";
        }

        // A framebuffer counts EVERY attachment at its sample count (the pool used to count
        // the first attachment only, and the GL framebuffer 4 bytes each at one sample).
        FramebufferSpecification spec;
        spec.Width = 16;
        spec.Height = 8;
        spec.Samples = 4;
        spec.Attachments = { FramebufferTextureFormat::RGBA16F, FramebufferTextureFormat::RED_INTEGER,
                             FramebufferTextureFormat::RGBA32F, FramebufferTextureFormat::Depth };
        EXPECT_EQ(FramebufferBytes(spec), 16u * 8u * 4u * (8u + 4u + 16u + 4u));
    }
} // namespace OloEngine::Tests
