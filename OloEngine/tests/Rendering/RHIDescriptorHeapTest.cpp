// OLO_TEST_LAYER: plumbing
// =============================================================================
// RHIDescriptorHeapTest.cpp
//
// Contract tests for the Phase 3 view registry + descriptor heap (issue #691,
// ADR 0011 §1.1 / §1.2 / §1.2a).
//
// THE TESTS THAT JUSTIFY THE DESIGN, in the order they matter:
//
//   1. `RecycledSlotWithStaleGenerationIsRejected` — the heap recycles slot
//      indices, so a bare u32 offset cannot tell a live view from a dead one.
//      This is the direct sibling of RHIResourceRegistryTest's test of the same
//      name one level down, and if it is ever weakened the generation on
//      ViewHandle buys nothing.
//   2. `TransientOffsetGoesStaleAtTheFrameBoundary` — ADR 0011 §1.2's
//      "never stored across a use" is a RULE nobody can enforce by review. This
//      makes breaking it detectable.
//   3. `AliasedResourceHoldsTwoOffsetsInOneFrame` — the property the whole
//      per-frame ring exists to produce. Under RenderGraph::WriteNewVersion one
//      physical object legitimately has two offsets; that is correct, and the
//      alternative (persistent transient slots) would need one rewritten
//      mid-frame, which is the stale-read archetype
//      docs/agent-rules/render-graph-transient-aliasing.md is about.
//   4. `OneResourceTwoViewsTwoOffsets` — the one-to-many relationship that
//      makes ViewHandle a separate type from ResourceHandle at all.
//
// These are pure CPU tests. The heap stores opaque u64 descriptors and reaches
// the device only through IDescriptorHeapBackend, so a recording fake lets the
// whole contract run headless with no GL context — which is deliberate: the
// bugs this layer exists to prevent are lifetime bugs, not driver bugs.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"

#include <gtest/gtest.h>

#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // A recording backend. Hands out a distinct, non-zero descriptor per
        // acquire so a test can tell "the heap re-fetched" from "the heap reused
        // a cached value" — the distinction InvalidateResource turns on.
        class FakeHeapBackend final : public RHI::IDescriptorHeapBackend
        {
          public:
            bool Supported = true;
            // Set to make the next acquire fail, so the "backend could not
            // realise this view" path is exercised rather than assumed.
            bool FailNextAcquire = false;

            u64 NextDescriptor = 100u;
            u32 Acquires = 0u;
            u32 Releases = 0u;
            u32 Uploads = 0u;
            u32 Binds = 0u;
            std::vector<u64> LastUpload;
            u32 LastUploadFirstSlot = 0u;

            [[nodiscard]] auto IsBindlessSupported() const -> bool override
            {
                return Supported;
            }

            [[nodiscard]] auto AcquireDescriptor(RHI::ResourceHandle, const RHI::ViewDesc&,
                                                 const RHI::SamplerDesc&) -> u64 override
            {
                ++Acquires;
                if (FailNextAcquire)
                {
                    FailNextAcquire = false;
                    return 0u;
                }
                return NextDescriptor++;
            }

            void ReleaseDescriptor(u64 descriptor) override
            {
                if (descriptor != 0u)
                {
                    ++Releases;
                }
            }

            void UploadSlots(u32 firstSlot, const u64* descriptors, u32 count) override
            {
                ++Uploads;
                LastUploadFirstSlot = firstSlot;
                LastUpload.assign(descriptors, descriptors + count);
            }

            void BindHeap() override
            {
                ++Binds;
            }

            // A distinctive non-zero value, deliberately. Zero would let a test
            // that meant "the slot holds the null descriptor" pass equally when
            // the slot was simply never written.
            static constexpr u64 kNull = 0xD15AB1EDu;
            [[nodiscard]] auto NullDescriptor() const -> u64 override
            {
                return kNull;
            }
        };

        // Registers a real resource identity so the heap's liveness check has
        // something true to check. Deliberately uses the real registry rather
        // than a fake: the heap's contract is "a view of a dead resource does
        // not produce an offset", and faking the registry would test the fake.
        [[nodiscard]] RHI::ResourceHandle MakeResource(u64 native)
        {
            return RHI::ResourceRegistry::Get().Register(RHI::ResourceKind::Texture, native, RHI::Backend::OpenGL);
        }

        struct HeapFixture : ::testing::Test
        {
            FakeHeapBackend Backend;

            void SetUpHeap(u32 persistent = 8u, u32 transient = 4u, bool poison = false)
            {
                RHI::HeapDesc desc;
                desc.ResourceSlotCapacity = persistent;
                desc.SamplerSlotCapacity = 8u;
                desc.FrameTransientRingSlots = transient;
                desc.PoisonOnFree = poison;

                RHI::DescriptorHeap::Get().Initialize(desc, &Backend);
                // The environment defaults the toggle OFF (that is what keeps a
                // machine without the extension on the old path), so every test
                // turns it on explicitly. A test that forgot would silently get
                // null handles everywhere and pass its negative assertions.
                RHI::DescriptorHeap::Get().SetEnabled(true);
                ASSERT_TRUE(RHI::DescriptorHeap::Get().IsEnabled())
                    << "The fake backend reports support, so the toggle must take.";
            }

            void TearDown() override
            {
                RHI::DescriptorHeap::Get().Shutdown();
            }
        };
    } // namespace

    // -------------------------------------------------------------------------
    // 1. Generation validation — the reason ViewHandle is not a bare u32.
    // -------------------------------------------------------------------------
    TEST_F(HeapFixture, RecycledSlotWithStaleGenerationIsRejected)
    {
        SetUpHeap();
        auto& heap = RHI::DescriptorHeap::Get();

        const RHI::ResourceHandle first = MakeResource(11u);
        const RHI::ViewHandle viewA = heap.CreateView(first, RHI::ViewDesc{}, RHI::SamplerDesc{},
                                                      RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(viewA.IsValid());
        const RHI::HeapOffset offsetA = heap.OffsetOf(viewA);
        ASSERT_TRUE(offsetA.IsValid());

        heap.DestroyView(viewA);

        // The freed slot goes back on the freelist, so the very next persistent
        // view takes the SAME index. That is the collision a raw offset cannot
        // see — and the whole point is that it is not merely unlikely here, it
        // is guaranteed by the allocator.
        const RHI::ResourceHandle second = MakeResource(22u);
        const RHI::ViewHandle viewB = heap.CreateView(second, RHI::ViewDesc{}, RHI::SamplerDesc{},
                                                      RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(viewB.IsValid());
        EXPECT_EQ(viewB.Index, viewA.Index) << "The allocator must actually recycle, or this test proves nothing.";
        EXPECT_NE(viewB.Generation, viewA.Generation);

        // The stale handle must not resolve, even though its INDEX is now a
        // perfectly valid live slot.
        EXPECT_FALSE(heap.OffsetOf(viewA).IsValid());
        EXPECT_FALSE(heap.IsLive(viewA));
        EXPECT_TRUE(heap.OffsetOf(viewB).IsValid());
    }

    // -------------------------------------------------------------------------
    // 2. The frame-transient rule, made enforceable.
    // -------------------------------------------------------------------------
    TEST_F(HeapFixture, TransientOffsetGoesStaleAtTheFrameBoundary)
    {
        SetUpHeap();
        auto& heap = RHI::DescriptorHeap::Get();

        const RHI::ResourceHandle resource = MakeResource(33u);
        const RHI::ViewHandle view = heap.CreateView(resource, RHI::ViewDesc{}, RHI::SamplerDesc{},
                                                     RHI::HeapSlotLifetime::FrameTransient);
        ASSERT_TRUE(view.IsValid());
        ASSERT_TRUE(heap.OffsetOf(view).IsValid());
        EXPECT_EQ(heap.LifetimeOf(view), RHI::HeapSlotLifetime::FrameTransient);

        heap.ResetFrameTransients();

        // ADR 0011 §1.2 says a transient offset "held past the point of write is
        // ALREADY stale — not 'might go stale'". Nothing can enforce that by
        // review; this makes it a detectable, cheap CPU-side failure instead of
        // a sample from whatever inherits the ring slot next frame.
        EXPECT_FALSE(heap.OffsetOf(view).IsValid());
        EXPECT_FALSE(heap.IsLive(view));
    }

    TEST_F(HeapFixture, TransientRingReusesTheSameSlotsEveryFrame)
    {
        SetUpHeap(/*persistent*/ 8u, /*transient*/ 4u);
        auto& heap = RHI::DescriptorHeap::Get();

        const RHI::ResourceHandle resource = MakeResource(44u);

        const RHI::ViewHandle frame1 = heap.CreateView(resource, RHI::ViewDesc{}, RHI::SamplerDesc{},
                                                       RHI::HeapSlotLifetime::FrameTransient);
        heap.ResetFrameTransients();
        const RHI::ViewHandle frame2 = heap.CreateView(resource, RHI::ViewDesc{}, RHI::SamplerDesc{},
                                                       RHI::HeapSlotLifetime::FrameTransient);

        ASSERT_TRUE(frame1.IsValid());
        ASSERT_TRUE(frame2.IsValid());
        // A ring, not a freelist: the cursor resets, so frame N and frame N+1
        // hand out the same indices in the same order. That determinism is what
        // makes a heap capture comparable between frames.
        EXPECT_EQ(frame1.Index, frame2.Index);
        EXPECT_NE(frame1.Generation, frame2.Generation);
    }

    // -------------------------------------------------------------------------
    // 3. Aliasing produces two offsets onto one object — by design.
    // -------------------------------------------------------------------------
    TEST_F(HeapFixture, AliasedResourceHoldsTwoOffsetsInOneFrame)
    {
        SetUpHeap();
        auto& heap = RHI::DescriptorHeap::Get();

        // One physical object, two logical resources — what TransientPool does
        // when two graph resources have disjoint lifetimes, and what
        // WriteNewVersion does when it renames one physical resource.
        const RHI::ResourceHandle physical = MakeResource(55u);

        const RHI::ViewHandle versionA = heap.CreateView(physical, RHI::ViewDesc{}, RHI::SamplerDesc{},
                                                         RHI::HeapSlotLifetime::FrameTransient);
        const RHI::ViewHandle versionB = heap.CreateView(physical, RHI::ViewDesc{}, RHI::SamplerDesc{},
                                                         RHI::HeapSlotLifetime::FrameTransient);

        ASSERT_TRUE(versionA.IsValid());
        ASSERT_TRUE(versionB.IsValid());

        const RHI::HeapOffset offsetA = heap.OffsetOf(versionA);
        const RHI::HeapOffset offsetB = heap.OffsetOf(versionB);
        ASSERT_TRUE(offsetA.IsValid());
        ASSERT_TRUE(offsetB.IsValid());

        // NOT a bug. ADR 0011 §1.2: "a version rename gets a second offset
        // pointing at the same physical object. That is not a bug, it is the
        // correct model, and it makes the alias VISIBLE in the heap (two
        // offsets, one object) instead of invisible."
        EXPECT_NE(offsetA.Value, offsetB.Value);
        EXPECT_TRUE(heap.OffsetOf(versionA).IsValid()) << "Both must stay live simultaneously — that is the point.";
        EXPECT_TRUE(heap.OffsetOf(versionB).IsValid());
    }

    // -------------------------------------------------------------------------
    // 4. One resource, many views — why ViewHandle is a separate type.
    // -------------------------------------------------------------------------
    TEST_F(HeapFixture, OneResourceTwoViewsTwoOffsets)
    {
        SetUpHeap();
        auto& heap = RHI::DescriptorHeap::Get();

        const RHI::ResourceHandle depthArray = MakeResource(66u);

        // The engine's existing proof of the one-to-many relationship: one depth
        // array read both as a hardware-comparison shadow sampler and as raw
        // depth for the PCSS blocker search. Today that costs a whole second GL
        // texture object (CreateDepthArrayCompareOffViewHandle); under the heap
        // it is two views over one resource.
        RHI::ViewDesc compareOn;
        compareOn.Resource = depthArray;
        compareOn.DepthCompare = true;

        RHI::ViewDesc compareOff;
        compareOff.Resource = depthArray;
        compareOff.DepthCompare = false;

        RHI::SamplerDesc shadowSampler;
        shadowSampler.Compare = RHI::CompareOp::LessOrEqual;

        const RHI::ViewHandle shadowView =
            heap.CreateView(depthArray, compareOn, shadowSampler, RHI::HeapSlotLifetime::Persistent);
        const RHI::ViewHandle rawView =
            heap.CreateView(depthArray, compareOff, shadowSampler, RHI::HeapSlotLifetime::Persistent);

        ASSERT_TRUE(shadowView.IsValid());
        ASSERT_TRUE(rawView.IsValid());
        EXPECT_NE(heap.OffsetOf(shadowView).Value, heap.OffsetOf(rawView).Value);

        // Destroying one must not disturb the other. A resource-owned slot would
        // have made this impossible to express at all.
        heap.DestroyView(shadowView);
        EXPECT_FALSE(heap.OffsetOf(shadowView).IsValid());
        EXPECT_TRUE(heap.OffsetOf(rawView).IsValid());
    }

    // -------------------------------------------------------------------------
    // GetOrCreateView — the entry point a render pass actually calls.
    //
    // Every test above uses CreateView, which always mints. The memoising
    // wrapper carries the behaviour that matters in production: a pass runs
    // every frame, so if the persistent case did not memoise it would drain the
    // heap in seconds AND hand each frame a different offset, destroying the
    // stability ADR 0011 §1.2 calls "the performance argument for bindless".
    // -------------------------------------------------------------------------
    TEST_F(HeapFixture, GetOrCreateViewMemoisesPersistentViewsPerResourceSamplerAndCompareMode)
    {
        SetUpHeap();
        auto& heap = RHI::DescriptorHeap::Get();

        const RHI::ResourceHandle resource = MakeResource(201u);

        RHI::SamplerDesc samplerA;
        RHI::SamplerDesc samplerB;
        samplerB.MinFilter = RHI::Filter::Nearest;

        RHI::ViewDesc compareOn;
        RHI::ViewDesc compareOff;
        compareOff.DepthCompare = false;

        const RHI::ViewHandle first =
            heap.GetOrCreateView(resource, compareOn, samplerA, RHI::HeapSlotLifetime::Persistent);
        const RHI::ViewHandle again =
            heap.GetOrCreateView(resource, compareOn, samplerA, RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(first.IsValid());
        EXPECT_EQ(first, again) << "The same (resource, sampler, compare) triple must return the SAME view.";

        const RHI::ViewHandle otherSampler =
            heap.GetOrCreateView(resource, compareOn, samplerB, RHI::HeapSlotLifetime::Persistent);
        EXPECT_NE(first, otherSampler) << "Different sampler state is a different view.";

        const RHI::ViewHandle otherCompare =
            heap.GetOrCreateView(resource, compareOff, samplerA, RHI::HeapSlotLifetime::Persistent);
        EXPECT_NE(first, otherCompare) << "Compare-on and compare-off are different views of one resource.";
    }

    TEST_F(HeapFixture, GetOrCreateViewNeverMemoisesTransients)
    {
        SetUpHeap();
        auto& heap = RHI::DescriptorHeap::Get();

        const RHI::ResourceHandle resource = MakeResource(202u);
        const RHI::ViewHandle a = heap.GetOrCreateView(resource, {}, {}, RHI::HeapSlotLifetime::FrameTransient);
        const RHI::ViewHandle b = heap.GetOrCreateView(resource, {}, {}, RHI::HeapSlotLifetime::FrameTransient);

        ASSERT_TRUE(a.IsValid());
        ASSERT_TRUE(b.IsValid());
        // Two acquisitions of one physical object in one frame MUST get two
        // offsets — that is how WriteNewVersion aliasing becomes visible in the
        // heap instead of needing a mid-frame rewrite.
        EXPECT_NE(a, b);
        EXPECT_NE(heap.OffsetOf(a).Value, heap.OffsetOf(b).Value);
    }

    TEST_F(HeapFixture, GetOrCreateViewDoesNotServeAStaleCacheHitAfterTheViewDies)
    {
        SetUpHeap();
        auto& heap = RHI::DescriptorHeap::Get();

        const RHI::ResourceHandle resource = MakeResource(203u);
        const RHI::ViewHandle first = heap.GetOrCreateView(resource, {}, {}, RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(first.IsValid());

        // Nothing tells the cache a view was destroyed, so the entry survives.
        // The lookup must revalidate and fall through to a fresh mint rather
        // than hand back a handle whose slot may now belong to someone else.
        heap.DestroyView(first);

        const RHI::ViewHandle second = heap.GetOrCreateView(resource, {}, {}, RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(second.IsValid());
        EXPECT_NE(first, second);
        EXPECT_FALSE(heap.OffsetOf(first).IsValid());
        EXPECT_TRUE(heap.OffsetOf(second).IsValid());
    }

    TEST_F(HeapFixture, EvictingAStaleCacheEntryIsNotCountedAsAStaleOffsetRejection)
    {
        SetUpHeap();
        auto& heap = RHI::DescriptorHeap::Get();

        const RHI::ResourceHandle resource = MakeResource(205u);
        const RHI::ViewHandle first = heap.GetOrCreateView(resource, {}, {}, RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(first.IsValid());
        heap.DestroyView(first);

        // The cache still holds the dead entry; the next lookup must evict it.
        // That eviction is internal housekeeping, NOT a caller presenting a stale
        // handle — and StaleOffsetRejections is documented as the signal for the
        // latter, "the moment a cached offset outlived its view". Folding
        // evictions in would make the number unusable for the one job it has.
        const u64 rejectionsBefore = heap.GetStats().StaleOffsetRejections;
        const RHI::ViewHandle second = heap.GetOrCreateView(resource, {}, {}, RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(second.IsValid());
        EXPECT_EQ(heap.GetStats().StaleOffsetRejections, rejectionsBefore)
            << "A cache eviction must not be charged to the caller-facing rejection counter.";

        // …while a real caller presenting the dead handle still is.
        EXPECT_FALSE(heap.OffsetOf(first).IsValid());
        EXPECT_GT(heap.GetStats().StaleOffsetRejections, rejectionsBefore);
    }

    TEST_F(HeapFixture, RepeatedGetOrCreateViewDoesNotLeakSamplerSlots)
    {
        SetUpHeap();
        auto& heap = RHI::DescriptorHeap::Get();

        const RHI::ResourceHandle resource = MakeResource(204u);
        for (int i = 0; i < 32; ++i)
        {
            ASSERT_TRUE(heap.GetOrCreateView(resource, {}, {}, RHI::HeapSlotLifetime::Persistent).IsValid());
        }

        // The lookup acquires a sampler slot just to read its index and must
        // release it again; its own implementation comment warns the balance is
        // leak-prone, so pin it. One distinct SamplerDesc means one slot no
        // matter how many times a pass asks.
        EXPECT_EQ(heap.GetStats().SamplerSlotsLive, 1u);
        EXPECT_EQ(heap.GetStats().PersistentLive, 1u) << "Memoised, so only ONE slot may have been consumed.";
    }

    // -------------------------------------------------------------------------
    // Sampler heap — ADR 0011 §1.2a's dedup, which has no GL counterpart.
    // -------------------------------------------------------------------------
    TEST_F(HeapFixture, IdenticalSamplerStateSharesOneSamplerSlot)
    {
        SetUpHeap();
        auto& heap = RHI::DescriptorHeap::Get();

        RHI::SamplerDesc shared;
        shared.MinFilter = RHI::Filter::Linear;
        shared.AddressU = RHI::AddressMode::Repeat;

        RHI::SamplerDesc different = shared;
        different.AddressU = RHI::AddressMode::ClampToEdge;

        const RHI::ViewHandle a = heap.CreateView(MakeResource(71u), RHI::ViewDesc{}, shared,
                                                  RHI::HeapSlotLifetime::Persistent);
        const RHI::ViewHandle b = heap.CreateView(MakeResource(72u), RHI::ViewDesc{}, shared,
                                                  RHI::HeapSlotLifetime::Persistent);
        const RHI::ViewHandle c = heap.CreateView(MakeResource(73u), RHI::ViewDesc{}, different,
                                                  RHI::HeapSlotLifetime::Persistent);

        ASSERT_TRUE(a.IsValid());
        ASSERT_TRUE(b.IsValid());
        ASSERT_TRUE(c.IsValid());

        // The resource-heap offsets are all distinct (one per view)…
        EXPECT_NE(heap.OffsetOf(a).Value, heap.OffsetOf(b).Value);
        // …but the sampler heap is a SECOND heap, deduplicated by value. This is
        // the whole argument in §1.2a: a few configurations serve hundreds of
        // textures, so one slot per texture would size the sampler heap by
        // texture count.
        EXPECT_EQ(heap.SamplerOffsetOf(a).Value, heap.SamplerOffsetOf(b).Value);
        EXPECT_NE(heap.SamplerOffsetOf(a).Value, heap.SamplerOffsetOf(c).Value);

        EXPECT_EQ(heap.GetStats().SamplerSlotsLive, 2u) << "Three views, two distinct sampler configurations.";
    }

    // -------------------------------------------------------------------------
    // Poison — the instrument that makes a use-after-free deterministic.
    // -------------------------------------------------------------------------
    TEST_F(HeapFixture, PoisonOnFreeOverwritesTheSlotInThePublishedTable)
    {
        SetUpHeap(/*persistent*/ 8u, /*transient*/ 4u, /*poison*/ true);
        auto& heap = RHI::DescriptorHeap::Get();

        const RHI::ViewHandle view = heap.CreateView(MakeResource(81u), RHI::ViewDesc{}, RHI::SamplerDesc{},
                                                     RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(view.IsValid());
        const u32 slot = heap.OffsetOf(view).Value;

        heap.Flush();
        ASSERT_GE(Backend.Uploads, 1u);
        // The live descriptor really was published, so the assertion below is
        // about poison overwriting something rather than about nothing ever
        // having been there.
        ASSERT_GE(slot, Backend.LastUploadFirstSlot);
        ASSERT_LT(slot - Backend.LastUploadFirstSlot, Backend.LastUpload.size())
            << "The published range must actually contain the slot under test.";
        EXPECT_NE(Backend.LastUpload[slot - Backend.LastUploadFirstSlot], FakeHeapBackend::kNull);

        heap.DestroyView(view);
        heap.Flush();

        // Without poison, a use-after-free reads whatever the previous tenant
        // left — which LIFO slot reuse hides in steady state, exactly as the
        // transient POOL hides the same class of bug
        // (docs/agent-rules/render-graph-transient-aliasing.md). With it, the
        // read is deterministically nothing.
        ASSERT_GE(slot, Backend.LastUploadFirstSlot);
        ASSERT_LT(slot - Backend.LastUploadFirstSlot, Backend.LastUpload.size());
        EXPECT_EQ(Backend.LastUpload[slot - Backend.LastUploadFirstSlot], FakeHeapBackend::kNull)
            << "A freed slot must hold the backend's null descriptor, not a zero that could also mean 'never written'.";
        EXPECT_GE(heap.GetStats().SlotsPoisoned, 1u);
    }

    // -------------------------------------------------------------------------
    // Invalidation — the hazard the STABLE resource identity creates.
    // -------------------------------------------------------------------------
    TEST_F(HeapFixture, InvalidateResourceRefetchesEveryViewOfThatResource)
    {
        SetUpHeap();
        auto& heap = RHI::DescriptorHeap::Get();

        const RHI::ResourceHandle resource = MakeResource(91u);
        const RHI::ViewHandle viewA = heap.CreateView(resource, RHI::ViewDesc{}, RHI::SamplerDesc{},
                                                      RHI::HeapSlotLifetime::Persistent);
        RHI::SamplerDesc other;
        other.MinFilter = RHI::Filter::Nearest;
        const RHI::ViewHandle viewB = heap.CreateView(resource, RHI::ViewDesc{}, other,
                                                      RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(viewA.IsValid());
        ASSERT_TRUE(viewB.IsValid());

        const u32 acquiresBefore = Backend.Acquires;
        heap.Flush();
        const std::vector<u64> beforeTable = Backend.LastUpload;

        // An in-place reload recreates the storage on the SAME C++ object, so
        // ResourceRegistry deliberately KEEPS the handle valid — that is what
        // makes caching a ResourceHandle safe. The descriptors do not inherit
        // that safety: an ARB_bindless_texture handle names the object that was
        // just deleted, and the view's own generation has not moved, so
        // OffsetOf cannot detect it. Exact mirror of the Phase 2 slice-6
        // finding, where stable identity made the bind cache SKIP a needed bind.
        heap.InvalidateResource(resource);

        EXPECT_EQ(Backend.Acquires, acquiresBefore + 2u) << "Both views of the resource must be re-fetched.";
        // Both handles stay valid — invalidation replaces the descriptor, it
        // does not retire the view. A pass holding a persistent offset keeps it.
        EXPECT_TRUE(heap.OffsetOf(viewA).IsValid());
        EXPECT_TRUE(heap.OffsetOf(viewB).IsValid());

        heap.Flush();
        ASSERT_EQ(Backend.LastUpload.size(), beforeTable.size());
        EXPECT_NE(Backend.LastUpload, beforeTable) << "The published table must carry the NEW descriptors.";
    }

    TEST_F(HeapFixture, InvalidateResourceIgnoresViewsOfOtherResources)
    {
        SetUpHeap();
        auto& heap = RHI::DescriptorHeap::Get();

        const RHI::ResourceHandle reloaded = MakeResource(101u);
        const RHI::ResourceHandle untouched = MakeResource(102u);
        ASSERT_TRUE(heap.CreateView(reloaded, RHI::ViewDesc{}, RHI::SamplerDesc{},
                                    RHI::HeapSlotLifetime::Persistent)
                        .IsValid());
        ASSERT_TRUE(heap.CreateView(untouched, RHI::ViewDesc{}, RHI::SamplerDesc{},
                                    RHI::HeapSlotLifetime::Persistent)
                        .IsValid());

        const u32 acquiresBefore = Backend.Acquires;
        heap.InvalidateResource(reloaded);
        EXPECT_EQ(Backend.Acquires, acquiresBefore + 1u);
    }

    // -------------------------------------------------------------------------
    // Failure modes: every one of these must degrade to "use the old path",
    // never to a live handle whose offset is wrong.
    // -------------------------------------------------------------------------
    TEST_F(HeapFixture, ViewOfADeadResourceProducesNoHandle)
    {
        SetUpHeap();
        auto& heap = RHI::DescriptorHeap::Get();

        const RHI::ResourceHandle resource = MakeResource(111u);
        RHI::ResourceRegistry::Get().Unregister(resource);

        const RHI::ViewHandle view = heap.CreateView(resource, RHI::ViewDesc{}, RHI::SamplerDesc{},
                                                     RHI::HeapSlotLifetime::Persistent);
        EXPECT_FALSE(view.IsValid());
        EXPECT_EQ(Backend.Acquires, 0u) << "The heap must not even ask the backend for a dead resource's descriptor.";
    }

    TEST_F(HeapFixture, ABackendFailureReturnsTheSlotRatherThanPublishingPoison)
    {
        SetUpHeap();
        auto& heap = RHI::DescriptorHeap::Get();

        Backend.FailNextAcquire = true;
        const RHI::ViewHandle failed = heap.CreateView(MakeResource(121u), RHI::ViewDesc{}, RHI::SamplerDesc{},
                                                       RHI::HeapSlotLifetime::Persistent);
        EXPECT_FALSE(failed.IsValid())
            << "A live view whose offset reads as black is much harder to diagnose than no view at all.";

        // The slot must come back, or a run of transient failures would leak the
        // persistent region away over a session.
        const RHI::ViewHandle ok = heap.CreateView(MakeResource(122u), RHI::ViewDesc{}, RHI::SamplerDesc{},
                                                   RHI::HeapSlotLifetime::Persistent);
        EXPECT_TRUE(ok.IsValid());
        EXPECT_EQ(heap.GetStats().PersistentLive, 1u);
    }

    TEST_F(HeapFixture, TransientRingOverflowFallsBackInsteadOfGrowing)
    {
        SetUpHeap(/*persistent*/ 8u, /*transient*/ 2u);
        auto& heap = RHI::DescriptorHeap::Get();

        const RHI::ResourceHandle resource = MakeResource(131u);
        EXPECT_TRUE(heap.CreateView(resource, {}, {}, RHI::HeapSlotLifetime::FrameTransient).IsValid());
        EXPECT_TRUE(heap.CreateView(resource, {}, {}, RHI::HeapSlotLifetime::FrameTransient).IsValid());

        // Growing the ring would silently reintroduce mid-frame offset
        // rewriting, which is the exact thing the ring exists to prevent. An
        // overflow is a sizing problem, and the honest response is to say so and
        // let the caller bind the old way.
        EXPECT_FALSE(heap.CreateView(resource, {}, {}, RHI::HeapSlotLifetime::FrameTransient).IsValid());
        EXPECT_EQ(heap.GetStats().TransientOverflows, 1u);
    }

    TEST_F(HeapFixture, DestroyingATransientViewByHandIsRefused)
    {
        SetUpHeap();
        auto& heap = RHI::DescriptorHeap::Get();

        const RHI::ViewHandle view = heap.CreateView(MakeResource(141u), {}, {},
                                                     RHI::HeapSlotLifetime::FrameTransient);
        ASSERT_TRUE(view.IsValid());

        // The ring is a cursor, not a freelist, so a hand-destroy would return
        // the index to nobody while retiring a generation the frame boundary is
        // about to retire anyway. Refused and counted rather than silently
        // half-honoured.
        const u64 rejectionsBefore = heap.GetStats().StaleOffsetRejections;
        heap.DestroyView(view);
        EXPECT_TRUE(heap.OffsetOf(view).IsValid()) << "The transient view must survive a refused hand-destroy.";
        EXPECT_GT(heap.GetStats().StaleOffsetRejections, rejectionsBefore)
            << "…and the refusal must be COUNTED, which is what the comment claims makes it non-silent.";
    }

    // -------------------------------------------------------------------------
    // The disabled path — every machine below the feature floor runs in it, and
    // so does every headless test. It must be inert, not broken.
    // -------------------------------------------------------------------------
    TEST_F(HeapFixture, AnUnsupportedBackendLeavesTheHeapPermanentlyDisabled)
    {
        Backend.Supported = false;

        RHI::HeapDesc desc;
        desc.ResourceSlotCapacity = 8u;
        desc.SamplerSlotCapacity = 4u;
        desc.FrameTransientRingSlots = 4u;
        RHI::DescriptorHeap::Get().Initialize(desc, &Backend);

        auto& heap = RHI::DescriptorHeap::Get();
        EXPECT_FALSE(heap.IsEnabled());

        heap.SetEnabled(true);
        EXPECT_FALSE(heap.IsEnabled()) << "The toggle must not be able to override a missing extension.";

        EXPECT_FALSE(heap.CreateView(MakeResource(151u), {}, {}, RHI::HeapSlotLifetime::Persistent).IsValid());
        EXPECT_EQ(Backend.Acquires, 0u);

        // Inert, not crashing: every one of these runs on a machine without the
        // extension, every frame.
        heap.ResetFrameTransients();
        heap.Flush();
        heap.InvalidateResource(MakeResource(152u));
        EXPECT_EQ(Backend.Uploads, 0u);
        EXPECT_EQ(Backend.Binds, 0u);
    }

    TEST_F(HeapFixture, OffsetOfARetiredViewIsInvalidAfterShutdown)
    {
        SetUpHeap();
        auto& heap = RHI::DescriptorHeap::Get();

        const RHI::ViewHandle view = heap.CreateView(MakeResource(161u), {}, {},
                                                     RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(view.IsValid());

        heap.Shutdown();

        // A handle held across a device teardown must not resolve into the next
        // device's slots — the same reason ResourceRegistry::Clear advances
        // every generation.
        EXPECT_FALSE(heap.OffsetOf(view).IsValid());
        EXPECT_EQ(Backend.Releases, 1u) << "Shutdown must drop residency, or the texture stays immutable forever.";
    }

    // -------------------------------------------------------------------------
    // The free function Phase 1 declared. Implementing the DECLARED vocabulary
    // rather than the one the prose describes is the Phase 2 step 2 lesson
    // (`RHI::MemoryResidency` was nearly reinvented as `BufferUsage`), so it is
    // worth a test that the declared spelling is the one that works.
    // -------------------------------------------------------------------------
    TEST_F(HeapFixture, TheDeclaredFreeFunctionAnswersTheSameOffsetAsTheMember)
    {
        SetUpHeap();
        auto& heap = RHI::DescriptorHeap::Get();

        const RHI::ViewHandle view = heap.CreateView(MakeResource(171u), {}, {},
                                                     RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(view.IsValid());

        EXPECT_EQ(RHI::OffsetOf(view), heap.OffsetOf(view));
        EXPECT_FALSE(RHI::OffsetOf(RHI::ViewHandle{}).IsValid());
    }

    // -------------------------------------------------------------------------
    // Publication. The binding must be re-established even on a clean frame —
    // an unbound heap does not fail loudly, it hands every shader a valid index
    // into whatever buffer is at that binding point.
    // -------------------------------------------------------------------------
    TEST_F(HeapFixture, FlushBindsEveryFrameButUploadsOnlyWhenDirty)
    {
        SetUpHeap();
        auto& heap = RHI::DescriptorHeap::Get();

        ASSERT_TRUE(heap.CreateView(MakeResource(181u), {}, {}, RHI::HeapSlotLifetime::Persistent).IsValid());

        heap.Flush();
        EXPECT_EQ(Backend.Uploads, 1u);
        EXPECT_EQ(Backend.Binds, 1u);

        heap.Flush();
        EXPECT_EQ(Backend.Uploads, 1u) << "Nothing changed, so nothing should be uploaded.";
        EXPECT_EQ(Backend.Binds, 2u) << "…but the binding must be re-established regardless.";
    }
} // namespace OloEngine::Tests
