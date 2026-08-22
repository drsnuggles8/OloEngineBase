// OLO_TEST_LAYER: plumbing
// =============================================================================
// RHIResourceRegistryTest.cpp
//
// Contract tests for the generation-checked handle mint (issue #691
// step 3, ADR 0011 §1.1).
//
// THE TEST THAT JUSTIFIES THE WHOLE DESIGN is
// `RecycledIndexWithStaleGenerationIsRejected`. Before this change the boundary
// currency was a bare GL name, and GL recycles names: delete texture A, create
// texture B, and B can be handed A's name. Every `GetRendererID()` comparison
// in the engine — including `Texture::operator==` — then reports A == B. A
// generation makes that detectable; a u32 structurally cannot. If that test is
// ever weakened, the rest of the identity currency buys nothing.
//
// These are pure CPU tests: the registry stores an opaque u64 and never talks
// to a device, so they run headless with no GL context.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/RHI/RHIResources.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // The registry is a process-wide singleton, so a test must not assume it
        // starts empty — other tests (and the renderer itself, when a GL context
        // exists) register into it. Every assertion below is therefore relative
        // to handles this test minted itself, or to a delta of the counters.
        [[nodiscard]] auto Registry() -> RHI::ResourceRegistry&
        {
            return RHI::ResourceRegistry::Get();
        }
    } // namespace

    TEST(RHIResourceRegistry, RegisterMintsALiveResolvableHandle)
    {
        auto& registry = Registry();
        const auto handle = registry.Register(RHI::ResourceKind::Texture, 0xABCDu, RHI::Backend::OpenGL);

        EXPECT_TRUE(handle.IsValid());
        EXPECT_TRUE(registry.IsLive(handle));
        EXPECT_EQ(registry.ResolveNativeForBackend(handle), 0xABCDu);
        EXPECT_EQ(registry.KindOf(handle), RHI::ResourceKind::Texture);

        const auto tagged = registry.ResolveTaggedForBackend(handle);
        EXPECT_EQ(tagged.Value, 0xABCDu);
        EXPECT_EQ(tagged.Owner, RHI::Backend::OpenGL);

        registry.Unregister(handle);
    }

    TEST(RHIResourceRegistry, DefaultConstructedHandleIsNeitherValidNorLive)
    {
        auto& registry = Registry();
        constexpr RHI::ResourceHandle null{};

        EXPECT_FALSE(null.IsValid());
        EXPECT_FALSE(registry.IsLive(null));
        EXPECT_EQ(registry.ResolveNativeForBackend(null), 0u);
        EXPECT_EQ(registry.KindOf(null), RHI::ResourceKind::Unknown);
    }

    TEST(RHIResourceRegistry, UnregisterMakesTheHandleStale)
    {
        auto& registry = Registry();
        const auto handle = registry.Register(RHI::ResourceKind::Buffer, 77u, RHI::Backend::OpenGL);
        ASSERT_TRUE(registry.IsLive(handle));

        registry.Unregister(handle);

        EXPECT_FALSE(registry.IsLive(handle));
        // Still structurally valid — it is a well-formed handle naming a dead
        // object, which is exactly the state the generation exists to describe.
        EXPECT_TRUE(handle.IsValid());
        EXPECT_EQ(registry.ResolveNativeForBackend(handle), 0u);
    }

    // ==========================================================================
    // The load-bearing one. See the file header.
    // ==========================================================================
    TEST(RHIResourceRegistry, RecycledIndexWithStaleGenerationIsRejected)
    {
        auto& registry = Registry();

        // Same native name deliberately: this models GL handing a newly created
        // object the name it just freed, which is what makes the bare-u32
        // currency undetectably wrong.
        constexpr u64 kRecycledNativeName = 4242u;

        const auto first = registry.Register(RHI::ResourceKind::Texture, kRecycledNativeName, RHI::Backend::OpenGL);
        registry.Unregister(first);
        const auto second = registry.Register(RHI::ResourceKind::Texture, kRecycledNativeName, RHI::Backend::OpenGL);

        // The freelist is LIFO, so the second registration reuses the slot the
        // first just freed. That is the whole point — the index collides.
        ASSERT_EQ(first.Index, second.Index) << "Test precondition: the slot must actually be recycled";

        // ...and the generation is what keeps them apart.
        EXPECT_NE(first.Generation, second.Generation);
        EXPECT_NE(first, second) << "Two distinct objects sharing a recycled slot must not compare equal — "
                                    "this is the exact bug Texture::operator== had with bare renderer IDs";

        EXPECT_FALSE(registry.IsLive(first));
        EXPECT_TRUE(registry.IsLive(second));

        // The stale handle must NOT resolve to the recycled native name.
        EXPECT_EQ(registry.ResolveNativeForBackend(first), 0u);
        EXPECT_EQ(registry.ResolveNativeForBackend(second), kRecycledNativeName);

        registry.Unregister(second);
    }

    // The cache-fingerprint corollary of the test above, and the reason
    // RenderPipeline::ComputeBlackboardFingerprint hashes RHI::HashKey(handle)
    // rather than the raw id (issue #691).
    //
    // The concrete bug: DDGIProbeUpdatePass::EnsureResources calls
    // DestroyResources() BEFORE creating the replacement atlases, so every
    // attachment texture is freed first and GL is free to reissue the same
    // names. Hashing those names therefore could not observe a
    // Resolution/HitCacheTexels edit at all — the fingerprint never changed,
    // BuildFrameGraph was never rebuilt, and the render graph kept an import
    // whose Width/Height still described the old resolution (which is what
    // olo_render_list_targets then reported).
    //
    // Note this is a STRICTLY stronger claim than `first != second` above: a
    // fingerprint mixes a single integer, so it needs the *keyed* form to
    // differ, not merely the handles.
    TEST(RHIResourceRegistry, HashKeyDiffersAcrossADestroyRecreateThatReusesTheNativeName)
    {
        auto& registry = Registry();

        constexpr u64 kRecycledNativeName = 909090u;

        const auto before = registry.Register(RHI::ResourceKind::Texture, kRecycledNativeName, RHI::Backend::OpenGL);
        registry.Unregister(before);
        const auto after = registry.Register(RHI::ResourceKind::Texture, kRecycledNativeName, RHI::Backend::OpenGL);

        ASSERT_EQ(before.Index, after.Index) << "Test precondition: the slot must actually be recycled";
        ASSERT_EQ(registry.ResolveNativeForBackend(after), kRecycledNativeName)
            << "Test precondition: the recreate must genuinely reuse the freed native name — "
               "that is the case a raw-id hash cannot see";

        EXPECT_NE(RHI::HashKey(before), RHI::HashKey(after))
            << "A cache keyed on 'did this GPU object change' must observe a destroy/recreate even "
               "when the driver reissues the name. If this ever compares equal, every fingerprint "
               "built from HashKey silently stops invalidating.";

        registry.Unregister(after);
    }

    TEST(RHIResourceRegistry, UpdateNativeKeepsIdentityAcrossAnInPlaceReload)
    {
        // Models texture hot-reload (issue #544): the C++ object survives,
        // its GL storage is recreated, and GL may hand the new storage a
        // different name. The handle must stay live and follow the object.
        auto& registry = Registry();
        const auto handle = registry.Register(RHI::ResourceKind::Texture, 100u, RHI::Backend::OpenGL);

        registry.UpdateNative(handle, 200u);

        EXPECT_TRUE(registry.IsLive(handle));
        EXPECT_EQ(registry.ResolveNativeForBackend(handle), 200u);

        registry.Unregister(handle);

        // ...and it is still generation-checked afterwards.
        registry.UpdateNative(handle, 300u);
        EXPECT_FALSE(registry.IsLive(handle));
        EXPECT_EQ(registry.ResolveNativeForBackend(handle), 0u);
    }

    // A recreate path zeroes its native name transiently between destroying the
    // old object and creating the new one, so ScopedResourceHandle::Sync sees
    // `0` in the middle of a sequence that must PRESERVE identity. An earlier
    // version treated any zero as a release and retired the handle here, which
    // broke in-place hot-reload — caught first by the GL-gated
    // RHIHandleNativeIdentityTest, and pinned headlessly here so the fast suite
    // catches a regression without needing a device.
    TEST(RHIResourceRegistry, SyncTreatsATransientZeroAsRecreateNotRelease)
    {
        auto& registry = Registry();

        RHI::ScopedResourceHandle holder;
        holder.Sync(RHI::ResourceKind::Texture, 100u, RHI::Backend::OpenGL);
        const auto before = holder.Get();
        ASSERT_TRUE(before.IsValid());

        // The exact shape of OpenGLTexture2D::InvalidateImpl's reload:
        // release the old name, then create a new one.
        holder.Sync(RHI::ResourceKind::Texture, 0u, RHI::Backend::OpenGL);
        holder.Sync(RHI::ResourceKind::Texture, 200u, RHI::Backend::OpenGL);

        EXPECT_EQ(holder.Get(), before) << "A recreate must not change identity — materials cache "
                                           "the handle alongside their Ref<T> (ADR 0011 §1.2)";
        EXPECT_TRUE(registry.IsLive(before));
        EXPECT_EQ(registry.ResolveNativeForBackend(before), 200u)
            << "...and the preserved handle must resolve to the NEW native name";
    }

    TEST(RHIResourceRegistry, SyncMintsNothingForAnObjectThatNeverGotAResource)
    {
        // Construction-failure paths assign 0 before any create. There is no
        // object to name, so no identity should be minted — a consumer asking
        // `handle.IsValid()` must see "no resource".
        RHI::ScopedResourceHandle holder;
        holder.Sync(RHI::ResourceKind::Texture, 0u, RHI::Backend::OpenGL);
        EXPECT_FALSE(holder.Get().IsValid());
    }

    TEST(RHIResourceRegistry, ScopedHandleRetiresItsEntryOnDestruction)
    {
        auto& registry = Registry();
        RHI::ResourceHandle observed;
        {
            RHI::ScopedResourceHandle holder;
            holder.Sync(RHI::ResourceKind::Buffer, 55u, RHI::Backend::OpenGL);
            observed = holder.Get();
            ASSERT_TRUE(registry.IsLive(observed));
        }
        // RAII is now the ONLY thing that retires an identity, since Sync no
        // longer does — so this is the assertion that keeps handles from
        // outliving their objects.
        EXPECT_FALSE(registry.IsLive(observed));
        EXPECT_EQ(registry.ResolveNativeForBackend(observed), 0u);
    }

    TEST(RHIResourceRegistry, StaleResolveIsCounted)
    {
        auto& registry = Registry();
        const auto before = registry.GetStats().StaleRejections;

        const auto handle = registry.Register(RHI::ResourceKind::Framebuffer, 5u, RHI::Backend::OpenGL);
        registry.Unregister(handle);

        EXPECT_EQ(registry.ResolveNativeForBackend(handle), 0u);

        // A null handle is not a stale one — it is the ordinary "no resource"
        // value and must not inflate the diagnostic counter.
        EXPECT_EQ(registry.ResolveNativeForBackend(RHI::ResourceHandle{}), 0u);

        EXPECT_EQ(registry.GetStats().StaleRejections, before + 1u);
    }

    TEST(RHIResourceRegistry, DoubleUnregisterIsCountedAndDoesNotCorruptTheFreelist)
    {
        auto& registry = Registry();
        const auto before = registry.GetStats().StaleUnregisters;

        const auto handle = registry.Register(RHI::ResourceKind::VertexArray, 9u, RHI::Backend::OpenGL);
        registry.Unregister(handle);
        registry.Unregister(handle);

        EXPECT_EQ(registry.GetStats().StaleUnregisters, before + 1u);

        // If the double-unregister had pushed the index twice, these two
        // registrations would share a slot and the first would be reported dead.
        const auto a = registry.Register(RHI::ResourceKind::VertexArray, 10u, RHI::Backend::OpenGL);
        const auto b = registry.Register(RHI::ResourceKind::VertexArray, 11u, RHI::Backend::OpenGL);
        EXPECT_NE(a.Index, b.Index);
        EXPECT_TRUE(registry.IsLive(a));
        EXPECT_TRUE(registry.IsLive(b));
        EXPECT_EQ(registry.ResolveNativeForBackend(a), 10u);
        EXPECT_EQ(registry.ResolveNativeForBackend(b), 11u);

        registry.Unregister(a);
        registry.Unregister(b);
    }

    TEST(RHIResourceRegistry, HandlesAreUniqueAcrossManyRegistrations)
    {
        auto& registry = Registry();

        constexpr u32 kCount = 4096u; // spans more than one 1024-slot chunk
        std::vector<RHI::ResourceHandle> handles;
        handles.reserve(kCount);
        for (u32 i = 0u; i < kCount; ++i)
            handles.push_back(registry.Register(RHI::ResourceKind::Buffer, i + 1u, RHI::Backend::OpenGL));

        std::unordered_set<u64> seen;
        seen.reserve(kCount);
        for (u32 i = 0u; i < kCount; ++i)
        {
            ASSERT_TRUE(handles[i].IsValid()) << "registration " << i << " failed";
            const auto key = (static_cast<u64>(handles[i].Generation) << 32u) | static_cast<u64>(handles[i].Index);
            EXPECT_TRUE(seen.insert(key).second) << "duplicate handle at registration " << i;
            EXPECT_EQ(registry.ResolveNativeForBackend(handles[i]), i + 1u);
        }

        for (const auto handle : handles)
            registry.Unregister(handle);
    }

    TEST(RHIResourceRegistry, DebugEscapeHatchResolvesThroughTheSameRegistry)
    {
        auto& registry = Registry();
        const auto handle = registry.Register(RHI::ResourceKind::Texture, 0xF00Du, RHI::Backend::OpenGL);

        const auto native = RHI::GetNativeHandleForDebug(handle);
        EXPECT_EQ(native.Value, 0xF00Du);
        EXPECT_EQ(native.Owner, RHI::Backend::OpenGL);

        registry.Unregister(handle);

        // The escape hatch is generation-checked too — a debug tool holding a
        // handle across a resource's death reports "gone", not a recycled name.
        const auto stale = RHI::GetNativeHandleForDebug(handle);
        EXPECT_EQ(stale.Value, 0u);
        EXPECT_EQ(stale.Owner, RHI::Backend::None);
    }

    TEST(RHIResourceRegistry, ResourceAndViewHandlesAreMutuallyNonConvertible)
    {
        // Guards the property RHITypes.h claims for the tag-templated Handle:
        // ViewHandle is declared but not minted yet (see ADR 0011's
        // "Amendments from the call-site sweep"), and when it is minted, a
        // resource id must not be silently passable where a view id is wanted.
        static_assert(!std::is_convertible_v<RHI::ResourceHandle, RHI::ViewHandle>);
        static_assert(!std::is_convertible_v<RHI::ViewHandle, RHI::ResourceHandle>);
        SUCCEED();
    }

    TEST(RHIResourceRegistry, ConcurrentRegisterAndResolveStayConsistent)
    {
        // Registration happens on whichever thread creates a resource, and the
        // backend resolves on the render thread; the registry promises
        // lock-free reads alongside serialised writes. This is a smoke test for
        // that promise, not a proof — but a torn read would show up as a native
        // value that never belonged to the handle.
        auto& registry = Registry();

        constexpr u32 kThreads = 4u;
        constexpr u32 kPerThread = 512u;
        std::atomic<u32> mismatches{ 0u };

        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (u32 t = 0u; t < kThreads; ++t)
        {
            threads.emplace_back([&registry, &mismatches, t]()
                                 {
                std::vector<RHI::ResourceHandle> mine;
                mine.reserve(kPerThread);
                for (u32 i = 0u; i < kPerThread; ++i)
                {
                    const u64 native = (static_cast<u64>(t) << 32u) | (i + 1u);
                    const auto handle = registry.Register(RHI::ResourceKind::Buffer, native, RHI::Backend::OpenGL);
                    if (!handle.IsValid() || registry.ResolveNativeForBackend(handle) != native)
                        mismatches.fetch_add(1u, std::memory_order_relaxed);
                    mine.push_back(handle);
                }
                for (const auto handle : mine)
                {
                    if (registry.ResolveNativeForBackend(handle) == 0u)
                        mismatches.fetch_add(1u, std::memory_order_relaxed);
                    registry.Unregister(handle);
                } });
        }

        for (auto& thread : threads)
            thread.join();

        EXPECT_EQ(mismatches.load(), 0u);
    }
} // namespace OloEngine::Tests
